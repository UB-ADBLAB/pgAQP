#include "aqp_pswrctl_node.h"

#include <executor/nodeAgg.h>
#include <nodes/nodeFuncs.h>
#include <nodes/makefuncs.h>
#include <nodes/execnodes.h>
#include <nodes/extensible.h>
#include <nodes/pathnodes.h>
#include <nodes/plannodes.h>
#include <nodes/print.h>
#include <utils/lsyscache.h>
#include <utils/timestamp.h>

#include <executor/executor.h>
#include <utils/tuplestore.h>
#include <miscadmin.h>

#include "aqp_planner.h"
#include "pg_explain.h"
#include "aqp_explain.h"
#include "aqp_pagg.h"
#include "aqp_math.h"

static Plan* aqp_plan_pswrctl_path(PlannerInfo *root,
                                   RelOptInfo *rel,
                                   CustomPath *best_path,
                                   List *tlist,
                                   List *clauses,
                                   List *custom_plans);
static void AQPPSWRControlPathPrivate_copy(ExtensibleNode *newnode_,
                                           const ExtensibleNode *from_);
static bool AQPPSWRControlPathPrivate_equal(const ExtensibleNode *newnode_,
                                            const ExtensibleNode *from_);
static void AQPPSWRControlPathPrivate_out(StringInfo str,
                                          const ExtensibleNode *from_);
static void AQPPSWRControlPathPrivate_read(ExtensibleNode *newnode_);
static void AQPPSWRControlPrivate_copy(ExtensibleNode *newnode_,
                                           const ExtensibleNode *from_);
static bool AQPPSWRControlPrivate_equal(const ExtensibleNode *newnode_,
                                            const ExtensibleNode *from_);
static void AQPPSWRControlPrivate_out(StringInfo str,
                                          const ExtensibleNode *from_);
static void AQPPSWRControlPrivate_read(ExtensibleNode *newnode_);
static Node *aqp_pswrctl_create_state(CustomScan *cscan);
static bool aqp_expr_contains_aggref(Node *node);
static bool aqp_expr_contains_aggref_walker(Node *node, void *ctx);
static AttrNumber aqp_add_distinct_expr_to_tlist(List **tlist, Expr *expr);
static void aqp_rewrite_pswrctl_tlist(
    List *dummy_tlist,
    List *orig_agg_tlist,
    List **agg_tlist,
    List **pswrctl_tlist);
static Node *aqp_rewrite_pswrctl_tlist_expr(
    Node *expr,
    List **agg_tlist);
static AttrNumber aqp_add_distinct_aggref_to_tlist(List **aggrefs,
                                                   Aggref *aggref);

static void aqp_pswrctl_begin(CustomScanState *node, EState *estate,
                              int eflags);
static TupleTableSlot *aqp_pswrctl_exec(CustomScanState *node);
static TupleTableSlot *aqp_pswrctl_emit_next_group(AQPPSWRControlState *state);
static void aqp_pswrctl_detach_group_slot(AQPPSWRControlState *state,
                                          TupleTableSlot *slot);
static void aqp_pswrctl_finalize_group_slot(AQPPSWRControlState *state,
                                            TupleTableSlot *slot);
static void aqp_pswrctl_end(CustomScanState *node);
static void aqp_pswrctl_rescan(CustomScanState *node);
static void aqp_pswrctl_explain(CustomScanState *node, List *ancestors,
                                ExplainState *es);
static void aqp_pswrctl_init_pswrctl_global_params(AQPPSWRControlState *state,
                                                   EState *estate);
static void aqp_pswrctl_mark_driver_subplan(AQPPSWRCtlInfo pswrctl_info,
                                            int driver_subplan_id);

static uint64 aqp_pswr_budget_spent(AQPPSWRControlState *state);
static uint64 aqp_pswr_remaining_budget(AQPPSWRControlState *state);
static void aqp_pswr_progressive_checkpoint_round_start(
    AQPPSWRControlState *state);
static void aqp_pswr_progressive_rollback_bad_rounds(
    AQPPSWRControlState *state);

static void aqp_pswr_progressive_init_current_phase(AQPPSWRControlState *state);
static void aqp_pswr_progressive_build_each_round(AQPPSWRControlState *state);
static bool aqp_pswr_progressive_check_ci_and_switch(AQPPSWRControlState *state);
static void aqp_pswr_progressive_enter_uniform(AQPPSWRControlState *state);
static void aqp_pswr_progressive_allocate_uniform_round(
    AQPPSWRControlState *state);
static uint64 aqp_pswr_estimate_uniform_remaining_sample_size(
    AQPPSWRControlState *state);

#define AQP_PSWR_MIN_STRATIFIED_PROBE_ROUNDS 3
#define AQP_PSWR_MAX_BAD_PROGRESSIVE_ROUNDS 3
#define AQP_PSWR_NEAR_TARGET_FACTOR 1.10    
#define AQP_PSWR_UNIFORM_COMPETITIVE_FACTOR 1.10      
#define AQP_PSWR_PHASE_REGRESSION_FACTOR 1.10      

static uint64
aqp_pswr_budget_spent(AQPPSWRControlState *state)
{
    return state->total_samples_fetched + state->discarded_probe_samples;
}

static uint64
aqp_pswr_remaining_budget(AQPPSWRControlState *state)
{
    uint64 spent = aqp_pswr_budget_spent(state);

    return state->sample_budget > spent ? state->sample_budget - spent : 0;
}

CustomPathMethods aqp_pswrctl_path_methods = {
    AQPPSWRControlPathName,
    aqp_plan_pswrctl_path,
    aqp_reparameterize_custom_path_by_child /* dummy that returns NULL */
};

static ExtensibleNodeMethods aqp_pswrctl_path_private_methods = {
    AQPPSWRControlPathPrivateName,
    sizeof(AQPPSWRControlPathPrivate),
    AQPPSWRControlPathPrivate_copy,
    AQPPSWRControlPathPrivate_equal,
    AQPPSWRControlPathPrivate_out,
    AQPPSWRControlPathPrivate_read
};

static ExtensibleNodeMethods aqp_pswrctl_private_methods = {
    AQPPSWRControlPrivateName ,
    sizeof(AQPPSWRControlPrivate),
    AQPPSWRControlPrivate_copy,
    AQPPSWRControlPrivate_equal,
    AQPPSWRControlPrivate_out,
    AQPPSWRControlPrivate_read
};

CustomScanMethods aqp_pswrctl_methods = {
    AQPPSWRControlName,
    aqp_pswrctl_create_state
};

CustomExecMethods aqp_pswrctl_exec_methods = {
    /*CustomName=*/AQPPSWRControlStateName,
    /*BeginCustomScan=*/aqp_pswrctl_begin,
    /*ExecCustomScan=*/aqp_pswrctl_exec,
    /*EndCustomScan=*/aqp_pswrctl_end,
    /*ReScanCustomScan=*/aqp_pswrctl_rescan,
    /*MarkPosCustomScan=*/NULL,
    /*RestrPosCustomScan=*/NULL,
    /*EstimateDSMCustomScan=*/NULL,
    /*InitializeDSMCustomScan=*/NULL,
    /*ReInitializeDSMCustomScan=*/NULL,
    /*InitializeWorkerCustomScan=*/NULL,
    /*ShutdownCustomScan=*/NULL,
    /*ExplainCustomScan=*/aqp_pswrctl_explain
};
static Plan*
aqp_plan_pswrctl_path(PlannerInfo *root,
                      RelOptInfo *rel,
                      CustomPath *best_path,
                      List *tlist,
                      List *clauses,
                      List *custom_plans)
{
    CustomScan *pswrctl;
    AQPPSWRControlPrivate *private;
    AQPPSWRControlPathPrivate *pprivate;

    Assert(list_length(best_path->custom_private) == 1);
    pprivate =
        (AQPPSWRControlPathPrivate *) linitial(best_path->custom_private);

    private = (AQPPSWRControlPrivate *) palloc(sizeof(AQPPSWRControlPrivate));
    private->extnode.type = T_ExtensibleNode;
    private->extnode.extnodename = AQPPSWRControlPrivateName;

    if (IsA(pprivate->sample_size_expr, Const))
    {
        Const *c = (Const *) pprivate->sample_size_expr;
        if (c->constisnull)
            elog(ERROR, "null sample budget specified in TABLESAMPLE SWR/PSWR");
        private->sample_budget = DatumGetInt64(c->constvalue);
        private->sample_size_expr = NULL;
    }
    else
    {
        private->sample_budget = 100; /* dummy value */
        private->sample_size_expr = pprivate->sample_size_expr;
    }
    if (private->sample_budget <= 0)
    {
        elog(ERROR, "non-positive row count %ld in TABLESAMPLE SWR/PSWR",
                    private->sample_budget);
    }

    private->initial_phase_sample_size_expr =
        pprivate->initial_phase_sample_size_expr;
    private->step_sample_size_expr =
        pprivate->step_sample_size_expr;
    private->err0_expr = pprivate->err0_expr;
    private->confidence0_expr = pprivate->confidence0_expr;
    private->pswrctl_info_paramid = -1; /* to be filled later */

    Assert(list_length(custom_plans) == 1);
    Assert(IsA(linitial(custom_plans), Agg));
    
    pswrctl = makeNode(CustomScan);
    pswrctl->scan.plan.targetlist = tlist;
    pswrctl->scan.plan.qual = NULL;
    pswrctl->scan.plan.lefttree = linitial(custom_plans);
    pswrctl->scan.plan.righttree = NULL;

    pswrctl->scan.scanrelid = 0;
    pswrctl->flags = 0;
    pswrctl->custom_plans = NIL;
    pswrctl->custom_exprs = NIL;
    pswrctl->custom_private = list_make1(private);
    pswrctl->custom_scan_tlist = NIL;
    pswrctl->custom_relids = NULL; /* will be set by create_customscan_plan */
    pswrctl->methods = &aqp_pswrctl_methods;
    
    return (Plan *) pswrctl;
}

void
aqp_fix_pswrctl(CustomScan *cscan,
                int pswrctl_info_paramid)
{
    AQPPSWRControlPrivate *private;
    Agg *agg;
    List *agg_tlist;
    List *pswrctl_tlist;
    
    if (cscan->scan.plan.lefttree == NULL ||
        !IsA(cscan->scan.plan.lefttree, Agg))
    {
        elog(ERROR, "unexpected child node type for PSWRCTL node: %d",
             nodeTag(cscan));
    }

    private = (AQPPSWRControlPrivate *) linitial(cscan->custom_private);
    agg = (Agg *) cscan->scan.plan.lefttree;
    aqp_rewrite_pswrctl_tlist(cscan->scan.plan.targetlist, agg->plan.targetlist,
                              &agg_tlist, &pswrctl_tlist);

    /* 
     * We're leaking two old tlists in cscan and agg. They should go away
     * anyway once the query finishes.
     */
    cscan->scan.plan.targetlist = pswrctl_tlist;
    agg->plan.targetlist = agg_tlist;
    agg->aggsplit = AGGSPLITOP_SKIPFINAL;
    private->pswrctl_info_paramid = pswrctl_info_paramid;
}

static void
aqp_rewrite_pswrctl_tlist(
    List *dummy_tlist,
    List *orig_agg_tlist,
    List **agg_tlist,
    List **pswrctl_tlist)
{
    ListCell *lc, *lc2;

    *agg_tlist = NIL;
    *pswrctl_tlist = NIL;
    forboth(lc, dummy_tlist, lc2, orig_agg_tlist)
    {
        TargetEntry *tle_dummy = (TargetEntry *) lfirst(lc);
        TargetEntry *tle = (TargetEntry *) lfirst(lc2);

        TargetEntry *new_tle = makeNode(TargetEntry);
        new_tle->expr = (Expr *) aqp_rewrite_pswrctl_tlist_expr(
            (Node *) tle->expr, agg_tlist);
        new_tle->resno = tle->resno;
        /* result aliases are assigned from upper ops if any */
        new_tle->resname = tle_dummy->resname;
        new_tle->ressortgroupref = tle_dummy->ressortgroupref;
        new_tle->resorigtbl = 0;
        new_tle->resorigcol = 0;
        new_tle->resjunk = false;

        *pswrctl_tlist = lappend(*pswrctl_tlist, new_tle);
    }
}

static Node *
aqp_rewrite_pswrctl_tlist_expr(Node *expr, List **agg_tlist)
{
    if (expr == NULL)
        return NULL;

    if (IsA(expr, Aggref))
    {
        Aggref *aggref = (Aggref *) expr;
        AttrNumber varattno;
        Var *var;

        varattno = aqp_add_distinct_aggref_to_tlist(agg_tlist, aggref);
        var = makeVar(OUTER_VAR, varattno, INT8OID, 0, InvalidOid, 0);
        var->varnosyn = 0;
        var->varattnosyn = 0;
        return (Node *) var;
    }

    if (!aqp_expr_contains_aggref(expr) &&
        !IsA(expr, Const) &&
        !IsA(expr, Param))
    {
        AttrNumber varattno;

        varattno = aqp_add_distinct_expr_to_tlist(agg_tlist, (Expr *) expr);

        return (Node *) makeVar(OUTER_VAR,
                                varattno,
                                exprType(expr),
                                exprTypmod(expr),
                                exprCollation(expr),
                                0);
    }

    return expression_tree_mutator(expr, aqp_rewrite_pswrctl_tlist_expr,
                                   agg_tlist);
}

static AttrNumber
aqp_add_distinct_aggref_to_tlist(List **aggrefs, Aggref *aggref)
{
    ListCell *lc;
    TargetEntry *tle;
    AttrNumber resno;

    if (aggref->aggfnoid != aqp_approx_sum_internal_oid)
        {
            elog(ERROR, "exact aggregation is currently disallowed in aqp "
                        "mode");
        }

    foreach(lc, *aggrefs)
    {
        tle = (TargetEntry *) lfirst(lc);
        /* 
         * make sure we have an approximate aggregation;
         * others are not supported right now.
         *
         */

        if (equal((Node *) aggref, (Node *) tle->expr))
        {
            /* cell number starts from 0 but attrnumber starts from 1*/
            return (AttrNumber) list_cell_number(*aggrefs, lc) + 1;
        }
    }
    
    resno = list_length(*aggrefs) + 1;
    tle = makeNode(TargetEntry);
    tle->expr = (Expr *) aggref;
    tle->resno = resno;
    tle->resname = NULL;
    tle->ressortgroupref = 0;
    tle->resorigtbl = 0;
    tle->resorigcol = 0;
    tle->resjunk = false;
    *aggrefs = lappend(*aggrefs, tle);

    return resno;
}

static bool
aqp_expr_contains_aggref(Node *node)
{
    bool found = false;

    (void) expression_tree_walker(node,
                                  aqp_expr_contains_aggref_walker,
                                  &found);
    return found;
}

static bool
aqp_expr_contains_aggref_walker(Node *node, void *ctx)
{
    bool *found = (bool *) ctx;

    if (node == NULL)
        return false;

    if (IsA(node, Aggref))
    {
        *found = true;
        return true;
    }

    return expression_tree_walker(node,
                                  aqp_expr_contains_aggref_walker,
                                  ctx);
}

static AttrNumber
aqp_add_distinct_expr_to_tlist(List **tlist, Expr *expr)
{
    ListCell *lc;
    TargetEntry *tle;
    AttrNumber resno;

    foreach(lc, *tlist)
    {
        tle = (TargetEntry *) lfirst(lc);
        if (equal((Node *) expr, (Node *) tle->expr))
            return (AttrNumber) list_cell_number(*tlist, lc) + 1;
    }

    resno = list_length(*tlist) + 1;
    tle = makeTargetEntry((Expr *) copyObject(expr), resno, NULL, false);
    tle->ressortgroupref = 0;
    *tlist = lappend(*tlist, tle);

    return resno;
}

static void
AQPPSWRControlPathPrivate_copy(ExtensibleNode *newnode_,
                               const ExtensibleNode *from_)
{
    elog(ERROR, "not implemented");
}

static bool
AQPPSWRControlPathPrivate_equal(const ExtensibleNode *a_,
                                const ExtensibleNode *b_)
{
    elog(ERROR, "not implemented");
    return false;
}

static void
AQPPSWRControlPathPrivate_out(StringInfo str,
                              const ExtensibleNode *node_)
{
    AQPPSWRControlPathPrivate *node = (AQPPSWRControlPathPrivate *) node_;
    
    appendStringInfoString(str, ":sample_size_expr ");
    outNode(str, node->sample_size_expr);
    appendStringInfoString(str, ":initial_phase_sample_size_expr ");
    outNode(str, node->initial_phase_sample_size_expr);
    appendStringInfoString(str, ":step_sample_size_expr ");
    outNode(str, node->step_sample_size_expr);
    appendStringInfoString(str, ":err0_expr ");
    outNode(str, node->err0_expr);
    appendStringInfoString(str, ":confidence0_expr ");
    outNode(str, node->confidence0_expr);
}

static void
AQPPSWRControlPathPrivate_read(ExtensibleNode *node_)
{
    elog(ERROR, "not implemented");
}

static void
AQPPSWRControlPrivate_copy(ExtensibleNode *newnode_,
                           const ExtensibleNode *from_)
{
    elog(ERROR, "not implemented");
}

static bool
AQPPSWRControlPrivate_equal(const ExtensibleNode *a_,
                            const ExtensibleNode *b_)
{
    elog(ERROR, "not implemented");
    return false;
}

static void
AQPPSWRControlPrivate_out(StringInfo str,
                          const ExtensibleNode *node_)
{
    AQPPSWRControlPrivate *node = (AQPPSWRControlPrivate *) node_;

    appendStringInfo(str, ": sample_budget " UINT64_FORMAT, node->sample_budget);
    appendStringInfoString(str, ":sample_size_expr ");
    outNode(str, node->sample_size_expr);
    appendStringInfoString(str, ":initial_phase_sample_size_expr ");
    outNode(str, node->initial_phase_sample_size_expr);
    appendStringInfoString(str, ":step_sample_size_expr ");
    outNode(str, node->step_sample_size_expr);
    appendStringInfoString(str, ":err0_expr ");
    outNode(str, node->err0_expr);
    appendStringInfoString(str, ":confidence0_expr ");
    outNode(str, node->confidence0_expr);
    appendStringInfo(str, ":pswrctl_info_paramid %d",
                     node->pswrctl_info_paramid);
}

static void
AQPPSWRControlPrivate_read(ExtensibleNode *node_)
{
    elog(ERROR, "not implemented");
}

static Node *
aqp_pswrctl_create_state(CustomScan *cscan)
{
    AQPPSWRControlState *state;
    
    Assert(cscan->custom_private != NIL);
    
    state = (AQPPSWRControlState *) palloc0(sizeof(AQPPSWRControlState));  
    state->css.ss.ps.type = T_CustomScanState;
    state->css.custom_ps = NIL;
    state->css.pscan_len = 0;
    state->css.methods = &aqp_pswrctl_exec_methods;
    
    return (Node *) state;
}

static void
aqp_pswrctl_begin(CustomScanState *node, EState *estate, int eflags)
{
    AQPPSWRControlState *state = (AQPPSWRControlState *) node;
    CustomScan *cscan = (CustomScan *) state->css.ss.ps.plan;
    AQPPSWRControlPrivate *private =
        (AQPPSWRControlPrivate *) linitial(cscan->custom_private);
    Plan *outerPlan;
    AggState *aggstate;
    AQPPSWRCtlInfo pswrctl_info;
    ExprContext *ecxt;
    int i;
    TupleDesc agg_resultdesc;

    /*
     * At this point, ExecInitCustomScan has done the following:
     *
     * 1. Created the expression context for this node.
     * 2. Since our scanrel is NULL, it'll initialize the scan tuple slot using
     *    custom_scan_tlist (which is NIL), with a zero-length descriptor.
     *    This is useless and needs to be discarded.
     * 3. Initialized the result tuple slot as a virtual tuple slot using our
     *    dummy target list (nulls with correct types). The resulting tuple
     *    descriptor and slot are both correct.
     * 4. A projection info node that performs the final step of the
     *    aggregations and the remaining projection expressions. The projection
     *    info is valid. 
     * 5. No qual will be initialized since we did not provide that in the
     *    first place.
     *
     * We first perform the assertions below to make sure the reality matches
     * the description above. However, unlike the actual sample scan nodes, we
     * don't bother remove the extra scan tuple slot. It won't be used anyway.
     * Later, ExecEndCustomScan will also try to clear the scan tuple slot. If
     * it is NULL, we'll segfault there.
     */
    Assert(state->css.ss.ps.ps_ExprContext != NULL);
    Assert(state->css.ss.ss_currentRelation == NULL);
    Assert(state->css.ss.ss_ScanTupleSlot != NULL);
    Assert(state->css.ss.ps.ps_ResultTupleSlot != NULL);
    Assert(state->css.ss.ps.ps_ProjInfo != NULL);
    Assert(state->css.ss.ps.qual == NULL);
    
    
    /* 
     * At least allocate the persample info and sampler ctl before we
     * initialize the subtree, so they can grab the pointers to them from
     * the global exec param list. They will remain constant throughout
     * the plan execution.
     */
    aqp_pswrctl_init_pswrctl_global_params(state, estate);
    pswrctl_info = state->pswrctl_info;
    
    outerPlan = outerPlan(cscan);
    Assert(outerPlan != NULL && IsA(outerPlan, Agg));
    state->grouped = (((Agg *) outerPlan)->numCols > 0);
    outerPlanState(state) = ExecInitNode(outerPlan, estate, eflags);

    if (pswrctl_info->nsubplans <= 0)
    {
        elog(ERROR, "no valid pswr subplans were found");
    }

    if ((aqp_batch_sampling || aqp_batch_sampling_dp) &&
        private->initial_phase_sample_size_expr != NULL &&
        pswrctl_info->nsamplers > 1)
    {
        elog(ERROR,
            "greedy does not support join queries");
    }
    
    state->n_valid_subplans = 0;
    state->emptyres = false;
    for (i = 0; i < pswrctl_info->nsubplans; ++i)
    {
        if (!pswrctl_info->subplan_info[i].valid)
            continue;
        
        ++state->n_valid_subplans;
        state->emptyres |= pswrctl_info->subplan_info[i].emptyres;
    }
    
    /* 
     * If any subplan says the quals are not satisfiable (i.e, emptyres),
     * we can skip any plan selection and/or sample size allocation. Just let
     * them return a NULL slot.
     */
    if (state->emptyres)
    {
        state->n_valid_subplans = 0;
        state->valid_subplan_idx = NULL;
    }
    else
    {
        int j;

        state->valid_subplan_idx = (int*)
            palloc(sizeof(int) * state->n_valid_subplans);
    
        j = 0;
        for (i = 0; i < pswrctl_info->nsubplans; ++i)
        {
            if (!pswrctl_info->subplan_info[i].valid)
                continue;
            Assert(j < state->n_valid_subplans);
            state->valid_subplan_idx[j++] = i;
        }
    }

    /*
     * Initialize physical TABLESAMPLE runtime state. 
     * sampler_ctl[] is for per-table sampler state such as
     * inv_prob, local draw count, driver flag, and per-table sample_size.
     */
    if (pswrctl_info->sampler_ctl)
    {
        for (i = 0; i < pswrctl_info->nsamplers; ++i)
        {
            pswrctl_info->sampler_ctl[i].inv_prob = 1.0;
            pswrctl_info->sampler_ctl[i].num_samples_fetched = 0;
            pswrctl_info->sampler_ctl[i].is_driver = false;
        }
    }

    aqp_pswrctl_mark_driver_subplan(
        pswrctl_info,
        (state->n_valid_subplans > 0) ? state->valid_subplan_idx[0] : -1);

    /* Compute the sampling parameters. */
    ecxt = state->css.ss.ps.ps_ExprContext;
    if (!private->sample_size_expr)
    {
        state->sample_budget = private->sample_budget;
        state->sample_size_expr_state = NULL;
    }
    else
    {
        Datum res;
        bool isnull;

        state->sample_size_expr_state = ExecInitExpr(private->sample_size_expr,
                                                     &state->css.ss.ps);
        res = ExecEvalExprSwitchContext(state->sample_size_expr_state,
                                        ecxt,
                                        &isnull);
        if (isnull)
            elog(ERROR, "null sample size in tablesample pswr/swr operator");
        state->sample_budget = (uint64) res;
    }
    
    if (!private->initial_phase_sample_size_expr)
    {
        state->do_pswr = false;

        state->do_progressive_sampling = false;
        state->do_switch_to_uniform = false;

        state->initial_phase_sample_size_expr_state = NULL;
        state->err0_expr_state = NULL;
        state->confidence0_expr_state = NULL;

        state->initial_phase_sample_size = state->sample_budget;
        state->initial_and_uniform_sample_size = state->initial_phase_sample_size;
        state->step_sample_size = 0;
        state->err0 = 0.0;
        state->relative_ci = 0.0;
        state->confidence0 = 0.0;
    }
    else
    {
        MemoryContext oldcontext;
        Datum res;
        bool isnull;

        Assert(private->err0_expr);
        Assert(private->confidence0_expr);

        state->do_pswr = true;
        state->initial_phase_sample_size_expr_state = ExecInitExpr(
            private->initial_phase_sample_size_expr, &state->css.ss.ps);
        state->step_sample_size_expr_state = ExecInitExpr(
            private->step_sample_size_expr, &state->css.ss.ps);
        state->err0_expr_state = ExecInitExpr(
            private->err0_expr, &state->css.ss.ps);
        state->confidence0_expr_state = ExecInitExpr(
            private->confidence0_expr, &state->css.ss.ps);

        state->do_progressive_sampling = aqp_enable_pswr_switch_to_uniform;
        state->do_switch_to_uniform = false;

        state->progressive_current_phase_enabled = false;
        state->batch_resume_start_progressive = false;
        state->progressive_uniform_candidate = false;
        state->progressive_partition_budget_capacity = 0;
        state->stratified_phase_sample_size = 0;
        state->uniform_phase_sample_size = 0;

        state->prev_stratified_phase_sample_size = 0;
        state->phase_start_ci = 0.0;
        state->progressive_last_ci = 0.0;

        state->stratified_probe_rounds = 0;
        state->progressive_bad_rounds = 0;

        state->discarded_probe_samples = 0;
        state->progressive_bad_checkpoint_valid = false;
        state->progressive_bad_checkpoint = NULL;

        state->phase_remaining_budget = 0;
        state->current_round_budget = 0;

        state->partition_total_budget = NULL;
        state->partition_remaining_budget = NULL;
        state->partition_round_budget = NULL;

        oldcontext = MemoryContextSwitchTo(ecxt->ecxt_per_tuple_memory);

        res = ExecEvalExpr(state->initial_phase_sample_size_expr_state,
                           ecxt, &isnull);
        if (isnull)
            elog(ERROR, "null initial phase sample size in tablesample pswr "
                        "operator");
        state->initial_phase_sample_size = DatumGetUInt64(res);

        state->initial_and_uniform_sample_size = state->initial_phase_sample_size;

        res = ExecEvalExpr(state->step_sample_size_expr_state,
                           ecxt, &isnull);
        if (isnull)
            elog(ERROR, "null step sample size in tablesample pswr operator");
        state->step_sample_size = DatumGetUInt64(res);

        res = ExecEvalExpr(state->err0_expr_state, ecxt, &isnull);
        if (isnull)
            elog(ERROR, "null err0 in tablesample pswr operator");
        state->relative_ci = DatumGetFloat8(res);
        state->err0 = 0.0;
        if (state->relative_ci <= 0.0 ||
            state->relative_ci >= 1.0)
            elog(ERROR, "target relative CI must be between 0 and 1");        

        res = ExecEvalExpr(state->confidence0_expr_state, ecxt, &isnull);
        if (isnull)
            elog(ERROR, "null confidence0 in tablesample pswr operator");
        state->confidence0 = DatumGetFloat8(res);

        MemoryContextSwitchTo(oldcontext);
    }
    
    if (private->sample_size_expr || private->initial_phase_sample_size_expr)
        MemoryContextReset(ecxt->ecxt_per_tuple_memory);
    
    /* TODO We don't have an syntactical operator for plan switching yet... */
    state->do_plan_switching = false;


    Assert(IsA(outerPlanState(state), AggState));
    aggstate = (AggState *) outerPlanState(state);

    /* 
     * We preallocate the same number of pertrans states for the internal
     * transition functions to grab.
     */
    Assert(aggstate->numtrans > 0);
    state->ntrans = aggstate->numtrans;

    /* 
     * palloc probably won't give us cacheline aligned buffers, so
     * we allocate a bit more than what we need and manually align it.*/
    state->transstate = (AQPApproxAggTransState *)
        palloc0(sizeof(AQPApproxAggTransState) * state->ntrans
                + PG_CACHE_LINE_SIZE);
    
   if (((uintptr_t) state->transstate) & (PG_CACHE_LINE_SIZE - 1))
    {
        state->transstate = (AQPApproxAggTransState *)
            CACHELINEALIGN(state->transstate);
    }
    for (i = 0; i < aggstate->numtrans; ++i)
    {
        /* 
         * XXX right now, we just arbitrarily choose the first aggregation as
         * the optimization target. This is based on the syntactical input
         * in the SQL, and this is the very first Aggref that gets added
         * to the Agg node. Maybe later we can have some other way for
         * specifying our optimization target through some interesting SQL
         * syntax....
         */
        aqp_approx_sum_transstate_init(state, &state->transstate[i]);
        aggstate->pertrans[i].initValue =
            PointerGetDatum(&state->transstate[i]);
        aggstate->pertrans[i].initValueIsNull = false;
    }

    /*
    * We also need to fix the result slot descriptor so that Aggref output
    * columns from the child Agg are described as transition states.  With
    * GROUP BY, the child Agg targetlist also contains group keys, and those
    * columns must keep their original descriptor types.
    */
    {
        Agg *agg = (Agg *) outerPlan;
        ListCell *lc;

        agg_resultdesc = aggstate->ss.ps.ps_ResultTupleDesc;

        foreach(lc, agg->plan.targetlist)
        {
            TargetEntry *tle = lfirst_node(TargetEntry, lc);

            if (IsA(tle->expr, Aggref))
            {
                Aggref *aggref = (Aggref *) tle->expr;

                TupleDescInitEntry(agg_resultdesc,
                                tle->resno,
                                NULL,
                                aggref->aggtranstype,
                                -1,
                                0);
            }
        }
    }
    
    if (state->grouped)
    {
        state->grouped_results =
            tuplestore_begin_heap(false, false, work_mem);

        state->grouped_output_slot =
            ExecInitExtraTupleSlot(estate,
                                   agg_resultdesc,
                                   &TTSOpsMinimalTuple);

        state->grouped_results_ready = false;
    }

    if (aqp_batch_sampling || aqp_optimization_strategy != AQP_OPTIMIZATION_STRATEGY_DP)
        aqp_pswr_tree = false;
    
    /* if (aqp_pswr_tree)
        aqp_optimization_strategy = AQP_OPTIMIZATION_STRATEGY_DP; */

    if (state->do_pswr && !aqp_batch_sampling)
    {
        size_t sz;
        char *mem;
        if (aqp_optimization_strategy == AQP_OPTIMIZATION_STRATEGY_OPT_STRAT)
        {
            /* TODO do not need prev_idx and gpsa_opt_cost for strategy 2 */
            sz = sizeof(AQPPrefixStats) * (state->initial_phase_sample_size + 1)
                    + sizeof(Datum) * 2 * state->initial_phase_sample_size
                    + sizeof(float8) * state->initial_phase_sample_size
                    /* + MAXALIGN(sizeof(int) * (state->initial_phase_sample_size - 1)
                        * state->initial_phase_sample_size) */
                    + sizeof(Datum) * state->initial_phase_sample_size
                    + sizeof(uint64) * state->initial_phase_sample_size
                    + sizeof(float8) * state->initial_phase_sample_size;
            elog(INFO, "dp_mem_usage = %lu", sz);
            mem = palloc(sz);

            state->prefix_stats = (AQPPrefixStats *) mem;
            mem += sizeof(AQPPrefixStats) * (state->initial_phase_sample_size + 1);
            pswrctl_info->recorded_kv_pairs = (Datum *) mem;
            mem += sizeof(Datum) * 2 * state->initial_phase_sample_size;
            state->gpsa_opt_cost = (float8 *) mem;
            mem += sizeof(float8) * state->initial_phase_sample_size;
            /*state->gpsa_opt_prev_idx = (int*) mem;
            mem += MAXALIGN(sizeof(int) * (state->initial_phase_sample_size - 1)
                * state->initial_phase_sample_size);*/
            state->gpsa_opt_ub = (Datum *) mem;
            mem += sizeof(uint64) * state->initial_phase_sample_size;
            state->gpsa_opt_sample_size = (uint64 *) mem;
            mem += sizeof(float8) * state->initial_phase_sample_size;
            state->gpsa_opt_sample_percent = (float8 *) mem;
        }
        else if (aqp_optimization_strategy == AQP_OPTIMIZATION_STRATEGY_EQUAL_STRAT)
        {
            sz = sizeof(Datum) * (state->initial_phase_sample_size + 1)
                    + sizeof(Datum) * state->initial_phase_sample_size
                    + sizeof(Datum) * state->initial_phase_sample_size
                    + sizeof(uint64) * state->initial_phase_sample_size
                    + sizeof(float8) * state->initial_phase_sample_size;
            elog(INFO, "dp_mem_usage = %lu", sz);
            mem = palloc(sz);

            state->prefix_keys = (Datum *) mem;
            mem += sizeof(Datum) * (state->initial_phase_sample_size + 1);
            pswrctl_info->recorded_kv_pairs = (Datum *) mem;
            mem += sizeof(Datum) * state->initial_phase_sample_size;
            state->gpsa_opt_ub = (Datum *) mem;
            mem += sizeof(uint64) * state->initial_phase_sample_size;
            state->gpsa_opt_sample_size = (uint64 *) mem;
            mem += sizeof(float8) * state->initial_phase_sample_size;
            state->gpsa_opt_sample_percent = (float8 *) mem;
        }
        else
        {
            if (aqp_pswr_tree)
                sz = sizeof(AQPPrefixStats) * (aqp_dp_intervals_count + 1)
                        + sizeof(Datum) * 2 * state->initial_phase_sample_size
                        + sizeof(uint32) * state->initial_phase_sample_size
                        + sizeof(float8) * aqp_dp_intervals_count
                        /* + sizeof(float8) * aqp_dp_intervals_count
                        + sizeof(float8) * aqp_dp_intervals_count */
                        + sizeof(float8) * aqp_dp_intervals_count
                        + MAXALIGN(sizeof(int) * (aqp_dp_intervals_count - 1)
                            * aqp_dp_intervals_count)
                        + sizeof(Datum) * aqp_dp_intervals_count
                        + sizeof(uint64) * aqp_dp_intervals_count
                        + sizeof(float8) * aqp_dp_intervals_count;
            else
                sz = sizeof(AQPPrefixStats) * (aqp_dp_intervals_count + 1)
                        + sizeof(Datum) * 2 * state->initial_phase_sample_size
                        + sizeof(float8) * aqp_dp_intervals_count
                        + MAXALIGN(sizeof(int) * (aqp_dp_intervals_count - 1)
                            * aqp_dp_intervals_count)
                        + sizeof(Datum) * aqp_dp_intervals_count
                        + sizeof(uint64) * aqp_dp_intervals_count
                        + sizeof(float8) * aqp_dp_intervals_count;
            elog(INFO, "dp_mem_usage = %lu", sz);
            mem = palloc(sz);

            state->prefix_stats = (AQPPrefixStats *) mem;
            mem += sizeof(AQPPrefixStats) * (aqp_dp_intervals_count + 1);
            pswrctl_info->recorded_kv_pairs = (Datum *) mem;
            mem += sizeof(Datum) * 2 * state->initial_phase_sample_size;
            if (aqp_pswr_tree)
            {
                pswrctl_info->samples_height = (uint32 *) mem;
                mem += sizeof(uint32) * state->initial_phase_sample_size;
                /* state->gpsa_opt_sigma = (float8 *) mem;
                mem += sizeof(float8) * aqp_dp_intervals_count;
                state->gpsa_opt_height = (float8 *) mem;
                mem += sizeof(float8) * aqp_dp_intervals_count; */
                state->gpsa_opt_cost2 = (float8 *) mem;
                mem += sizeof(float8) * aqp_dp_intervals_count;
            }
            state->gpsa_opt_cost = (float8 *) mem;
            mem += sizeof(float8) * aqp_dp_intervals_count;
            state->gpsa_opt_prev_idx = (int*) mem;
            mem += MAXALIGN(sizeof(int) * (aqp_dp_intervals_count - 1)
                * aqp_dp_intervals_count);
            state->gpsa_opt_ub = (Datum *) mem;
            mem += sizeof(uint64) * aqp_dp_intervals_count;
            state->gpsa_opt_sample_size = (uint64 *) mem;
            mem += sizeof(float8) * aqp_dp_intervals_count;
            state->gpsa_opt_sample_percent = (float8 *) mem;
        }
    }

    if (aqp_batch_sampling_dp)
        aqp_batch_sampling = true;

    if (!aqp_batch_sampling)
    {
        for (i = 0; i < state->ntrans; ++i)
        {
            AQPApproxAggTransState *ts = &state->transstate[i];
            ts->mu_part = (float8 *) palloc0(sizeof(float8));
            ts->var_part = (float8 *) palloc0(sizeof(float8));
            ts->n_part = (uint32 *) palloc0(sizeof(uint32));
            if (aqp_pswr_tree)
                ts->height_part = (float8 *) palloc0(sizeof(float8));
        }
    } 
    else
    {
        for (i = 0; i < state->ntrans; ++i)
        {
            AQPApproxAggTransState *ts = &state->transstate[i];
            ts->mu_part = (float8 *) palloc0(sizeof(float8) * 10000);
            ts->var_part = (float8 *) palloc0(sizeof(float8) * 10000);
            ts->n_part = (uint32 *) palloc0(sizeof(uint32) * 10000);
            ts->done = (uint32 *) palloc0(sizeof(uint32) * 10000);
            ts->weights = (float8 *) palloc(sizeof(float8) * 10000);
            ts->used = (uint32 *) palloc0(sizeof(uint32) * 10000);
            for (int j = 0; j < 10000; j++)
                ts->weights[j] = 1;
            ts->split_idx = 0;
        }
        state->gpsa_opt_sample_size = (uint64 *) palloc0(sizeof(uint64) * 10000);
        state->gpsa_opt_sample_percent = (float8 *) palloc0(sizeof(float8) * 10000);
        state->dp_prev_idx = (int *) palloc0(sizeof(int) * 10000);
        state->continue_descent = 0;
    }
    state->cur_phase = -1;
    state->cur_phase_n_partitions = 0;
    state->cur_partition = 0;
    state->continue_descent_dp = 0;
}

static TupleTableSlot *
aqp_pswrctl_emit_next_group(AQPPSWRControlState *state)
{
    TupleTableSlot *slot = state->grouped_output_slot;

    ExecClearTuple(slot);

    if (!tuplestore_gettupleslot(state->grouped_results,
                                 true,
                                 false,
                                 slot))
        return NULL;

    /* grouped slots are finalized before being stored in grouped_results. */

    state->css.ss.ps.ps_ProjInfo->pi_exprContext->ecxt_outertuple = slot;
    return ExecProject(state->css.ss.ps.ps_ProjInfo);
}

static void
aqp_pswrctl_detach_group_slot(AQPPSWRControlState *state,
                              TupleTableSlot *slot)
{
    Agg *agg = (Agg *) outerPlanState(state)->plan;
    MemoryContext oldcontext;
    ListCell *lc;

    if (aqp_batch_sampling)
        elog(ERROR, "GROUP BY with aqp_batch_sampling is not implemented yet");

    slot_getallattrs(slot);

    oldcontext = MemoryContextSwitchTo(state->css.ss.ps.state->es_query_cxt);

    foreach(lc, agg->plan.targetlist)
    {
        TargetEntry *tle = lfirst_node(TargetEntry, lc);

        if (IsA(tle->expr, Aggref))
        {
            bool isnull;
            Datum d;
            AQPApproxAggTransState *src;
            AQPApproxAggTransState *dst;

            d = slot_getattr(slot, tle->resno, &isnull);
            if (isnull)
                continue;

            src = (AQPApproxAggTransState *) DatumGetPointer(d);

            dst = (AQPApproxAggTransState *)
                palloc0(sizeof(AQPApproxAggTransState));
            memcpy(dst, src, sizeof(AQPApproxAggTransState));

            dst->is_template = false;

            {
                int nparts = 1;

                if (state->do_pswr && !aqp_batch_sampling)
                {
                    nparts = state->cur_phase_n_partitions + 1;
                    if (nparts < 1)
                        nparts = 1;
                }

                dst->mu_part = (float8 *) palloc0(sizeof(float8) * nparts);
                dst->var_part = (float8 *) palloc0(sizeof(float8) * nparts);
                dst->n_part = (uint32 *) palloc0(sizeof(uint32) * nparts);

                if (src->mu_part)
                    memcpy(dst->mu_part, src->mu_part, sizeof(float8) * nparts);
                if (src->var_part)
                    memcpy(dst->var_part, src->var_part, sizeof(float8) * nparts);
                if (src->n_part)
                    memcpy(dst->n_part, src->n_part, sizeof(uint32) * nparts);

                if (src->height_part)
                {
                    dst->height_part = (float8 *) palloc0(sizeof(float8) * nparts);
                    memcpy(dst->height_part, src->height_part, sizeof(float8) * nparts);
                }
            }

            if (src->grouped_kv_capacity > 0)
            {
                dst->grouped_kv_capacity = src->grouped_kv_capacity;

                dst->grouped_kv_values =
                    (Datum *) palloc(sizeof(Datum) * src->grouped_kv_capacity);
                memcpy(dst->grouped_kv_values,
                    src->grouped_kv_values,
                    sizeof(Datum) * src->grouped_kv_capacity);

                dst->grouped_kv_value_set =
                    (bool *) palloc(sizeof(bool) * src->grouped_kv_capacity);
                memcpy(dst->grouped_kv_value_set,
                    src->grouped_kv_value_set,
                    sizeof(bool) * src->grouped_kv_capacity);
            }

            slot->tts_values[tle->resno - 1] = PointerGetDatum(dst);
            slot->tts_isnull[tle->resno - 1] = false;
        }
    }

    MemoryContextSwitchTo(oldcontext);
}

static void
aqp_pswrctl_finalize_group_slot(AQPPSWRControlState *state,
                                TupleTableSlot *slot)
{
    Agg *agg = (Agg *) outerPlanState(state)->plan;
    ListCell *lc;

    slot_getallattrs(slot);

    foreach(lc, agg->plan.targetlist)
    {
        TargetEntry *tle = lfirst_node(TargetEntry, lc);

        if (IsA(tle->expr, Aggref))
        {
            bool isnull;
            Datum d;
            AQPApproxAggTransState *ts;

            d = slot_getattr(slot, tle->resno, &isnull);
            if (isnull)
                continue;

            ts = (AQPApproxAggTransState *) DatumGetPointer(d);

            if (!(ts->flags & AQP_APPROX_AGG_TRANSFLAG_FINALIZED))
            {
                aqp_approx_sum_transstate_finalize(ts);
                ts->flags |= AQP_APPROX_AGG_TRANSFLAG_FINALIZED;
            }
        }
    }
}

static TupleTableSlot *
aqp_pswrctl_exec(CustomScanState *node)
{
    AQPPSWRControlState *state = (AQPPSWRControlState *) node;
    AQPPSWRCtlInfo pswrctl_info = state->pswrctl_info;
    TupleTableSlot *slot;
    int i;  
    
    TimestampTz tz1, tz2, tz3, tz4;
    float8 time_total = 0.0;
    long s;
    int us;            

    if (state->grouped && state->grouped_results_ready)
    {
        slot = aqp_pswrctl_emit_next_group(state);

        if (!TupIsNull(slot))
            return slot;

        state->grouped_results_ready = false;
        tuplestore_clear(state->grouped_results);

        if (!state->do_pswr)
            return NULL;
    }

contine_initial_phase:
    if (state->last_phase)
    {
        elog(INFO, "total_samples_fetched = %lu, total_samples_effective = %lu", 
            aqp_pswr_budget_spent(state), state->total_samples_fetched);
        return NULL;
    }
       
    if (!aqp_batch_sampling)
    {
        if (state->grouped_resume_next_partition)
        {
            state->grouped_resume_next_partition = false;

            if (state->cur_partition == 0)
                goto process_first_partition;

            goto process_next_partition;
        }

        if (state->cur_phase == -1)
        {
            state->cur_phase = 0;
            if (state->emptyres)
            {
                pswrctl_info->cur_plan_id = -1; 
            }
            else
            {
                int cur_plan_id;

                pswrctl_info->sample_size = state->initial_phase_sample_size;
                state->cur_phase_n_partitions = 1;
                state->cur_partition = 0;
                state->cur_phase_sample_size = state->initial_phase_sample_size;
                
                /* We choose the initial plan arbitrarily for now. */
                /* if (state->do_pswr && aqp_pswr_chosen_plan_index >= 0)
                    cur_plan_id = aqp_pswr_chosen_plan_index;
                else
                    cur_plan_id = (state->n_valid_subplans > 0) ?
                        state->valid_subplan_idx[0] : -1;

                pswrctl_info->cur_plan_id = cur_plan_id;

                if (cur_plan_id < 0 ||
                    cur_plan_id >= pswrctl_info->nsubplans ||
                    !pswrctl_info->subplan_info[cur_plan_id].valid)
                    elog(ERROR, "invalid pswr chosen plan index %d", cur_plan_id);

                aqp_pswrctl_mark_driver_subplan(pswrctl_info, cur_plan_id); */

                /* Set the full range scan key. */
                /* pswrctl_info->current_scan_keys =
                    pswrctl_info->subplan_info[cur_plan_id].scan_keys;
                pswrctl_info->n_current_scan_keys =
                    pswrctl_info->subplan_info[cur_plan_id].n_scan_keys; */

                if (state->do_pswr &&
                    aqp_pswr_chosen_plan_index < 0 &&
                    !aqp_batch_sampling &&
                    !state->grouped)
                {
                    AQPPSWRPlanProbeSnapshot best;
                    uint64 total_probe_samples = 0;
                    int dp_candidate_count = 0;

                    memset(&best, 0, sizeof(best));

                    for (int pi = 0; pi < state->n_valid_subplans; ++pi)
                    {
                        int pid = state->valid_subplan_idx[pi];
                        AQPPSWRCtlSubplanInfo sp = &pswrctl_info->subplan_info[pid];

                        if (sp->valid && !sp->emptyres &&
                            !sp->allequi &&
                            sp->partition_attno != InvalidAttrNumber &&
                            OidIsValid(sp->partition_atttypid) &&
                            get_typbyval(sp->partition_atttypid))
                            ++dp_candidate_count;
                    }

                    if (dp_candidate_count == 0)
                        elog(ERROR, "pswr auto plan selection found no DP-capable candidate plan");

                    if (state->initial_phase_sample_size == 0 ||
                        state->initial_phase_sample_size >
                            state->sample_budget / (uint64) dp_candidate_count)
                        elog(ERROR,
                            "pswr auto plan selection needs %lu samples for %d candidate plans, "
                            "but sample_budget is %lu",
                            state->initial_phase_sample_size,
                            dp_candidate_count,
                            state->sample_budget);

                    elog(INFO,
                        "pswr auto plan selection: candidates = %d, initial sample size for each candidate = %lu",
                        dp_candidate_count,
                        state->initial_phase_sample_size);

                    for (int pi = 0; pi < state->n_valid_subplans; ++pi)
                    {
                        int pid = state->valid_subplan_idx[pi];
                        AQPPSWRCtlSubplanInfo sp = &pswrctl_info->subplan_info[pid];
                        TimestampTz probe_t1;
                        TimestampTz probe_t2;
                        uint64 fetched;
                        uint64 score;
                        uint64 prefix_len;

                        if (!sp->valid || sp->emptyres ||
                            sp->allequi ||
                            sp->partition_attno == InvalidAttrNumber)
                        {
                            elog(INFO, "skip plan %d: no DP partition key", pid);
                            continue;
                        }

                        if (!OidIsValid(sp->partition_atttypid) ||
                            !get_typbyval(sp->partition_atttypid))
                        {
                            elog(INFO,
                                "skip plan %d: pswr does not support byref partition columns",
                                pid);
                            continue;
                        }

                        pswrctl_info->cur_plan_id = pid;
                        aqp_pswrctl_mark_driver_subplan(pswrctl_info, pid);

                        pswrctl_info->current_scan_keys = sp->scan_keys;
                        pswrctl_info->n_current_scan_keys = sp->n_scan_keys;
                        pswrctl_info->sample_size = state->initial_phase_sample_size;
                        pswrctl_info->want_partition_key = true;
                        pswrctl_info->next_kv_idx = 0;
                        pswrctl_info->num_samples_fetched = 0;
                        pswrctl_info->inv_prob = 1.0;

                        if (pswrctl_info->sampler_ctl)
                        {
                            for (int sid = 0; sid < pswrctl_info->nsamplers; ++sid)
                            {
                                pswrctl_info->sampler_ctl[sid].inv_prob = 1.0;
                                pswrctl_info->sampler_ctl[sid].num_samples_fetched = 0;
                            }
                        }

                        state->nintvls = 0;
                        state->err0 = 0.0;
                        state->last_phase = false;
                        state->total_samples_fetched = 0;
                        state->initial_and_uniform_sample_size = state->initial_phase_sample_size;
                        state->cur_phase_n_partitions = 1;
                        state->cur_partition = 0;
                        state->cur_phase_sample_size = state->initial_phase_sample_size;

                        for (i = 0; i < state->ntrans; ++i)
                        {
                            AQPApproxAggTransState *ts = &state->transstate[i];

                            ts->flags = 0;
                            ts->n = 0;
                            ts->mu = 0.0;
                            ts->var = 0.0;
                            ts->mu_phase = 0.0;
                            ts->var_phase = 0.0;
                            ts->mu_total = 0.0;
                            ts->height = 0.0;
                            ts->var_total = 0.0;
                            ts->var_0 = 0.0;
                            ts->var_total_pre = 0.0;
                            ts->var_total_next = 0.0;
                            ts->split_idx = 0;
                            ts->dp_start_idx = 0;

                            if (ts->mu_part)
                                ts->mu_part[0] = 0.0;
                            if (ts->var_part)
                                ts->var_part[0] = 0.0;
                            if (ts->n_part)
                                ts->n_part[0] = 0;
                            if (ts->height_part)
                                ts->height_part[0] = 0.0;
                        }

                        state->transstate[0].flags |=
                            AQP_APPROX_AGG_TRANSFLAG_WANT_STATISTICS;

                        ExecReScanAgg((AggState *) outerPlanState(state));

                        probe_t1 = GetCurrentTimestamp();
                        slot = ExecProcNode(outerPlanState(state));
                        probe_t2 = GetCurrentTimestamp();

                        TimestampDifference(probe_t1, probe_t2, &s, &us);

                        fetched = pswrctl_info->num_samples_fetched;
                        total_probe_samples += fetched;

                        if (slot == NULL || TupIsNull(slot))
                        {
                            elog(INFO, "plan %d returned no result slot", pid);
                            continue;
                        }

                        state->total_samples_fetched = fetched;
                        state->initial_and_uniform_sample_size = fetched;

                        for (i = 0; i < state->ntrans; ++i)
                            aqp_approx_sum_transstate_finalize(&state->transstate[i]);

                        if (state->nintvls == 0 ||
                            state->transstate[0].mu_total == 0.0)
                        {
                            elog(INFO,
                                "plan %d skipped because phase0 samples produced zero estimate ",
                                pid);
                            continue;
                        }

                        state->err0 = fabs(state->transstate[0].mu_total) *
                                    state->relative_ci;

                        if (aqp_optimization_strategy == AQP_OPTIMIZATION_STRATEGY_OPT_STRAT)
                            aqp_gpsa_compute_metainfo_optimized_stratified(state);
                        else if (aqp_optimization_strategy == AQP_OPTIMIZATION_STRATEGY_EQUAL_STRAT)
                            aqp_gpsa_compute_metainfo_equal_stratified(state);
                        else if (aqp_pswr_tree)
                            aqp_gpsa_compute_metainfo_height(state);
                        else
                            aqp_gpsa_compute_metainfo(state);

                        score = state->cur_phase_sample_size;

                        elog(INFO,
                            "plan %d: required_sample_size = %lu, phase0_time_ms = %f",
                            pid,
                            score,
                            s * 1e3 + us * 1e-3);

                        if (!best.valid || score < best.score)
                        {
                            best.valid = true;
                            best.plan_id = pid;
                            best.fetched = fetched;
                            best.nintvls = state->nintvls;
                            best.score = score;
                            best.elapsed_ms = s * 1e3 + us * 1e-3;
                            best.prefix_len = state->nintvls + 1;
                            prefix_len = best.prefix_len;

                            if (state->prefix_stats != NULL)
                            {
                                best.prefix_stats =
                                    (AQPPrefixStats *) palloc(sizeof(AQPPrefixStats) *
                                                            prefix_len);
                                memcpy(best.prefix_stats,
                                    state->prefix_stats,
                                    sizeof(AQPPrefixStats) * prefix_len);
                            }

                            if (state->prefix_keys != NULL)
                            {
                                best.prefix_keys =
                                    (Datum *) palloc(sizeof(Datum) * prefix_len);
                                memcpy(best.prefix_keys,
                                    state->prefix_keys,
                                    sizeof(Datum) * prefix_len);
                            }

                            best.trans = (AQPPSWRPlanProbeTransSnapshot *)
                                palloc0(sizeof(AQPPSWRPlanProbeTransSnapshot) *
                                        state->ntrans);

                            for (i = 0; i < state->ntrans; ++i)
                            {
                                AQPApproxAggTransState *ts = &state->transstate[i];
                                AQPPSWRPlanProbeTransSnapshot *dst = &best.trans[i];

                                dst->scalar.flags = ts->flags;
                                dst->scalar.n = ts->n;
                                dst->scalar.mu = ts->mu;
                                dst->scalar.var = ts->var;
                                dst->scalar.mu_phase = ts->mu_phase;
                                dst->scalar.var_phase = ts->var_phase;
                                dst->scalar.mu_total = ts->mu_total;
                                dst->scalar.height = ts->height;
                                dst->scalar.var_total = ts->var_total;
                                dst->scalar.var_0 = ts->var_0;
                                dst->scalar.var_total_pre = ts->var_total_pre;
                                dst->scalar.var_total_next = ts->var_total_next;
                                dst->scalar.split_idx = ts->split_idx;
                                dst->scalar.dp_start_idx = ts->dp_start_idx;

                                dst->mu_part0 = ts->mu_part ? ts->mu_part[0] : 0.0;
                                dst->var_part0 = ts->var_part ? ts->var_part[0] : 0.0;
                                dst->n_part0 = ts->n_part ? ts->n_part[0] : 0;
                                dst->height_part0 =
                                    ts->height_part ? ts->height_part[0] : 0.0;
                            }

                            best.slot = ExecAllocTableSlot(
                                &state->css.ss.ps.state->es_tupleTable,
                                slot->tts_tupleDescriptor,
                                slot->tts_ops);
                            ExecCopySlot(best.slot, slot);
                        }
                    }

                    if (!best.valid)
                    {
                        if (total_probe_samples > 0)
                            state->discarded_probe_samples += total_probe_samples;

                        elog(ERROR,
                            "pswr auto plan selection could not choose a plan because " 
                            "all probed plans had zero phase-0 estimates "
                            "after %lu total probe samples",
                            total_probe_samples);
                    }

                    cur_plan_id = best.plan_id;
                    pswrctl_info->cur_plan_id = cur_plan_id;
                    aqp_pswrctl_mark_driver_subplan(pswrctl_info, cur_plan_id);

                    pswrctl_info->current_scan_keys =
                        pswrctl_info->subplan_info[cur_plan_id].scan_keys;
                    pswrctl_info->n_current_scan_keys =
                        pswrctl_info->subplan_info[cur_plan_id].n_scan_keys;
                    pswrctl_info->sample_size = state->initial_phase_sample_size;
                    pswrctl_info->want_partition_key = true;
                    pswrctl_info->num_samples_fetched = best.fetched;

                    state->nintvls = best.nintvls;
                    state->err0 = 0.0;
                    state->last_phase = false;
                    state->cur_phase = 0;
                    state->cur_phase_n_partitions = 1;
                    state->cur_partition = 1;
                    state->cur_phase_sample_size = state->initial_phase_sample_size;
                    state->total_samples_fetched = best.fetched;
                    state->initial_and_uniform_sample_size = best.fetched;

                    if (total_probe_samples > best.fetched)
                        state->discarded_probe_samples += total_probe_samples - best.fetched;

                    if (best.prefix_stats != NULL && state->prefix_stats != NULL)
                        memcpy(state->prefix_stats,
                            best.prefix_stats,
                            sizeof(AQPPrefixStats) * best.prefix_len);

                    if (best.prefix_keys != NULL && state->prefix_keys != NULL)
                        memcpy(state->prefix_keys,
                            best.prefix_keys,
                            sizeof(Datum) * best.prefix_len);

                    for (i = 0; i < state->ntrans; ++i)
                    {
                        AQPApproxAggTransState *ts = &state->transstate[i];
                        AQPPSWRPlanProbeTransSnapshot *src = &best.trans[i];

                        ts->flags = src->scalar.flags;
                        ts->n = src->scalar.n;
                        ts->mu = src->scalar.mu;
                        ts->var = src->scalar.var;
                        ts->mu_phase = src->scalar.mu_phase;
                        ts->var_phase = src->scalar.var_phase;
                        ts->mu_total = src->scalar.mu_total;
                        ts->height = src->scalar.height;
                        ts->var_total = src->scalar.var_total;
                        ts->var_0 = src->scalar.var_0;
                        ts->var_total_pre = src->scalar.var_total_pre;
                        ts->var_total_next = src->scalar.var_total_next;
                        ts->split_idx = src->scalar.split_idx;
                        ts->dp_start_idx = src->scalar.dp_start_idx;

                        if (ts->mu_part)
                            ts->mu_part[0] = src->mu_part0;
                        if (ts->var_part)
                            ts->var_part[0] = src->var_part0;
                        if (ts->n_part)
                            ts->n_part[0] = src->n_part0;
                        if (ts->height_part)
                            ts->height_part[0] = src->height_part0;
                    }

                    elog(NOTICE,
                        "chosen plan %d, kept = %lu, discarded = %lu, required_sample_size = %lu",
                        cur_plan_id,
                        best.fetched,
                        state->discarded_probe_samples,
                        best.score);

                    state->css.ss.ps.ps_ProjInfo->pi_exprContext->ecxt_outertuple =
                        best.slot;
                    return ExecProject(state->css.ss.ps.ps_ProjInfo);
                }

                if (state->do_pswr && aqp_pswr_chosen_plan_index >= 0)
                    cur_plan_id = aqp_pswr_chosen_plan_index;
                else
                    cur_plan_id = (state->n_valid_subplans > 0) ?
                        state->valid_subplan_idx[0] : -1;

                pswrctl_info->cur_plan_id = cur_plan_id;

                if (cur_plan_id < 0 ||
                    cur_plan_id >= pswrctl_info->nsubplans ||
                    !pswrctl_info->subplan_info[cur_plan_id].valid)
                    elog(ERROR, "invalid pswr chosen plan index %d", cur_plan_id);
                
                if (state->do_pswr && aqp_pswr_chosen_plan_index >= 0)
                    elog(INFO,
                        "now using plan %d",
                        cur_plan_id);

                aqp_pswrctl_mark_driver_subplan(pswrctl_info, cur_plan_id);

                pswrctl_info->current_scan_keys =
                    pswrctl_info->subplan_info[cur_plan_id].scan_keys;
                pswrctl_info->n_current_scan_keys =
                    pswrctl_info->subplan_info[cur_plan_id].n_scan_keys;
            }

            if (state->do_pswr)
            {
                Oid typid;
                pswrctl_info->want_partition_key = true;
                state->transstate[0].flags |=
                    AQP_APPROX_AGG_TRANSFLAG_WANT_STATISTICS;

                typid = pswrctl_info
                    ->subplan_info[pswrctl_info->cur_plan_id].partition_atttypid;

                /* 
                * XXX this is more or less a limitation in our implementation due
                * to laziness, not really something we can't do.
                *
                * To add varlen support, we also need to allocate a nested memory
                * context to store the varlen values there, potentially with
                * de-TOASTED large values.
                */
                if (!get_typbyval(typid))
                    elog(ERROR, "pswr does not support byref partition columns");

                pswrctl_info->next_kv_idx = 0;
                pswrctl_info->num_samples_fetched = 0;
                state->last_phase = false;
            }
            else
            {
                /* Not doing pswr, then this is already the last phase. */
                state->last_phase = true;
            }
        }
        else /* state->cur_phase >= 0 */
        {
            pswrctl_info->height_max = 0;
            pswrctl_info->height_min = 0;
            pswrctl_info->height_avg = 0.0;

            pswrctl_info->time_max = 0.0;
            pswrctl_info->time_min = 0.0;
            pswrctl_info->time_avg = 0.0;

            do {
                if (aqp_pswr_budget_spent(state) >= state->sample_budget)
                {
                    /* All budget used. We're done */
                    state->last_phase = true;
                    return NULL;
                }

                /* entering the new phase */
                ++state->cur_phase;

                if (state->cur_phase == 1 && state->err0 == 0.0)
                {
                    state->err0 = fabs(state->transstate[0].mu_total) * state->relative_ci;
                    if (state->transstate[0].mu_total == 0.0)
                        elog(ERROR,
                            "relative CI target is undefined because the phase0 estimate is zero");
                    else
                        elog(INFO,
                            "relative CI target: %f", state->err0);
                }
                
                if (!state->do_switch_to_uniform)
                {
                    for (i = 0; i < state->ntrans; ++i)
                        aqp_approx_sum_transstate_reset_phase(&state->transstate[i]);
                }

                if (state->nintvls == 0)
                {
                    /* 
                    * This is duplication of the initial phase, but if we ever
                    * end up here, it means we never got any meaningful
                    * estimation of the data.
                    *
                    * (Probably we shouldn't trigger that anyway).
                    */
                    state->cur_phase_n_partitions = 1;
                    state->cur_partition = 0;
                    state->cur_phase_sample_size = aqp_pswr_remaining_budget(state);
                    state->last_phase = true;

                    pswrctl_info->current_scan_keys = pswrctl_info->subplan_info[
                        pswrctl_info->cur_plan_id].scan_keys;
                    pswrctl_info->n_current_scan_keys = pswrctl_info->subplan_info[
                        pswrctl_info->cur_plan_id].n_scan_keys;
                    pswrctl_info->want_partition_key = false;
                    pswrctl_info->sample_size = state->cur_phase_sample_size;
                    break;
                }
                else
                {
                    tz3 = GetCurrentTimestamp();

                    if (pswrctl_info->want_partition_key)
                    {
                        if (aqp_optimization_strategy == 
                                AQP_OPTIMIZATION_STRATEGY_OPT_STRAT)
                        {
                            aqp_gpsa_compute_metainfo_optimized_stratified(state);
                        }
                        else if (aqp_optimization_strategy == 
                                AQP_OPTIMIZATION_STRATEGY_EQUAL_STRAT)
                        {
                            aqp_gpsa_compute_metainfo_equal_stratified(state);
                        }
                        else
                        {
                            if (aqp_pswr_tree)
                                aqp_gpsa_compute_metainfo_height(state);
                            else
                                aqp_gpsa_compute_metainfo(state);
                        }

                        for (i = 0; i < state->ntrans; ++i)
                        {
                            AQPApproxAggTransState *ts = &state->transstate[i];

                            ts->mu_part =
                                palloc0(sizeof(float8) *
                                        (state->cur_phase_n_partitions + 1));
                            ts->var_part =
                                palloc0(sizeof(float8) *
                                        (state->cur_phase_n_partitions + 1));
                            ts->n_part =
                                palloc0(sizeof(uint32) *
                                        (state->cur_phase_n_partitions + 1));

                            if (aqp_pswr_tree)
                            {
                                ts->height_part =
                                    palloc0(sizeof(float8) *
                                            (state->cur_phase_n_partitions + 1));
                                ts->height_part[0] = ts->height;
                            }

                            ts->mu_part[0] = ts->mu_total;
                            ts->var_part[0] = ts->var_total;
                            ts->n_part[0] = ts->n;
                        }
                    }
                    /*
                    else if (aqp_pswr_to_swr && state->continue_descent == 1)
                    {
                        state->transstate->mu_part = 
                            palloc0(sizeof(float8) * 
                                    (state->cur_phase_n_partitions + 1));
                        state->transstate->var_part = 
                            palloc0(sizeof(float8) * 
                                    (state->cur_phase_n_partitions + 1));
                        state->transstate->n_part = 
                            palloc0(sizeof(uint32) *
                                    (state->cur_phase_n_partitions + 1));
                        
                        if (aqp_pswr_tree)
                        {
                            state->transstate->height_part = 
                                palloc0(sizeof(float8) *
                                        (state->cur_phase_n_partitions + 1));
                            state->transstate->height_part[0] = state->transstate->height;
                        }
                        state->transstate->mu_part[0] = state->transstate->mu_total;
                        state->transstate->var_part[0] = state->transstate->var_total;
                        state->transstate->n_part[0] = state->transstate->n;
                    }
                    */

                    tz4 = GetCurrentTimestamp();
                    TimestampDifference(tz3, tz4, &s, &us);
                    elog(INFO, "dp time = %f", (s * 1e3 + us * 1e-3));
                    
                    state->stratified_phase_sample_size = state->cur_phase_sample_size;

                    aqp_pswr_cap_allocation_to_remaining_budget(state);

                    if (state->cur_phase_sample_size == 0)
                    {
                        state->last_phase = true;
                        return NULL;
                    }

                    elog(INFO, "nintvls = %lu, n_partitions = %d, sample_size_total = %lu",
                            state->nintvls, state->cur_phase_n_partitions,
                            state->cur_phase_sample_size);

                    /* 
                    * state->cur_phase_n_partitions, state->cur_phase_sample_size
                    * are set by aqp_gpsa_compute_metainfo() 
                    */

                    if (state->do_progressive_sampling &&
                        state->cur_phase_n_partitions > 1 &&
                        !state->do_switch_to_uniform)
                    {
                        uint64 remaining_budget;
                        uint64 pswr_eff;
                        uint64 uniform_eff;
                        uint64 probe_window;
                        /* uint64 prev_pswr; */
                        float8 current_ci;
                        /*
                        bool has_previous_gpsa_phase;
                        bool previous_phase_unstable;
                        bool allocation_growth; 
                        */
                        bool uniform_competitive;
                        bool large_allocation;

                        current_ci = aqp_erf_inv(state->confidence0) *
                                    sqrt(2 * state->transstate->var_total);

                        /* has_previous_gpsa_phase = state->cur_phase > 1;
                        previous_phase_unstable =
                            has_previous_gpsa_phase &&
                            state->phase_start_ci > 0.0 &&
                            current_ci >= state->phase_start_ci; */

                        probe_window =
                            (uint64) AQP_PSWR_MIN_STRATIFIED_PROBE_ROUNDS *
                            (uint64) aqp_progressive_round_size;

                        state->phase_start_ci = current_ci;
                        state->progressive_last_ci = current_ci;

                        remaining_budget = aqp_pswr_remaining_budget(state);

                        if (remaining_budget == 0)
                        {
                            state->last_phase = true;
                            return NULL;
                        }

                        /* prev_pswr = state->prev_stratified_phase_sample_size; */
                        state->uniform_phase_sample_size =
                            aqp_pswr_estimate_uniform_remaining_sample_size(state);

                        pswr_eff = Min(state->stratified_phase_sample_size, remaining_budget);
                        uniform_eff =
                            state->uniform_phase_sample_size > 0 ?
                            Min(state->uniform_phase_sample_size, remaining_budget) : 0;

                        uniform_competitive =
                            state->uniform_phase_sample_size > 0 &&
                            pswr_eff > 0 &&
                            (float8) uniform_eff <=
                            (float8) pswr_eff * AQP_PSWR_UNIFORM_COMPETITIVE_FACTOR;

                        /* allocation_growth =
                            has_previous_gpsa_phase &&
                            prev_pswr > 0 &&
                            (float8) state->stratified_phase_sample_size >
                            (float8) prev_pswr * AQP_PSWR_UNIFORM_COMPETITIVE_FACTOR; */

                        large_allocation = pswr_eff > probe_window;

                        state->progressive_uniform_candidate = uniform_competitive;
                        state->progressive_current_phase_enabled = large_allocation;

                        state->prev_stratified_phase_sample_size =
                            state->stratified_phase_sample_size;

                        /* elog(INFO,
                            "phase %d: pswr_raw=%lu, prev_pswr=%lu, uniform_raw=%lu, "
                            "remaining=%lu, pswr_eff=%lu, uniform_eff=%lu, unstable=%s, "
                            "growth=%s, close=%s, large=%s, progressive=%s",
                            state->cur_phase,
                            state->stratified_phase_sample_size,
                            prev_pswr,
                            state->uniform_phase_sample_size,
                            remaining_budget,
                            pswr_eff,
                            uniform_eff,
                            previous_phase_unstable ? "true" : "false",
                            allocation_growth ? "true" : "false",
                            uniform_competitive ? "true" : "false",
                            large_allocation ? "true" : "false",
                            state->progressive_current_phase_enabled ? "true" : "false"); */

                        if (state->progressive_current_phase_enabled)
                        {
                            aqp_pswr_progressive_init_current_phase(state);
                            aqp_pswr_progressive_build_each_round(state);
                        }
                    }
                    else
                    {
                        state->progressive_current_phase_enabled = false;
                        state->progressive_uniform_candidate = false;
                    }

process_first_partition:
                    state->cur_partition = 0;
                    /* state->last_phase = true; */ /* TODO pswr strategy? */
                    pswrctl_info->want_partition_key = false;
                    
                    if (state->cur_phase_n_partitions == 1)
                    {
                        /* Only one partition in this phase */
                        pswrctl_info->current_scan_keys = pswrctl_info
                            ->subplan_info[pswrctl_info->cur_plan_id].scan_keys;
                        pswrctl_info->n_current_scan_keys = pswrctl_info
                            ->subplan_info[pswrctl_info->cur_plan_id].n_scan_keys;
                        pswrctl_info->sample_size = state->cur_phase_sample_size;
                        break; /* breaks to while (0) */
                    }

                    /* More than 1 partition. Start from the lmost first. */
                    {
                        ScanKey lmost;
                        int ub_idx;

                        lmost = pswrctl_info->subplan_info[
                            pswrctl_info->cur_plan_id].lmost_partition_keys;
                        ub_idx = pswrctl_info->subplan_info[
                            pswrctl_info->cur_plan_id].ub_idx_in_lmost_partition;

                        lmost[ub_idx].sk_argument = state->gpsa_opt_ub[0];

                        if (state->progressive_current_phase_enabled &&
                            !state->do_switch_to_uniform)
                            pswrctl_info->sample_size =
                                state->partition_round_budget[0];
                        else
                            pswrctl_info->sample_size = 
                                state->gpsa_opt_sample_size[0];

                        pswrctl_info->current_scan_keys = lmost;
                        pswrctl_info->n_current_scan_keys = pswrctl_info
                            ->subplan_info[pswrctl_info->cur_plan_id]
                            .n_lmost_partition_keys;
                        break; /* breaks to while(0) */
                    }

process_next_partition:
                    Assert(state->cur_partition > 0);
                    if (state->cur_partition + 1 == state->cur_phase_n_partitions)
                    {
                        /* handles right-most partition */
                        ScanKey rmost;
                        int lb_idx;

                        rmost = pswrctl_info->subplan_info[
                            pswrctl_info->cur_plan_id].rmost_partition_keys;
                        lb_idx = pswrctl_info->subplan_info[
                            pswrctl_info->cur_plan_id].lb_idx_in_rmost_partition;

                        rmost[lb_idx].sk_argument =
                            state->gpsa_opt_ub[state->cur_partition - 1];

                        if (state->progressive_current_phase_enabled &&
                            !state->do_switch_to_uniform)
                            pswrctl_info->sample_size =
                                state->partition_round_budget[state->cur_partition];
                        else
                            pswrctl_info->sample_size =
                                state->gpsa_opt_sample_size[state->cur_partition];

                        pswrctl_info->current_scan_keys = rmost;
                        pswrctl_info->n_current_scan_keys = pswrctl_info
                            ->subplan_info[pswrctl_info->cur_plan_id]
                            .n_rmost_partition_keys;
                        break; /* breaks to while(0) */
                    }
                        
                    /* all other partitions in the middle */
                    {
                        ScanKey key;
                        int lb_idx;
                        int ub_idx;

                        key = pswrctl_info->subplan_info[
                            pswrctl_info->cur_plan_id].partition_keys;
                        lb_idx = pswrctl_info->subplan_info[
                            pswrctl_info->cur_plan_id].lb_idx_in_partition_keys;
                        ub_idx = pswrctl_info->subplan_info[
                            pswrctl_info->cur_plan_id].ub_idx_in_partition_keys;
                        
                        key[lb_idx].sk_argument =
                            state->gpsa_opt_ub[state->cur_partition - 1];
                        key[ub_idx].sk_argument =
                            state->gpsa_opt_ub[state->cur_partition];

                        if (state->progressive_current_phase_enabled &&
                            !state->do_switch_to_uniform)
                            pswrctl_info->sample_size =
                                state->partition_round_budget[state->cur_partition];
                        else
                            pswrctl_info->sample_size =
                                state->gpsa_opt_sample_size[state->cur_partition];
                                
                        pswrctl_info->current_scan_keys = key;
                        pswrctl_info->n_current_scan_keys = pswrctl_info
                            ->subplan_info[pswrctl_info->cur_plan_id]
                            .n_partition_keys;
                    }
                }
            } while(0);

rescan_current_round:
            pswrctl_info->num_samples_fetched = 0;
            for (i = 0; i < state->ntrans; ++i)
                aqp_approx_sum_transstate_reset_partition(&state->transstate[i]);
            ExecReScanAgg((AggState *) outerPlanState(state));
        } /* end of state->cur_phase >= 0 */

        /* elog(INFO, "phase %d partition %d size %lu", 
            state->cur_phase,
            state->cur_partition,
            pswrctl_info->sample_size); */
        
        tz1 = GetCurrentTimestamp();

        /*
         * The scan nodes share one pswrctl_info block. For join queries, each
         * participating SWR scan contributes its own inverse probability for
         * the current output tuple, so start from 1.0 before pulling the next
         * tuple from the subplan and let scan nodes multiply into it.
         */
        pswrctl_info->inv_prob = 1.0;
        if (pswrctl_info->sampler_ctl)
        {
            for (i = 0; i < pswrctl_info->nsamplers; ++i)
            {
                pswrctl_info->sampler_ctl[i].inv_prob = 1.0;
                pswrctl_info->sampler_ctl[i].num_samples_fetched = 0;
            }
        }
    
        if (state->grouped)
        {
            PlanState *outer_state = outerPlanState(state);
            int output_phase = state->cur_phase;
            int output_partition = state->cur_partition;

            if (outer_state == NULL)
                elog(ERROR, "AQPPSWRControl has no initialized child plan state");

            if (outer_state->ExecProcNode == NULL)
                elog(ERROR, "AQPPSWRControl child plan has no ExecProcNode");

            for (;;)
            {
                slot = ExecProcNode(outer_state);

                if (TupIsNull(slot))
                    break;

                aqp_pswrctl_finalize_group_slot(state, slot);
                aqp_pswrctl_detach_group_slot(state, slot);
                tuplestore_puttupleslot(state->grouped_results, slot);
            }

            state->total_samples_fetched += pswrctl_info->num_samples_fetched;
            if (state->do_switch_to_uniform)
                state->initial_and_uniform_sample_size += pswrctl_info->num_samples_fetched;

            if (state->do_pswr && pswrctl_info->want_partition_key)
            {
                for (i = 0; i < state->ntrans; ++i)
                    aqp_approx_sum_transstate_finalize(&state->transstate[i]);
            }

            if (state->do_pswr)
            {
                if (++state->cur_partition < state->cur_phase_n_partitions)
                {
                    Assert(state->cur_phase > 0);
                    state->grouped_resume_next_partition = true;
                }
                else if (state->cur_phase != 0)
                {
                    if (state->progressive_current_phase_enabled &&
                        !state->do_switch_to_uniform)
                    {
                        if (aqp_pswr_progressive_check_ci_and_switch(state))
                        {
                            aqp_pswr_progressive_enter_uniform(state);
                        }
                        else if (state->phase_remaining_budget > 0)
                        {
                            for (i = 0; i < state->ntrans; ++i)
                                aqp_approx_sum_transstate_reset_phase(&state->transstate[i]);
                            aqp_pswr_progressive_build_each_round(state);
                            state->cur_partition = 0;
                            state->grouped_resume_next_partition = true;
                        }
                        else
                        {
                            aqp_approx_sum_check_ci(state);
                        }
                    }
                    else
                    {
                        if (state->do_switch_to_uniform)
                            aqp_pswr_progressive_allocate_uniform_round(state);
                        else
                            aqp_approx_sum_check_ci(state);
                    }
                }
            }

            state->grouped_results_ready = true;
            tuplestore_rescan(state->grouped_results);

            elog(NOTICE,
                "aqp grouped output: phase = %d, partition = %d, sample_size = %lu, groups follow",
                output_phase,
                output_partition,
                pswrctl_info->sample_size);

            return aqp_pswrctl_emit_next_group(state);
        }

        slot = ExecProcNode(outerPlanState(state));
        
        tz2 = GetCurrentTimestamp();
        TimestampDifference(tz1, tz2, &s, &us);
        time_total += s * 1e3 + us * 1e-3;

        if (state->cur_partition + 1 == state->cur_phase_n_partitions &&
            !state->progressive_current_phase_enabled &&
            !state->do_switch_to_uniform)
        {
            elog(INFO, "%d phase time = %f", state->cur_phase+1, time_total);
            /* 
            if (state->cur_phase > 0)
            {
                pswrctl_info->height_avg = pswrctl_info->height_avg / 
                    state->cur_partition;
                elog(INFO, "height max: %d, min: %d, avg: %f", 
                        pswrctl_info->height_max, pswrctl_info->height_min,
                        pswrctl_info->height_avg);

                pswrctl_info->time_avg = pswrctl_info->time_avg / 
                    (state->cur_partition+1);
                elog(INFO, "time max: %f, min: %f, avg: %f", 
                        pswrctl_info->time_max, pswrctl_info->time_min,
                        pswrctl_info->time_avg);
    
            }
            */
        }

        state->total_samples_fetched += pswrctl_info->num_samples_fetched;
        if (state->do_switch_to_uniform)
            state->initial_and_uniform_sample_size += pswrctl_info->num_samples_fetched;

        for (i = 0; i < state->ntrans; ++i)
            aqp_approx_sum_transstate_finalize(&state->transstate[i]);
        if (++state->cur_partition < state->cur_phase_n_partitions)
        {
            Assert(state->do_pswr && state->cur_phase > 0);
            goto process_next_partition;
        }
        else if (state->cur_phase != 0) 
        {
            /* 
             * After one round, check if ci is dropping well
             * if not for three times, switch to uniform 
             * if yes, continue to next round
             */
            if (state->progressive_current_phase_enabled &&
                !state->do_switch_to_uniform)
            {
                bool switch_to_uniform;

                switch_to_uniform = aqp_pswr_progressive_check_ci_and_switch(state);

                if (state->last_phase)
                {
                    elog(INFO, "%d phase time = %f",
                        state->cur_phase + 1, time_total);
                }
                else if (switch_to_uniform)
                {
                    aqp_pswr_progressive_enter_uniform(state);
                    if (!state->last_phase)
                        goto rescan_current_round;

                    elog(INFO, "%d phase time = %f",
                        state->cur_phase + 1, time_total);
                }
                else if (state->phase_remaining_budget > 0)
                {
                    for (i = 0; i < state->ntrans; ++i)
                        aqp_approx_sum_transstate_reset_phase(&state->transstate[i]);

                    aqp_pswr_progressive_build_each_round(state);
                    goto process_first_partition;
                }
                else
                {
                    elog(INFO, "%d phase time = %f",
                        state->cur_phase + 1, time_total);

                    aqp_approx_sum_check_ci(state);
                }
            }
            else
            {
                if (state->do_switch_to_uniform)
                {
                    aqp_pswr_progressive_allocate_uniform_round(state);
                    if (!state->last_phase)
                        goto rescan_current_round;

                    elog(INFO, "%d phase time = %f",
                        state->cur_phase + 1, time_total);
                }
                else
                    aqp_approx_sum_check_ci(state);
            }
        }
    }
    else /* batch_sampling */
    {
        if (state->batch_resume_start_progressive)
        {
            state->batch_resume_start_progressive = false;
            ++state->cur_phase;

            for (i = 0; i < state->ntrans; ++i)
                aqp_approx_sum_transstate_reset_phase(&state->transstate[i]);

            aqp_pswr_progressive_init_current_phase(state);
            aqp_pswr_progressive_build_each_round(state);
            state->cur_partition = 1;
        }
        if (state->cur_phase == -1)
        {
            state->cur_phase = 0;
            if (state->emptyres)
            {
                pswrctl_info->cur_plan_id = -1; 
            }
            else
            {
                int cur_plan_id;

                pswrctl_info->sample_size = state->initial_phase_sample_size;
                state->cur_phase_n_partitions = 1;
                state->cur_partition = 0;
                state->cur_phase_sample_size = state->initial_phase_sample_size;
                
                /* We choose the initial plan arbitrarily for now. */
                if (state->do_pswr && aqp_pswr_chosen_plan_index >= 0)
                    cur_plan_id = aqp_pswr_chosen_plan_index;
                else
                    cur_plan_id = (state->n_valid_subplans > 0) ?
                        state->valid_subplan_idx[0] : -1;

                pswrctl_info->cur_plan_id = cur_plan_id;

                if (cur_plan_id < 0 ||
                    cur_plan_id >= pswrctl_info->nsubplans ||
                    !pswrctl_info->subplan_info[cur_plan_id].valid)
                    elog(ERROR, "invalid pswr chosen plan index %d", cur_plan_id);
                
                if (state->do_pswr)
                {
                    if (aqp_pswr_chosen_plan_index >= 0)
                        elog(INFO,
                            "batch sampling: using manually chosen plan %d",
                            cur_plan_id);
                    else
                        elog(INFO,
                            "batch sampling: using default plan %d",
                            cur_plan_id);
                }

                aqp_pswrctl_mark_driver_subplan(pswrctl_info, cur_plan_id);

                /* Set the full range scan key. */
                pswrctl_info->current_scan_keys =
                    pswrctl_info->subplan_info[cur_plan_id].scan_keys;
                pswrctl_info->n_current_scan_keys =
                    pswrctl_info->subplan_info[cur_plan_id].n_scan_keys;
            }

            if (state->do_pswr)
            {
                Oid typid;
                pswrctl_info->want_partition_key = false;
                state->transstate[0].flags = AQP_LEAF_PAGE_STATISTICS; 

                typid = pswrctl_info
                    ->subplan_info[pswrctl_info->cur_plan_id].partition_atttypid;

                /* 
                * XXX this is more or less a limitation in our implementation due
                * to laziness, not really something we can't do.
                *
                * To add varlen support, we also need to allocate a nested memory
                * context to store the varlen values there, potentially with
                * de-TOASTED large values.
                */
                if (!get_typbyval(typid))
                    elog(ERROR, "pswr does not support byref partition columns");

                /* pswrctl_info->next_kv_idx = 0; */
                pswrctl_info->num_samples_fetched = 0;
                state->last_phase = false;
            }
            else
            {
                /* Not doing pswr, then this is already the last phase. */
                state->last_phase = true;
            }
        }
        else /* state->cur_phase >= 0 */
        {
            /*
            pswrctl_info->height_max = 0;
            pswrctl_info->height_min = 0;
            pswrctl_info->height_avg = 0.0;

            pswrctl_info->time_max = 0.0;
            pswrctl_info->time_min = 0.0;
            pswrctl_info->time_avg = 0.0;
            */

            do {
                if ((aqp_pswr_budget_spent(state) >= state->sample_budget &&
                    state->cur_phase > 0)
                    || (state->cur_phase_n_partitions == 1 &&
                        !state->do_switch_to_uniform))
                {
                    /* All budget used or there is only one leaf page. We're done */
                    elog(INFO, "leaf_page_samples = %u", state->transstate->n_part[0]);

                    state->last_phase = true;
                    return NULL;
                }

                if (state->transstate[0].flags == AQP_LEAF_PAGE_STATISTICS)
                {
                    elog(INFO, "leaf_page_samples = %u", state->transstate->n_part[0]);
                    
                    /*
                    state->gpsa_opt_sample_size = palloc(sizeof(uint64) * 
                        (state->cur_phase_n_partitions));

                    state->transstate->mu_part = palloc0(sizeof(float8) * 
                        (state->cur_phase_n_partitions + 1));
                    state->transstate->var_part = palloc0(sizeof(float8) * 
                        (state->cur_phase_n_partitions + 1));
                    state->transstate->n_part = palloc0(sizeof(uint32) *
                        (state->cur_phase_n_partitions + 1));
                    */

                    state->transstate->mu_part[0] = state->transstate->mu_phase;
                    state->transstate->var_part[0] = state->transstate->var_phase;
                    state->transstate->n_part[0] = state->transstate->n;
                }

                state->transstate[0].flags = AQP_INTERNAL_PAGE_STATISTICS;

                if (state->cur_partition == state->cur_phase_n_partitions 
                    /*|| (pswrctl->cur_partition == 0 &&)*/)
                {
                    /* entering the new phase */
                    ++state->cur_phase;

                    for (i = 0; i < state->ntrans; ++i)
                        aqp_approx_sum_transstate_reset_phase(&state->transstate[i]);
                    state->cur_partition = 1;
                }

process_next_partition_subtree:
                if (state->cur_partition < state->cur_phase_n_partitions 
                    || state->continue_descent == 1)
                {
                    /* ++state->cur_partition; */
                    for (i = 0; i < state->ntrans; ++i)
                        aqp_approx_sum_transstate_reset_partition(&state->transstate[i]);
                }
            }while (0);
            
            pswrctl_info->num_samples_fetched = 0;
            ExecReScanAgg((AggState *) outerPlanState(state));
        } /* end of state->cur_phase >= 0 */

        tz1 = GetCurrentTimestamp();

        /* See the non-batch branch above for why inv_prob is reset here. */
        pswrctl_info->inv_prob = 1.0;
        if (pswrctl_info->sampler_ctl)
        {
            for (i = 0; i < pswrctl_info->nsamplers; ++i)
            {
                pswrctl_info->sampler_ctl[i].inv_prob = 1.0;
                pswrctl_info->sampler_ctl[i].num_samples_fetched = 0;
            }
        }

        if (state->grouped && state->do_pswr)
            elog(ERROR, "GROUP BY with PSWR batch sampling is not implemented yet");

        slot = ExecProcNode(outerPlanState(state));

        tz2 = GetCurrentTimestamp();
        TimestampDifference(tz1, tz2, &s, &us);
        time_total += s * 1e3 + us * 1e-3;

        if (state->cur_partition + 1 == state->cur_phase_n_partitions)
        {
            elog(INFO, "%d phase time = %f", state->cur_phase+1, time_total);

        }

        if (aqp_batch_sampling_dp)
        {
            if(state->continue_descent != 3 && state->continue_descent_dp == 0)
                state->initial_sample_size_total += pswrctl_info->num_samples_fetched;
        }
        else
        {
            if (state->continue_descent != 3)
                state->initial_sample_size_total += pswrctl_info->num_samples_fetched;
        }  

        state->total_samples_fetched += pswrctl_info->num_samples_fetched;

        if (state->do_switch_to_uniform)
            state->initial_and_uniform_sample_size += pswrctl_info->num_samples_fetched;
        
        if(state->continue_descent != -1)
            for (i = 0; i < state->ntrans; ++i)
                aqp_approx_sum_transstate_finalize(&state->transstate[i]);
        if (state->transstate[0].flags == AQP_LEAF_PAGE_STATISTICS 
            && state->cur_phase_n_partitions > 1)
        {
            ++state->cur_partition;
            goto contine_initial_phase;
        }
        else if (state->transstate[0].flags == AQP_INTERNAL_PAGE_STATISTICS)
        {
            if (++state->cur_partition < state->cur_phase_n_partitions)
                goto process_next_partition_subtree;
            else
            {
                if (state->do_switch_to_uniform)
                {
                    aqp_pswr_progressive_allocate_uniform_round(state);
                    if (!state->last_phase)
                        goto process_next_partition_subtree;
                }
                else if (state->progressive_current_phase_enabled &&
                        !aqp_batch_sampling_dp)
                {
                    if (aqp_pswr_progressive_check_ci_and_switch(state))
                    {
                        aqp_pswr_progressive_enter_uniform(state);
                        if (!state->last_phase)
                            goto process_next_partition_subtree;
                    }
                    else if (state->phase_remaining_budget > 0)
                    {
                        for (i = 0; i < state->ntrans; ++i)
                            aqp_approx_sum_transstate_reset_phase(&state->transstate[i]);

                        aqp_pswr_progressive_build_each_round(state);
                        state->cur_partition = 1;
                        goto process_next_partition_subtree;
                    }
                    else
                    {
                        aqp_approx_sum_check_ci(state);
                    }
                }
                else
                {
                    aqp_approx_sum_check_ci(state);
                }

                if (!state->last_phase &&
                    state->do_progressive_sampling &&
                    !aqp_batch_sampling_dp &&
                    !state->do_switch_to_uniform &&
                    state->continue_descent == 3 &&
                    state->cur_phase_n_partitions > 1)
                {
                    uint64 remaining_budget = aqp_pswr_remaining_budget(state);
                    uint64 probe_window =
                        (uint64) AQP_PSWR_MIN_STRATIFIED_PROBE_ROUNDS *
                        (uint64) aqp_progressive_round_size;
                    uint64 pswr_eff;
                    float8 current_ci;

                    if (remaining_budget == 0 || state->cur_phase_sample_size == 0)
                    {
                        state->last_phase = true;
                        return NULL;
                    }

                    state->stratified_phase_sample_size = state->cur_phase_sample_size;
                    pswr_eff = Min(state->stratified_phase_sample_size, remaining_budget);

                    current_ci = aqp_erf_inv(state->confidence0) *
                                sqrt(2 * state->transstate->var_total);

                    state->phase_start_ci = current_ci;
                    state->progressive_last_ci = current_ci;
                    state->uniform_phase_sample_size =
                        aqp_pswr_estimate_uniform_remaining_sample_size(state);

                    state->progressive_current_phase_enabled =
                        pswr_eff > probe_window;

                    if (state->progressive_current_phase_enabled)
                    {
                        state->batch_resume_start_progressive = true;
                    }
                }

                if((state->continue_descent == 1)
                || (state->continue_descent == 3 && state->continue_descent_dp == 1))
                    goto process_next_partition_subtree;
            }
        }
    }
    
    /* elog(INFO, "mu: %f, var: %f", state->transstate[0].mu_total, 
         aqp_erf_inv(0.95) * sqrt(2 * state->transstate[0].var_total));*/

    Assert(!TupIsNull(slot));
    state->css.ss.ps.ps_ProjInfo->pi_exprContext->ecxt_outertuple = slot;
    return ExecProject(state->css.ss.ps.ps_ProjInfo);
}

static void
aqp_pswrctl_end(CustomScanState *node)
{
    AQPPSWRControlState *state = (AQPPSWRControlState *) node;

    if (state->grouped_results)
        tuplestore_end(state->grouped_results);

    ExecEndNode(outerPlanState(node));
}

static void
aqp_pswrctl_rescan(CustomScanState *node)
{
    elog(ERROR, "aqp_pswrctl_rescan not implemented yet");
}

static void
aqp_pswrctl_explain(CustomScanState *node, List *ancestors,
                    ExplainState *es)
{
    /* TODO */
}

static void
aqp_pswrctl_init_pswrctl_global_params(AQPPSWRControlState *state,
                                       EState *estate)
{
    CustomScan *cscan = (CustomScan *) state->css.ss.ps.plan;
    AQPPSWRControlPrivate *private =
        (AQPPSWRControlPrivate *) linitial(cscan->custom_private);
    AQPPSWRCtlInfo pswrctl_info;
    
    Assert(CurrentMemoryContext == estate->es_query_cxt);
    
    /* 
     * XXX this is assuming we have a non-parallel execution plan and there is
     * exactly one sampler beneath the current pswrctl node.  If any of those
     * change in the future, make sure to allocate multiple ones in an array.
     */
    pswrctl_info = (AQPPSWRCtlInfo) palloc0(sizeof(AQPPSWRCtlInfoData));
    pswrctl_info->pswrctl = state;

    pswrctl_info->driver_subplan_id = -1;
    pswrctl_info->driver_sampler_id = -1;
    pswrctl_info->pswr_driver_sampler_id = -1;
    pswrctl_info->nsamplers = 0;
    pswrctl_info->sampler_ctl = NULL;
    
    estate->es_param_exec_vals[private->pswrctl_info_paramid].execPlan = NULL;
    estate->es_param_exec_vals[private->pswrctl_info_paramid].value =
        PointerGetDatum(pswrctl_info);
    estate->es_param_exec_vals[private->pswrctl_info_paramid].isnull = false;
    state->pswrctl_info = pswrctl_info;
}

/*
static void
dp_allocation(AQPPSWRCtlInfoData *pswrctl_info, AQPPSWRControlState *state, int k_clusters)
{
    Datum *keys = state->bound_keys;
    float8 *Sxx = state->Sxx;
    float8 *Sx = state->Sx;
    float8 *m = state->m;
    int8 **index = state->index;
    float8 **dp = state->dp;
    float8 linear_co = aqp_dp_linear_coefficient;
    float8 err = state->err0;
    float8 Z = sqrt(2)*aqp_erf_inv(state->confidence0);

    int k = k_clusters;

    int kn;

    float8 *Sxx_p = (float8*)palloc0((k_clusters+1) * sizeof(float8));

    for (int i = 1; i <= k; i++)
    {
        Sxx_p[i] = Sxx[i]-Sxx[i-1]+Sx[i]*Sx[i]/m[i]-Sx[i-1]*Sx[i-1]/m[i-1]
            - ((Sx[i]-Sx[i-1])*(Sx[i]-Sx[i-1])/(m[i]-m[i-1]));
        //elog(INFO, "%f, Sxx: %f", Sxx_p[i], Sxx[i]);
    }

    for (int i=0; i<=k; i++){
        elog(INFO, "%f", Sxx[i]);
    }
    
    for (int j = 1; j <= k; j++) 
    {
        for (int i = 1; i <= k; i++) 
        {
            dp[i][j] = INFINITY;
        }
    }
    
    for (int i = 1; i <= k; i++) {
        dp[i][1] = sqrt(Sxx[i]/(m[i]-1));
        //elog(INFO, "%f", dp[i][1]);
    }
    elog(INFO, "dp j = 1, dp[k][j] = %f", dp[k][1]);
    
    for (int j = 2; j <= k; j++)
    {
        for (int i = j; i <= k; i++) 
        {
            for (int x = j - 1; x < i; x++) 
            {
                float8 Sxx_p1 = Sxx[i]-Sxx[x]+Sx[i]*Sx[i]/m[i]-Sx[x]*Sx[x]/m[x]
                                - ((Sx[i]-Sx[x])*(Sx[i]-Sx[x])/(m[i]-m[x]));
                if (dp[i][j] > dp[x][j - 1] + sqrt((Sxx_p1/(m[i+1]-m[x+1]-1)))) 
                {
                    index[i][j] = x;
                }
                dp[i][j] = Min(dp[i][j], 
                               dp[x][j - 1] + sqrt((Sxx_p1/(m[i]-m[x]-1))));
                //elog(INFO, "dp j = %d, i = %d, x = %d, dp[i][j] = %f", j,i,x,dp[i][j]);
            }
        }
        elog(INFO, "dp j = %d, dp[k][j] = %f", j, dp[k][j]);
        //if (Z * dp[k][j] / err + linear_co * j 
        //    > Z * dp[k][j - 1] / err + linear_co * (j - 1))
        if (j == 4)
        {
            elog(INFO, "%f", dp[k][j-1]);
            kn = j - 1;
            elog(INFO, "kn=%d", kn);
            break;
        }
    }

    elog(INFO, "%d", kn);
    for(int j = 1; j <= kn; j++)
    {
        int idx = index[k][j];
        elog(INFO, "x: %d", index[k][j]);
        float8 Sxx_p = Sxx[j]-Sxx[j-1]+Sx[j]*Sx[j]/m[j]-Sx[j-1]*Sx[j-1]/m[j-1]
            - ((Sx[j]-Sx[j-1])*(Sx[j]-Sx[j-1])/(m[j]-m[j-1]));
        state->m[j-1] = Z*Z*Sxx_p*state->transstate->Sxx/(err*err);
        state->bound_keys[j-1] = keys[idx];
    }
}
*/

static void
aqp_pswrctl_mark_driver_subplan(AQPPSWRCtlInfo pswrctl_info,
                                int driver_subplan_id)
{
    int i;
    int driver_sampler_id;

    pswrctl_info->driver_subplan_id = driver_subplan_id;

    driver_sampler_id =
        pswrctl_info->pswr_driver_sampler_id >= 0 ?
        pswrctl_info->pswr_driver_sampler_id :
        driver_subplan_id;

    pswrctl_info->driver_sampler_id = driver_sampler_id;

    if (!pswrctl_info->sampler_ctl)
        return;

    for (i = 0; i < pswrctl_info->nsamplers; ++i)
        pswrctl_info->sampler_ctl[i].is_driver =
            (i == driver_sampler_id);
}

static uint64
aqp_pswr_estimate_uniform_remaining_sample_size(AQPPSWRControlState *state)
{
    AQPPrefixStats *ps = state->prefix_stats;
    uint64 nintvls = state->nintvls;
    float8 var_coefficient;
    float8 s;
    float8 c;
    float8 n0;
    float8 current_ci;
    float8 t1;
    float8 t2;
    uint64 sample_size;

    current_ci = aqp_erf_inv(state->confidence0) *
                 sqrt(2 * state->transstate->var_total);

    if (current_ci <= state->err0)
        return 0;

    if (state->do_switch_to_uniform &&
        state->transstate[0].var_part != NULL &&
        state->initial_and_uniform_sample_size > 0 &&
        state->transstate[0].var_part[0] > 0.0)
    {
        s = state->transstate[0].var_part[0] *
            state->initial_and_uniform_sample_size;
    }
    else
    {
        if (ps == NULL || nintvls == 0 ||
            ps[nintvls].m <= 1 || ps[nintvls].var <= 0.0)
            return 0;

        if (aqp_pswr_tree && ps[nintvls].h > 0)
        {
            float8 n = (float8) ps[nintvls].m;
            float8 h = (float8) ps[nintvls].h / n;

            s = sqrt(h * ps[nintvls].var * n) *
                sqrt(ps[nintvls].var * n / h);
        }
        else
        {
            s = ps[nintvls].var * ps[nintvls].m;
        }
    }

    var_coefficient = sqrt(2) * aqp_erf_inv(state->confidence0) /
                      state->err0;
    c = var_coefficient * var_coefficient;
    n0 = state->total_samples_fetched;

    t1 = s * c / 2.0 - n0;
    t2 = t1 * t1 + (state->transstate->var_total * c - 1.0) * n0 * n0;
    if (t2 < 0.0)
        return 0;
    t2 = sqrt(t2);
    if (t1 - t2 > 0)
    {
        elog(WARNING, "t1 = %f > t2 = %f", t1, t2);
        sample_size = ceil(t1 - t2);
    }
    else if (t1 + t2 > 0)
        sample_size = ceil(t1 + t2);
    else
        return 0;

    if (sample_size < 30.0)
        sample_size = 30.0;

    return sample_size;
}

static void
aqp_pswr_progressive_checkpoint_round_start(AQPPSWRControlState *state)
{
    AQPPSWRRollbackCheckpoint *ckpt;

    if (state->grouped ||
        !state->progressive_current_phase_enabled ||
        state->do_switch_to_uniform ||
        state->progressive_bad_rounds != 0)
        return;

    if (state->progressive_bad_checkpoint == NULL)
        state->progressive_bad_checkpoint =
            palloc0(sizeof(AQPPSWRRollbackCheckpoint));

    ckpt = state->progressive_bad_checkpoint;

    if (ckpt->ntrans != state->ntrans || ckpt->trans == NULL)
    {
        if (ckpt->trans == NULL)
            ckpt->trans = palloc0(sizeof(AQPApproxAggRollbackState) *
                                state->ntrans);
        else
            ckpt->trans = repalloc(ckpt->trans,
                                sizeof(AQPApproxAggRollbackState) *
                                state->ntrans);

        ckpt->ntrans = state->ntrans;
    }

    ckpt->total_samples_fetched = state->total_samples_fetched;
    ckpt->initial_and_uniform_sample_size =
        state->initial_and_uniform_sample_size;
    ckpt->progressive_last_ci = state->progressive_last_ci;

    for (int i = 0; i < state->ntrans; ++i)
    {
        AQPApproxAggTransState *ts = &state->transstate[i];
        AQPApproxAggRollbackState *dst = &ckpt->trans[i];

        dst->flags = ts->flags;
        dst->n = ts->n;
        dst->mu = ts->mu;
        dst->var = ts->var;
        dst->mu_phase = ts->mu_phase;
        dst->var_phase = ts->var_phase;
        dst->mu_total = ts->mu_total;
        dst->height = ts->height;
        dst->var_total = ts->var_total;
        dst->var_0 = ts->var_0;
        dst->var_total_pre = ts->var_total_pre;
        dst->var_total_next = ts->var_total_next;
        dst->split_idx = ts->split_idx;
        dst->dp_start_idx = ts->dp_start_idx;
    }

    state->progressive_bad_checkpoint_valid = true;
}

static void
aqp_pswr_progressive_rollback_bad_rounds(AQPPSWRControlState *state)
{
    AQPPSWRRollbackCheckpoint *ckpt = state->progressive_bad_checkpoint;
    uint64 discarded;

    if (state->grouped || !state->progressive_bad_checkpoint_valid || ckpt == NULL)
        return;

    discarded = state->total_samples_fetched > ckpt->total_samples_fetched ?
        state->total_samples_fetched - ckpt->total_samples_fetched : 0;

    state->discarded_probe_samples += discarded;
    state->total_samples_fetched = ckpt->total_samples_fetched;
    state->initial_and_uniform_sample_size =
        ckpt->initial_and_uniform_sample_size;
    state->progressive_last_ci = ckpt->progressive_last_ci;

    for (int i = 0; i < state->ntrans; ++i)
    {
        AQPApproxAggTransState *ts = &state->transstate[i];
        AQPApproxAggRollbackState *src = &ckpt->trans[i];

        ts->flags = src->flags;
        ts->n = src->n;
        ts->mu = src->mu;
        ts->var = src->var;
        ts->mu_phase = src->mu_phase;
        ts->var_phase = src->var_phase;
        ts->mu_total = src->mu_total;
        ts->height = src->height;
        ts->var_total = src->var_total;
        ts->var_0 = src->var_0;
        ts->var_total_pre = src->var_total_pre;
        ts->var_total_next = src->var_total_next;
        ts->split_idx = src->split_idx;
        ts->dp_start_idx = src->dp_start_idx;

        if (aqp_batch_sampling)
        {
            ts->mu_phase = ts->mu_total;
            ts->var_phase = ts->var_total;
        }
        else if (state->total_samples_fetched > state->initial_and_uniform_sample_size &&
                ts->mu_part != NULL &&
                ts->var_part != NULL)
        {
            float8 alpha =
                (float8) state->initial_and_uniform_sample_size /
                (float8) state->total_samples_fetched;
            float8 beta = 1.0 - alpha;

            ts->mu_phase =
                (ts->mu_total - alpha * ts->mu_part[0]) / beta;

            ts->var_phase =
                (ts->var_total - alpha * alpha * ts->var_part[0]) /
                (beta * beta);

            if (ts->var_phase < 0.0)
                ts->var_phase = 0.0;
        }
        else
        {
            ts->mu_phase = 0.0;
            ts->var_phase = 0.0;
        }
    }

    state->uniform_phase_sample_size = 0;
    state->progressive_bad_rounds = 0;

    state->progressive_bad_checkpoint_valid = false;

    /* elog(INFO,
         "rollback bad stratified rounds: discarded=%lu, total_discarded=%lu, "
         "effective_samples=%lu, budget_spent=%lu",
         discarded,
         state->discarded_probe_samples,
         state->total_samples_fetched,
         aqp_pswr_budget_spent(state)); */
}

static void 
aqp_pswr_progressive_init_current_phase(AQPPSWRControlState *state)
{
    int n = state->cur_phase_n_partitions;

    if (state->partition_total_budget == NULL ||
        state->progressive_partition_budget_capacity < (uint64) n)
    {
        if (state->partition_total_budget != NULL)
        {
            pfree(state->partition_total_budget);
            pfree(state->partition_remaining_budget);
            pfree(state->partition_round_budget);
        }

        state->partition_total_budget = (uint64 *) palloc0(sizeof(uint64) * n);
        state->partition_remaining_budget = (uint64 *) palloc0(sizeof(uint64) * n);
        state->partition_round_budget = (uint64 *) palloc0(sizeof(uint64) * n);
        state->progressive_partition_budget_capacity = n;
    }

    {
        uint64 remaining_budget = aqp_pswr_remaining_budget(state);
        uint64 total_budget = 0;

        if (aqp_batch_sampling)
        {
            for (int i = 1; i < n; i++)
                total_budget += state->gpsa_opt_sample_size[i];

            state->cur_phase_sample_size = total_budget;
        }
        else
        {
            total_budget = state->cur_phase_sample_size;
        }

        state->phase_remaining_budget = Min(total_budget, remaining_budget);
        state->current_round_budget = 0;
        state->stratified_probe_rounds = 0;
        state->progressive_bad_rounds = 0;

        for (int i = 0; i < n; i++)
        {
            state->partition_round_budget[i] = 0;

            if (aqp_batch_sampling && i == 0)
            {
                state->partition_total_budget[i] = 0;
                state->partition_remaining_budget[i] = 0;

                if (state->gpsa_opt_sample_percent != NULL)
                    state->gpsa_opt_sample_percent[i] = 0.0;

                continue;
            }

            state->partition_total_budget[i] = state->gpsa_opt_sample_size[i];
            state->partition_remaining_budget[i] = state->gpsa_opt_sample_size[i];

            if (aqp_batch_sampling &&
                total_budget > 0 &&
                state->gpsa_opt_sample_percent != NULL)
            {
                state->gpsa_opt_sample_percent[i] =
                    (float8) state->gpsa_opt_sample_size[i] /
                    (float8) total_budget;
            }
        }
    }
}

static void 
aqp_pswr_progressive_build_each_round(AQPPSWRControlState *state)
{
    uint64 tot_round_sample_size = 0;
    int first_partition =
        (aqp_batch_sampling && !state->do_switch_to_uniform) ? 1 : 0;
    
    state->current_round_budget = 
        Min((uint64) aqp_progressive_round_size, 
            state->phase_remaining_budget);
    
    for (int i = 0; i < state->cur_phase_n_partitions; i++)
    {
        uint64 sample_size;

        state->partition_round_budget[i] = 0;

        if (i < first_partition)
            continue;

        if (state->partition_remaining_budget[i] == 0)
            continue;

        sample_size =
            ceil(state->current_round_budget *
                 state->gpsa_opt_sample_percent[i]);

        if (sample_size < 30)
        {
            if (state->partition_remaining_budget[i] >= 30)
                sample_size = 30;
            else
                sample_size = state->partition_remaining_budget[i];
        }

        if (sample_size > state->partition_remaining_budget[i])
            sample_size = state->partition_remaining_budget[i];

        state->partition_round_budget[i] = sample_size;
        tot_round_sample_size += sample_size;
    }

    state->current_round_budget = tot_round_sample_size;

    if (aqp_batch_sampling)
        state->cur_phase_sample_size = state->current_round_budget;

    aqp_pswr_progressive_checkpoint_round_start(state);

    /* elog(INFO,
        "progressive round: n_partitions = %d, round_sample_size_total = %lu, phase_remaining_budget = %lu",
        state->cur_phase_n_partitions,
        state->current_round_budget,
        state->phase_remaining_budget); */
}

static bool
aqp_pswr_progressive_check_ci_and_switch(AQPPSWRControlState *state)
{
    float8 current_ci;
    uint64 uniform_sample_size;
    uint64 remaining_budget;
    uint64 stratified_eff;
    uint64 uniform_eff;
    float8 ci_drop;
    float8 projected_final_ci;
    uint64 drain_threshold;
    bool drain_stratified;
    bool ci_growing;
    bool ci_too_slow;
    bool ci_no_improve;
    bool phase_regressed;
    bool bad_signal;
    bool uniform_competitive;
    bool switch_to_uniform;

    for (int i = 0; i < state->cur_phase_n_partitions; i++)
    {
        uint64 used = state->partition_round_budget[i];

        if (used > state->partition_remaining_budget[i])
            state->partition_remaining_budget[i] = 0;
        else
            state->partition_remaining_budget[i] -= used;
    }

    if (state->current_round_budget > state->phase_remaining_budget)
        state->phase_remaining_budget = 0;
    else
        state->phase_remaining_budget -= state->current_round_budget;

    state->stratified_probe_rounds++;

    current_ci = aqp_erf_inv(state->confidence0) *
                 sqrt(2 * state->transstate->var_total);
    /* elog(INFO, "switch ci: %f", aqp_erf_inv(state->confidence0) * sqrt(2 * state->transstate->var_total)); */
    if (current_ci <= state->err0 ||
        aqp_pswr_budget_spent(state) >= state->sample_budget)
    {
        state->last_phase = true;
        return false;
    }

    /*
     * Near the target, CI naturally changes slowly. Keep stratified sampling
     * instead of switching because of small/noisy CI changes.
     */
    if (current_ci <= state->err0 * AQP_PSWR_NEAR_TARGET_FACTOR)
    {
        state->progressive_last_ci = current_ci;
        state->progressive_bad_rounds = 0;
        return false;
    }

    remaining_budget = aqp_pswr_remaining_budget(state);

    if (remaining_budget == 0)
    {
        state->last_phase = true;
        return false;
    }

    uniform_sample_size = aqp_pswr_estimate_uniform_remaining_sample_size(state);
    state->uniform_phase_sample_size = uniform_sample_size;

    stratified_eff = Min(state->phase_remaining_budget, remaining_budget);
    uniform_eff =
        uniform_sample_size > 0 ?
        Min(uniform_sample_size, remaining_budget) : 0;

    ci_growing =
        state->progressive_last_ci > 0.0 &&
        current_ci >= state->progressive_last_ci;

    ci_drop = 0.0;
    projected_final_ci = current_ci;
    ci_too_slow = false;

    if (state->progressive_last_ci > 0.0 &&
        state->current_round_budget > 0 &&
        state->phase_remaining_budget > 0 &&
        current_ci > state->err0 * AQP_PSWR_NEAR_TARGET_FACTOR)
    {
        ci_drop = state->progressive_last_ci - current_ci;

        if (ci_drop > 0.0)
        {
            projected_final_ci =
                current_ci -
                ci_drop *
                ((float8) state->phase_remaining_budget /
                (float8) state->current_round_budget);

            ci_too_slow =
                projected_final_ci >
                state->err0 * AQP_PSWR_NEAR_TARGET_FACTOR;
        }
    }

    ci_no_improve = ci_growing || ci_too_slow;

    phase_regressed =
        state->phase_start_ci > 0.0 &&
        current_ci >
        state->phase_start_ci * AQP_PSWR_PHASE_REGRESSION_FACTOR;

    bad_signal = ci_no_improve || (phase_regressed && ci_growing);

    uniform_competitive =
        uniform_sample_size > 0 &&
        stratified_eff > 0 &&
        (float8) uniform_eff <=
        (float8) stratified_eff * AQP_PSWR_UNIFORM_COMPETITIVE_FACTOR;

    if (bad_signal)
        state->progressive_bad_rounds++;
    else
        state->progressive_bad_rounds = 0;

    drain_threshold = (uint64) aqp_progressive_round_size * 2;
    if (state->current_round_budget > drain_threshold)
        drain_threshold = state->current_round_budget;

    drain_stratified =
        state->phase_remaining_budget > 0 &&
        state->phase_remaining_budget <= drain_threshold &&
        !phase_regressed;

    switch_to_uniform =
        state->phase_remaining_budget > 0 &&
        !drain_stratified &&
        state->progressive_bad_rounds >= AQP_PSWR_MAX_BAD_PROGRESSIVE_ROUNDS;
    
    /* elog(INFO,
        "switch check: ci=%f prev_ci=%f target=%f probe_rounds=%d "
        "stratified_remaining=%lu uniform=%lu remaining=%lu "
        "ci_no_improve=%s ci_growing=%s ci_too_slow=%s projected_final_ci=%f "
        "phase_regressed=%s bad_rounds=%d "
        "uniform_competitive=%s drain_stratified=%s switch=%s",
        current_ci,
        state->progressive_last_ci,
        state->err0,
        state->stratified_probe_rounds,
        state->phase_remaining_budget,
        uniform_sample_size,
        remaining_budget,
        ci_no_improve ? "true" : "false",
        ci_growing ? "true" : "false",
        ci_too_slow ? "true" : "false",
        projected_final_ci,
        phase_regressed ? "true" : "false",
        state->progressive_bad_rounds,
        uniform_competitive ? "true" : "false",
        drain_stratified ? "true" : "false",
        switch_to_uniform ? "true" : "false"); */

    if (switch_to_uniform)
        aqp_pswr_progressive_rollback_bad_rounds(state);

    if (!switch_to_uniform)
        state->progressive_last_ci = current_ci;

    state->progressive_uniform_candidate = uniform_competitive;

    return switch_to_uniform;
}

static void
aqp_pswr_progressive_allocate_uniform_round(AQPPSWRControlState *state)
{
    AQPPSWRCtlInfo pswrctl_info = state->pswrctl_info;
    uint64 remaining_budget;
    uint64 raw_uniform_sample_size;
    uint64 round_sample_size;
    float8 current_ci;

    current_ci = aqp_erf_inv(state->confidence0) *
                 sqrt(2 * state->transstate->var_total);

    if (current_ci <= state->err0 ||
        aqp_pswr_budget_spent(state) >= state->sample_budget)
    {
        state->last_phase = true;
        return;
    }

    remaining_budget = aqp_pswr_remaining_budget(state);

    if (remaining_budget == 0)
    {
        state->last_phase = true;
        return;
    }

    /*
     * In uniform fallback, phase_remaining_budget is the remaining sample
     * count in the current uniform allocation.  Only estimate a new uniform
     * allocation after the previous one has been fully scheduled.
     */
    if (state->phase_remaining_budget == 0)
    {
        raw_uniform_sample_size = state->uniform_phase_sample_size;

        if (raw_uniform_sample_size == 0)
            raw_uniform_sample_size =
                aqp_pswr_estimate_uniform_remaining_sample_size(state);

        if (raw_uniform_sample_size == 0)
            raw_uniform_sample_size =
                Min((uint64) aqp_progressive_round_size, remaining_budget);

        state->uniform_phase_sample_size = raw_uniform_sample_size;
        state->phase_remaining_budget =
            Min(raw_uniform_sample_size, remaining_budget);

        /* elog(INFO,
             "uniform allocation: raw=%lu, capped=%lu, remaining=%lu, ci=%f target=%f",
             raw_uniform_sample_size,
             state->phase_remaining_budget,
             remaining_budget,
             current_ci,
             state->err0); */
    }

    round_sample_size =
        Min((uint64) aqp_progressive_round_size,
            state->phase_remaining_budget);
    round_sample_size = Min(round_sample_size, remaining_budget);

    if (round_sample_size == 0)
    {
        state->last_phase = true;
        return;
    }

    state->phase_remaining_budget -= round_sample_size;

    if (state->phase_remaining_budget == 0)
        state->uniform_phase_sample_size = 0;

    state->cur_phase_n_partitions = 1;
    state->cur_partition = 0;
    state->cur_phase_sample_size = round_sample_size;
    state->current_round_budget = round_sample_size;

    if (state->gpsa_opt_sample_percent != NULL)
        state->gpsa_opt_sample_percent[0] = 1.0;

    if (state->gpsa_opt_sample_size != NULL)
        state->gpsa_opt_sample_size[0] = round_sample_size;

    pswrctl_info->want_partition_key = false;
    pswrctl_info->current_scan_keys =
        pswrctl_info->subplan_info[pswrctl_info->cur_plan_id].scan_keys;
    pswrctl_info->n_current_scan_keys =
        pswrctl_info->subplan_info[pswrctl_info->cur_plan_id].n_scan_keys;
    pswrctl_info->sample_size = round_sample_size;

    /* elog(INFO,
         "uniform progressive round: ci=%f, target=%f",
         current_ci,
         state->err0); */
}

static void 
aqp_pswr_progressive_enter_uniform(AQPPSWRControlState *state)
{
    state->do_switch_to_uniform = true;
    if (aqp_batch_sampling)
    {
        for (int i = 0; i < state->ntrans; ++i)
        {
            AQPApproxAggTransState *ts = &state->transstate[i];

            ts->mu_phase = ts->mu_total;
            ts->var_phase = ts->var_total;

            ts->n = 0;
            ts->mu = 0.0;
            ts->var = 0.0;

            ts->mu_part[0] = 0.0;
            ts->var_part[0] = 0.0;
            ts->n_part[0] = 0;

            if (aqp_pswr_tree && ts->height_part != NULL)
                ts->height_part[0] = 0.0;

            ts->flags = AQP_INTERNAL_PAGE_STATISTICS;
        }

        state->initial_and_uniform_sample_size = 0;
        state->continue_descent = 3;
        state->continue_descent_dp = 0;
        state->descent_try_sample_size = 0;
    }

    state->progressive_current_phase_enabled = false;
    state->progressive_uniform_candidate = false;
    state->cur_phase_n_partitions = 1;
    state->cur_partition = 0;

    state->phase_remaining_budget = 0;

    elog(INFO, "switching to uniform sampling.");

    aqp_pswr_progressive_allocate_uniform_round(state);
}

void
aqp_setup_pswrctl_node(void)
{
    RegisterExtensibleNodeMethods(&aqp_pswrctl_path_private_methods);
    RegisterExtensibleNodeMethods(&aqp_pswrctl_private_methods);
    RegisterCustomScanMethods(&aqp_pswrctl_methods);
}

