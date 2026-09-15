#include "aqp.h"

#include <miscadmin.h>
#include <nodes/makefuncs.h>
#include <nodes/nodeFuncs.h>
#include <nodes/print.h>
#include <optimizer/planner.h>
#include <parser/parse_func.h>
#include <utils/regproc.h>
#include <access/table.h>
#include <utils/guc.h>
#include <utils/lsyscache.h>
#include <utils/rel.h>

#include "aqp_planner.h"
#include "aqp_swrscan.h"
#include "aqp_samplepath.h"
#include "aqp_pswrctl_node.h"

typedef struct AQPSampleProbRewriterContext
{
    List *rtable;
    Plan* swr_scan;
    Plan* first_node_with_sample_prob;
    bool in_first_sample_prob_subtree;
    bool is_sample_func;
    int natts;
    List *swr_scans;
    int num_swr_scans;
} AQPSampleProbRewriterContext;

typedef struct 
{
    Var *left_var;
    Var *right_var;
} AQPJoinProbContext;

typedef struct AQPCheckFuncExistsContext
{
    Oid funcid;
    bool found;
} AQPCheckFuncExistsContext;

typedef struct
{
    Index scanrelid;
    int natts;
} AQPRewriteDummyVarInAQPSWRScanForExplainContext;


typedef struct
{
    Var *dummy_sample_prob_var;
    Param *dummy_running_sample_size_param;
    Param *dummy_running_sample_budget_param;
    Param *dummy_running_state_id_param;
} AQPRewriteDummyFuncExpr;

typedef struct AQPPostPlannerRewriteContext
{
    bool sampler_found;
    int pswrctl_info_paramid;
} AQPPostPlannerRewriteContext;


static PlannedStmt* aqp_planner(Query* parse, 
                                const char* query_string,
                                int cursorOptions,
                                ParamListInfo boundParams);
static void aqp_sample_prob_rewriter(Plan *plan, List *rtable);
static void aqp_sample_prob_rewriter_imp(Plan *plan,
                                         AQPSampleProbRewriterContext *cxt);
static Var *aqp_create_sample_prob_var(List *targetlist, Index var_no);
static Node *aqp_replace_all_dummy_funcexpr(Node *node, AQPRewriteDummyFuncExpr *new_func);

static void aqp_locate_dummy_funcexpr_and_rewrite_swr_scan(
                Plan *plan, AQPSampleProbRewriterContext *cxt);
static bool aqp_check_func_exists(Node *node, Oid oid);
static bool aqp_check_func_exists_imp(Node *node,
                                      AQPCheckFuncExistsContext *cxt);
static AttrNumber aqp_find_table_natts(AQPSampleProbRewriterContext *cxt,
                                       Index scanrelid);
static Var *aqp_find_sample_prob_var(List *targetlist, Index var_no);
static Expr *aqp_create_prob_multiply_expr(Var *left_var, Var *right_var);
static Node *aqp_replace_sample_prob_with_join_prob(Node *node, Var *left_var, Var *right_var);
static Node *aqp_replace_sample_prob_with_join_prob_walker(Node *node, AQPJoinProbContext *cxt);
static void aqp_append_targetlist_entry(Plan *plan, Var *var);
static Plan *aqp_remove_materialize_above_swr(Plan *plan);
static Node* aqp_rewrite_dummy_var_in_aqp_swrscan_for_explain(Node *node,
                                                              Index scanrelid,
                                                              int natts);
static Node* aqp_rewrite_dummy_var_in_aqp_swrscan_for_explain_impl(
    Node *node,
    AQPRewriteDummyVarInAQPSWRScanForExplainContext *cxt);
static bool aqp_lookup_fn_oids(void);
static Oid aqp_lookup_fn_oid(const char *funcname, int nargs,
                             const Oid *argtypes);
static Plan *aqp_post_planner_rewrite(Plan *plan, PlannedStmt *stmt);
static void aqp_post_planner_rewrite_impl(Plan *plan,
                                          AQPPostPlannerRewriteContext *ctx);

static bool aqp_query_has_swr(Query *query);
static bool aqp_query_has_pswr(Query *query);
static bool aqp_query_has_agg(Query *query);
static Aggref *aqp_find_first_aggref_in_node(Node *node);
static bool aqp_query_first_agg_supports_new_impl(Query *query);

static planner_hook_type prev_planner_hook = NULL;

bool aqp_fn_oid_cached = false;
Oid aqp_sample_prob_oid = InvalidOid;
Oid aqp_running_sample_size_oid = InvalidOid;
Oid aqp_running_sample_budget_oid = InvalidOid;
Oid aqp_running_state_id_oid = InvalidOid;
Oid aqp_swr_tsm_handler_oid = InvalidOid;
Oid aqp_pswr_tsm_handler_oid = InvalidOid;
Oid aqp_sum_float8_oid = InvalidOid;
Oid aqp_float8_mul_opno = InvalidOid;
Oid aqp_float8_mul_funcid = InvalidOid;
Oid aqp_float8_div_opno = InvalidOid;
Oid aqp_float8_div_funcid = InvalidOid;
Oid aqp_float48_div_opno = InvalidOid;
Oid aqp_float48_div_funcid = InvalidOid;
Oid aqp_erf_inv_oid = InvalidOid;

Oid aqp_approx_sum_oid = InvalidOid;
Oid aqp_approx_sum_half_ci_oid = InvalidOid;
Oid aqp_approx_count_oid = InvalidOid;
Oid aqp_approx_count_half_ci_oid = InvalidOid;
Oid aqp_approx_progressive_count_half_ci_oid = InvalidOid;
Oid aqp_approx_count_any_oid = InvalidOid;
Oid aqp_approx_count_any_half_ci_oid = InvalidOid;

Oid aqp_float8_accum_oid = InvalidOid;
Oid aqp_clt_half_ci_final_func_oid = InvalidOid;

Oid aqp_progressive_float8_accum_oid = InvalidOid;
Oid aqp_progressive_clt_half_ci_final_func_oid = InvalidOid;

Oid aqp_approx_sum_internal_oid;
Oid aqp_approx_sum_internal_accum_oid;
Oid aqp_approx_sum_internal_final_oid;
Oid aqp_approx_sum_clt_half_ci_internal_final_oid;

void
aqp_setup_planner_hook(void)
{
    prev_planner_hook = planner_hook;
    planner_hook = aqp_planner;
}

static PlannedStmt*
aqp_planner(Query* parse, 
            const char* query_string,
            int cursorOptions,
            ParamListInfo boundParams)
{
    PlannedStmt *plan;
    bool need_post_planner_rewrite = false;

    if (!aqp_lookup_fn_oids())
    {
        ereport(WARNING,
                errcode(ERRCODE_INTERNAL_ERROR),
                errmsg("aqp extension functions not found. "
                       "Plan rewriter disabled."));
    }


    /* check if need to switch to old impl */
    aqp_use_new_agg_impl = false;
 
    if (aqp_enable_new_agg_impl && aqp_fn_oid_cached)
    {
        if (aqp_query_has_pswr(parse))
        {
            if (!aqp_query_has_agg(parse) ||
                !aqp_query_first_agg_supports_new_impl(parse))
            {
               elog(ERROR, "TABLESAMPLE pswr() only supports approximate aggregate."); 
            }
            aqp_use_new_agg_impl = true;
        }
        else if (aqp_query_has_swr(parse) &&
                 aqp_query_has_agg(parse) &&
                 aqp_query_first_agg_supports_new_impl(parse))
        {
            aqp_use_new_agg_impl = true;
        }
    }

    /* 
     * We rewrite all approx_xxx aggregate functions before query planning.
     */
    if (aqp_fn_oid_cached)
    {
        need_post_planner_rewrite = aqp_check_and_rewrite_approx_agg(parse);

        if (Debug_print_parse || Debug_print_rewritten)
        {
            elog_node_display(LOG, "rewritten parse tree by module AQP",
                              parse, Debug_pretty_print);
        }
    }

    /*
        remember the previous hook then hook the new planner 
    */
    if(prev_planner_hook)
        plan = prev_planner_hook(parse, query_string,
                                 cursorOptions, boundParams);
    else
        plan = standard_planner(parse, query_string,
                                cursorOptions, boundParams);
    
    if (aqp_fn_oid_cached && need_post_planner_rewrite)
    {
        ListCell *lc;

        foreach(lc, plan->subplans)
        {
            Plan *subplan = (Plan *) lfirst(lc);
            //aqp_fix_swrscan_exprs(subplan);
            //aqp_sample_prob_rewriter(subplan, plan->rtable);
            subplan = aqp_post_planner_rewrite(subplan, plan);
            lfirst(lc) = subplan; 
        }

        plan->planTree = aqp_post_planner_rewrite(plan->planTree, plan);
    }

    //elog_node_display(NOTICE, "final plan", plan, true);
    return plan;
}

static Plan *
aqp_post_planner_rewrite(Plan *plan, PlannedStmt *stmt)
{
    /*
        remove any materialize nodes the planner inserted directly aboove aqp swr scan nodes. this must happen before the sample_prob rewriter so that the nestloop's children are the raw swr scans rather than wrapped materialize nodes.
      */
//    plan = aqp_remove_materialize_above_swr(plan);

    if (!aqp_use_new_agg_impl)
    {
        /* this is the old impl */
        aqp_fix_swrscan_exprs(plan);
        aqp_sample_prob_rewriter(plan, stmt->rtable);
        return plan;
    }

    /* 
     * In the new impl, let's combine fix_swrscan_exprs traversals and that for
     * pswrctl into one.
     */
    aqp_post_planner_rewrite_impl(plan, NULL);

    /* 
     * sample_prob() rewriter is a bit messy right now. Instead of fixing it,
     * let's invoke this as a separate pass. Since we're applying the new
     * aggregation rewriting rules, there shouldn't be any explicit
     * sample_prob() calls unless the user writes so in the query clauses.
     *
     * TODO the way of rewriting of these should be unified with how we pass
     * sample probability and other sampling states into the pswrctl node.
     */
    aqp_sample_prob_rewriter(plan, stmt->rtable);

    return plan;
}

/* 
 * implement the plan rewriting logic for
 * 1) If there's a tablesample swr, add an extra output probability.  Note
 * that, tablesample swr should have been transformed into a swr scan,
 * swr index only scan node, or progressive swr node at this point.
 *
 * 2) For each node at or above the SWRScan, SWRIndexOnlyScan or progressive
 * swr node that is below the highest node in the tree (closest to the tree
 * root) that has some expression calling sample_prob(), append a new target to
 * its target list that references the probability column in its subplan (or
 * indextlist).  However, do not descend into sub-queries.
 *
 * 3) If sample_prob() appears in any expression, replace it with a reference
 * to the probability output from its child plan (or indextlist).
 */
static void
aqp_sample_prob_rewriter(Plan *plan, List *rtable){
    AQPSampleProbRewriterContext cxt;
    cxt.rtable = rtable;
    cxt.first_node_with_sample_prob = NULL;
    cxt.is_sample_func = false;
    cxt.swr_scan = NULL;
    cxt.natts = 0;
    cxt.swr_scans = NIL;
    cxt.num_swr_scans = 0;
    aqp_sample_prob_rewriter_imp(plan, &cxt);
}


static void
aqp_sample_prob_rewriter_imp(Plan *plan, AQPSampleProbRewriterContext *cxt)
{
    Plan *swr_scan;
    AQPRewriteDummyFuncExpr new_func;
        
    if (plan == NULL)
    {
        return;
    }
   
    /* for the case of subquery scan in from list */
    if (IsA(plan, SubqueryScan))
    {
        /* rewrite subplan */
        SubqueryScan *subscan = (SubqueryScan *) plan;
        Plan *subplan = subscan->subplan;
        
        /* 
         * Be sure to call the rewriter with a fresh cxt because subplan
         * is independent from the parent plan.
         */
        aqp_sample_prob_rewriter(subplan, cxt->rtable);
    }

    /* 
     * If we found swr_scan in a slibling subtree, we will notice a non-NULL
     * cxt->swr_scan at this point. If we found a target list entry that calls
     * sample_prob() here, it is unclear what this sample_prob() refers to
     * since we would disallow having two tablesample clauses in the same query
     * block.
     *
     * XXX Subquery pull-up can indeed result in such a situation and this
     * should be fixed by tagging the sample_prob() calls with correct
     * references to the sampled tables.
     */


    /*
            if the sibling subtree already found a swr scan but this node's targetlist still calls sample_prob(), it is an error
           for wj the second scan is tracked in swr_scans, so num_swr_scans will be >= 1 once we start and we skip this check
      */
    if (cxt->swr_scan != NULL && cxt -> num_swr_scans == 0)
    {
        if (aqp_check_func_exists((Node*) plan->targetlist, aqp_sample_prob_oid))
        {
            elog(ERROR, "sample_prob() appears without swr scan in subtree");    
        }
    }

    /*
     * do something before goto lefttree
     * wirte dow isn, check if sample prob, 
     * if have sample prob && first node with sample prob is null, 
     * remeber as first smaple probe
     */
    aqp_locate_dummy_funcexpr_and_rewrite_swr_scan(plan, cxt);
    
    //write down wheather there is a index scan node before we go to left or right
    //if the node is not a index scan node;
    swr_scan = cxt->swr_scan;
    //get into left tree
    aqp_sample_prob_rewriter_imp(plan->lefttree, cxt);
    aqp_sample_prob_rewriter_imp(plan -> righttree, cxt);
    
    //get the param
    
    if(IsA(plan, Hash) || IsA(plan, Material)) {
        Plan *child = plan -> lefttree;
        if(child != NULL && list_length(child -> targetlist) > 0) {
            TargetEntry *last = llast_node(TargetEntry, child -> targetlist);
            if(last -> resname && strcmp(last -> resname, "sample_prob") == 0) 
            {
                Var *pv = makeVar(OUTER_VAR, last -> resno, FLOAT8OID, 0, InvalidOid, 0);
                aqp_append_targetlist_entry(plan, pv);
            }
        }
        goto check_and_finish;
    }

    /*
        wj case: two swr scans under a single nestloop. multiply their individual sampling probs to get their
        join prob and expose it as the "sample_prob" column of this plan node.
    */
    if(IsA(plan, NestLoop) && cxt -> num_swr_scans >= 2 && cxt -> first_node_with_sample_prob != NULL && cxt -> in_first_sample_prob_subtree && plan -> lefttree != NULL && plan -> righttree != NULL) {
        Var *left_prob_var = aqp_find_sample_prob_var(plan -> lefttree -> targetlist, OUTER_VAR);
        Var *right_prob_var = aqp_find_sample_prob_var(plan -> righttree -> targetlist, INNER_VAR);
        if(left_prob_var != NULL && right_prob_var != NULL) {
            Expr *mul_expr = aqp_create_prob_multiply_expr(left_prob_var, right_prob_var);
            if(plan != cxt -> first_node_with_sample_prob) {
                TargetEntry *mul_entry = makeNode(TargetEntry);
                mul_entry -> expr = mul_expr;
                mul_entry -> resno = list_length(plan -> targetlist) + 1;
                mul_entry -> resname = pstrdup("sample_prob");
                mul_entry -> ressortgroupref = 0;
                mul_entry -> resorigcol = InvalidAttrNumber;
                mul_entry -> resorigtbl = InvalidOid;
                mul_entry -> resjunk = false;
                plan -> targetlist = lappend(plan -> targetlist, mul_entry);
            }
            plan -> targetlist = (List *) aqp_replace_sample_prob_with_join_prob((Node *) plan -> targetlist, left_prob_var, right_prob_var);
            pfree(left_prob_var);
            pfree(right_prob_var);
            goto check_and_finish;
        }
        if(left_prob_var) {
            pfree(left_prob_var);
        }
        if(right_prob_var) {
            pfree(right_prob_var);
        }

        // fall thru to single scan prop below;
    }

    //case 2.1, before leftree ios is null, after enter left tree ios is not null
    //the ios is on lefttree
    if(swr_scan == NULL && cxt->swr_scan != NULL &&
        cxt->first_node_with_sample_prob != NULL &&
        cxt->in_first_sample_prob_subtree)
    {
        Var *sample_prob_var = NULL;

        /* Prefer left child; fall back to right child. */
        if (plan->lefttree != NULL)
            sample_prob_var =
                aqp_find_sample_prob_var(plan->lefttree->targetlist,
                                        OUTER_VAR);
        if (sample_prob_var == NULL && plan->righttree != NULL)
            sample_prob_var =
                aqp_find_sample_prob_var(plan->righttree->targetlist,
                                        INNER_VAR);

        if (sample_prob_var != NULL)
        {
            if (plan != cxt->first_node_with_sample_prob)
            {
                TargetEntry *tle =
                    makeTargetEntry((Expr *) sample_prob_var,
                                    list_length(plan->targetlist) + 1,
                                    pstrdup("sample_prob"),
                                    false);
                plan->targetlist = lappend(plan->targetlist, tle);
            }

            /* Build the PSWR-aware replacement descriptor. */
            memset(&new_func, 0, sizeof(new_func));
            new_func.dummy_sample_prob_var = sample_prob_var;
            if (aqp_plan_is_index_sample_scan(cxt->swr_scan))
            {
                CustomScan *cscan = (CustomScan *) cxt->swr_scan;
                AQPIndexSampleScanPrivate *priv =
                    (AQPIndexSampleScanPrivate *)
                    linitial(cscan->custom_private);
                if (AQPISSIsPSWRSubPlan(priv))
                {
                    AQPProgressiveSampleScanPrivate *pssp =
                        (AQPProgressiveSampleScanPrivate *) priv;
                    new_func.dummy_running_sample_size_param   =
                        pssp->running_sample_size_param;
                    new_func.dummy_running_sample_budget_param =
                        pssp->running_sample_budget_param;
                    new_func.dummy_running_state_id_param      =
                        pssp->running_state_id_param;
                }
            }

            plan->targetlist = (List *)
                aqp_replace_all_dummy_funcexpr(
                    (Node *) plan->targetlist, &new_func);

            if (plan != cxt->first_node_with_sample_prob)
                pfree(sample_prob_var);
        }
    }

check_and_finish:
    if (cxt->swr_scan == NULL &&
        (cxt->first_node_with_sample_prob == plan ||
         aqp_check_func_exists((Node *) plan->targetlist,
                               aqp_sample_prob_oid)))
    {
        elog(ERROR, "sample_prob() appears without swr scan in subtree");
    }

    if (cxt->first_node_with_sample_prob == plan)
        cxt->in_first_sample_prob_subtree = false;
}
static Var *
aqp_create_sample_prob_var(List *targetlist, Index var_no){
    TargetEntry *target_entry;
    Var *new_var;
    Assert(list_length(targetlist) > 0);
    target_entry = llast_node(TargetEntry, targetlist);
    if(IsA(target_entry -> expr, Var)) {
        new_var = (Var *) copyObject(target_entry -> expr);
        new_var -> varno = var_no;
        new_var -> varattno = target_entry -> resno;
    }else {
        //non var entry
        new_var = makeVar(var_no, target_entry -> resno, FLOAT8OID, 0, InvalidOid, 0);
    }
    return new_var;
}

// wirte a fun to go over TARGETLIST only. target list replace funcexpr with a VAR, where 
// var no is the resno from  
static Node *
aqp_replace_all_dummy_funcexpr(Node *node, AQPRewriteDummyFuncExpr *new_func){
    if (node == NULL)
        return NULL;
    if(IsA(node, FuncExpr)){
        FuncExpr *  func_node =  (FuncExpr*) node;
        if(func_node->funcid == aqp_sample_prob_oid)
        {
            Var *new_node = copyObject(new_func->dummy_sample_prob_var);
            return (Node *) new_node;
        }
        if (new_func->dummy_running_sample_size_param != NULL)
        {
            if(func_node->funcid == aqp_running_sample_size_oid)
            {
                Param *new_node = copyObject(new_func->dummy_running_sample_size_param);
                return (Node *) new_node;
            }
        }
        if (new_func->dummy_running_sample_budget_param != NULL)
        {
            if(func_node->funcid == aqp_running_sample_budget_oid)
            {
                Param *new_node = copyObject(new_func->dummy_running_sample_budget_param);
                return (Node *) new_node;
            }
        }
        if (new_func->dummy_running_state_id_param != NULL)
        {
            if(func_node->funcid == aqp_running_state_id_oid)
            {
                Param *new_node = copyObject(new_func->dummy_running_state_id_param);
                return (Node *) new_node;
            }
        }
    }
    return expression_tree_mutator(node, aqp_replace_all_dummy_funcexpr,
                                   new_func);
}

/*
 * Find the sample_prob output column in a plan's target list and return a
 * copy of it as a Var with the given varno (OUTER_VAR or INNER_VAR).
 *
 * Searches first for a TargetEntry with resname "sample_prob".  If the
 * matching entry's expr is not a Var (e.g., it is the WanderJoin product
 * FuncExpr), a fresh Var with the correct varattno is created.  Falls back
 * to the last entry if no named entry is found.  Returns NULL if not found.
 */
static Var *
aqp_find_sample_prob_var(List *targetlist, Index var_no)
{
    ListCell   *lc;

    foreach(lc, targetlist)
    {
        TargetEntry *tle = lfirst_node(TargetEntry, lc);
        if (tle->resname && strcmp(tle->resname, "sample_prob") == 0)
        {
            Var *new_var;
            if (IsA(tle->expr, Var))
            {
                new_var = (Var *) copyObject(tle->expr);
                new_var->varno    = var_no;
                new_var->varattno = tle->resno;
            }
            else
            {
                new_var = makeVar(var_no, tle->resno,
                                  FLOAT8OID, 0, InvalidOid, 0);
            }
            return new_var;
        }
    }
    /* Fallback: last entry if it is a Var. */
    if (targetlist != NIL)
    {
        TargetEntry *tle = llast_node(TargetEntry, targetlist);
        if (IsA(tle->expr, Var))
        {
            Var *new_var = (Var *) copyObject(tle->expr);
            new_var->varno    = var_no;
            new_var->varattno = tle->resno;
            return new_var;
        }
    }
    return NULL;
}

/* Build float8 * float8 expression representing the joint sample probability. */
static Expr *
aqp_create_prob_multiply_expr(Var *left_var, Var *right_var)
{
    FuncExpr *mul = makeNode(FuncExpr);
    mul->funcid        = aqp_float8_mul_funcid;
    mul->funcresulttype = FLOAT8OID;
    mul->funcretset    = false;
    mul->funcvariadic  = false;
    mul->funcformat    = COERCE_EXPLICIT_CALL;
    mul->funccollid    = InvalidOid;
    mul->inputcollid   = InvalidOid;
    mul->args          = list_make2(copyObject(left_var), copyObject(right_var));
    mul->location      = -1;
    return (Expr *) mul;
}

/* Replace sample_prob() FuncExpr nodes with left_var * right_var. */
static Node *
aqp_replace_sample_prob_with_join_prob(Node *node, Var *left_var, Var *right_var)
{
    AQPJoinProbContext cxt;
    cxt.left_var  = left_var;
    cxt.right_var = right_var;
    if (node == NULL)
        return NULL;
    if (IsA(node, FuncExpr) &&
        ((FuncExpr *) node)->funcid == aqp_sample_prob_oid)
        return (Node *) aqp_create_prob_multiply_expr(left_var, right_var);
    return expression_tree_mutator(
        node, aqp_replace_sample_prob_with_join_prob_walker, &cxt);
}

static Node *
aqp_replace_sample_prob_with_join_prob_walker(Node *node,
                                               AQPJoinProbContext *cxt)
{
    if (node == NULL)
        return NULL;
    if (IsA(node, FuncExpr) &&
        ((FuncExpr *) node)->funcid == aqp_sample_prob_oid)
        return (Node *)
            aqp_create_prob_multiply_expr(cxt->left_var, cxt->right_var);
    return expression_tree_mutator(
        node, aqp_replace_sample_prob_with_join_prob_walker, cxt);
}

/* Append a Var as a "sample_prob" TargetEntry to plan's target list. */
static void
aqp_append_targetlist_entry(Plan *plan, Var *var)
{
    TargetEntry *tle = makeNode(TargetEntry);
    tle->expr              = (Expr *) var;
    tle->resno             = list_length(plan->targetlist) + 1;
    tle->resname           = pstrdup("sample_prob");
    tle->ressortgroupref   = 0;
    tle->resorigcol        = InvalidAttrNumber;
    tle->resorigtbl        = InvalidOid;
    tle->resjunk           = false;
    plan->targetlist = lappend(plan->targetlist, tle);
}

/*
 * Remove any Materialize node inserted by the planner directly above an
 * AQP SWR scan.  PostgreSQL sometimes adds a Materialize as a buffer, but
 * it breaks the sample-probability passthrough for WanderJoin.
 */
static Plan *
aqp_remove_materialize_above_swr(Plan *plan)
{
    if (plan == NULL)
        return NULL;
    if (IsA(plan, Material))
    {
        Plan *child         = plan->lefttree;
        Plan *rewritten     = aqp_remove_materialize_above_swr(child);
        if (aqp_plan_is_index_sample_scan(rewritten))
            return rewritten;
        plan->lefttree = rewritten;
        return plan;
    }
    if (IsA(plan, SubqueryScan))
    {
        SubqueryScan *ss = (SubqueryScan *) plan;
        ss->subplan = aqp_remove_materialize_above_swr(ss->subplan);
        return plan;
    }
    plan->lefttree  = aqp_remove_materialize_above_swr(plan->lefttree);
    plan->righttree = aqp_remove_materialize_above_swr(plan->righttree);
    return plan;
}

static void
aqp_rewrite_swr_scan(CustomScan *cscan,
                     AQPIndexSampleScanPrivate *issp,
                     AQPSampleProbRewriterContext *cxt)
{
    List **p_qptlist;
    Var *sp_var;
    bool is_index_only = AQPISSIsIndexOnly(issp);
    bool is_pswr_subplan = AQPISSIsPSWRSubPlan(issp);
    AQPRewriteDummyFuncExpr new_func;

    if (is_pswr_subplan)
    {
        AQPProgressiveSampleScanPrivate *pssp =
            (AQPProgressiveSampleScanPrivate *) issp;
        p_qptlist = &pssp->qptlist;
        new_func.dummy_running_sample_size_param = pssp->running_sample_size_param;
        new_func.dummy_running_sample_budget_param = pssp->running_sample_budget_param;
        new_func.dummy_running_state_id_param = pssp->running_state_id_param;
    }
    else
    {
        p_qptlist = &cscan->scan.plan.targetlist;
        new_func.dummy_running_sample_size_param = NULL;
        new_func.dummy_running_sample_budget_param = NULL;
        new_func.dummy_running_state_id_param = NULL;
    }

    if (is_index_only)
    {
        List **p_indextlist;
        TargetEntry *sp_entry;

        if (is_pswr_subplan)
        {
            AQPProgressiveSampleScanPrivate *pssp =
                (AQPProgressiveSampleScanPrivate *) issp;
            p_indextlist = &pssp->indextlist;
        }
        else
        {
            p_indextlist = &cscan->custom_scan_tlist;
        }

        sp_var = makeVar(INDEX_VAR,
                         AQPSampleProbAttributeNumber,
                         FLOAT8OID,
                         0,
                         InvalidOid,
                         0);
        sp_entry = makeTargetEntry((Expr *) sp_var,
                                   list_length(*p_indextlist) + 1,
                                   pstrdup("sample_prob"),
                                   false);
        *p_indextlist = lappend(*p_indextlist, sp_entry);
        
        /* reference to sample prob as the last index column */
        sp_var = copyObject(sp_var);
        sp_var->varattno = sp_entry->resno;
    }
    else
    {
        AttrNumber natts = aqp_find_table_natts(cxt,
                                                cscan->scan.scanrelid);
        sp_var = makeVar(cscan->scan.scanrelid,
                         natts + 1,
                         FLOAT8OID,
                         0,
                         InvalidOid,
                         0);
    }

    new_func.dummy_sample_prob_var = sp_var;
    
    /* 
     * If the target list has any sample_prob() call, we need to replace it.
     * Note that, this is necessary because we don't have subtrees. That means,
     * cxt->swr_scan is alreay found before desceding into subtrees, and thus
     * the code to rewrite sample_prob() won't be triggered.
     */
    *p_qptlist = (List *) aqp_replace_all_dummy_funcexpr(
            (Node *) *p_qptlist, &new_func);

    if (cxt->first_node_with_sample_prob != (Plan *) cscan)
    {
        /* 
         * This is not the first node with a sample_prob() call, so we'll need
         * to add one extra target entry to return the sample_prob column to
         * the parent.
         */
        TargetEntry *tle = makeTargetEntry((Expr *) sp_var,
                                           list_length(*p_qptlist) + 1,
                                           pstrdup("sample_prob"),
                                           false);
        *p_qptlist = lappend(*p_qptlist, tle);
    }
    else
    {
        pfree(sp_var);
    }
}

static void 
aqp_locate_dummy_funcexpr_and_rewrite_swr_scan(Plan *plan,
                                               AQPSampleProbRewriterContext *cxt)
{
    CustomScan  *cscan;
    AQPIndexSampleScanPrivate *private;

    /*
     * The goal of this function is to (1) locate any sample_prob() call in the
     * target list if we have not found such a node up in the tree; (2) rewrite
     * an index sample scan node such that it has an extra sampling probability
     * column, if we have found a sample_prob() call somewhere up in the tree.
     * In most cases, this is somewhere up in the tree so this should not be
     * NULL.  However, it is also possible that this node is indeed index
     * sample scan node and it is the only node in the tree that refer to
     * sample_prob(), e.g., a single-table sample-project query.
     *
     * Therefore, If we haven't found the highest node in the tree that has a
     * sample_prob() call, try if we can find it in the current plan node first.
     */ 
    
    if (cxt->first_node_with_sample_prob == NULL &&
        aqp_check_func_exists((Node *) plan->targetlist, aqp_sample_prob_oid))
    {
        cxt->first_node_with_sample_prob = plan;
        cxt->in_first_sample_prob_subtree = true;
    }
    
    /* Not an index sample scan node -- skip it. */
    if (!aqp_plan_is_index_sample_scan(plan))
        return;

    /*
     * Second SWR scan encountered.  For single-table queries this is an
     * error.  For WanderJoin we track both scans and add the prob column to
     * the second scan just like we did for the first.
     */
    if (cxt->swr_scan != NULL)
    {
        if (cxt->first_node_with_sample_prob != NULL)
        {
            /* WanderJoin: second scan found — track and rewrite it. */
            if (cxt->num_swr_scans == 0)
            {
                /* First scan was not yet tracked; add it now. */
                cxt->swr_scans = lappend(cxt->swr_scans, cxt->swr_scan);
                cxt->num_swr_scans++;
            }
            cxt->swr_scans = lappend(cxt->swr_scans, plan);
            cxt->num_swr_scans++;
            cxt->swr_scan = plan;

            /* Add the sampling-probability column to the second scan. */
            cscan = (CustomScan *) plan;
            Assert(cscan->custom_private != NIL);
            private = (AQPIndexSampleScanPrivate *)
                linitial(cscan->custom_private);
            if (!AQPISSIsPSWRSubPlan(private))
                aqp_rewrite_swr_scan(cscan, private, cxt);
            return;
        }
        /* No sample_prob() requested — WanderJoin case, skip silently. */
        return;
    }

    /* First SWR scan found. */
    cxt->swr_scan = plan;
    if (cxt->first_node_with_sample_prob != NULL)
    {
        /* Track for WanderJoin detection. */
        cxt->swr_scans = lappend(cxt->swr_scans, plan);
        cxt->num_swr_scans++;
    }
    
    /* 
     * No one up in the tree (including this node) asks for the sampling
     * probability -- no rewriting needed. Just record that fact that we have
     * found the index sample scan node (for checking whether we found another
     * one in a sibling subtree of some ancestor).
     */
    if (cxt->first_node_with_sample_prob == NULL)
        return;
    
    /* We do need to append the sampling probability column if we get here. */
    cscan = (CustomScan *) plan;
    Assert(cscan->custom_private != NIL);
    private = (AQPIndexSampleScanPrivate *) linitial(cscan->custom_private);

    if (!AQPISSIsPSWRSubPlan(private))
    {
        aqp_rewrite_swr_scan(cscan, private, cxt);
    }
    else
    {
        ListCell *lc;

        foreach(lc, cscan->custom_private)
        {
            AQPIndexSampleScanPrivate *issp =
                (AQPIndexSampleScanPrivate *) lfirst(lc);
            aqp_rewrite_swr_scan(cscan, issp, cxt);
        }
        
        /* 
         * We also need to append a dummy sample_prob Var to the original
         * qptlist if some node up in the tree needs it. Note that this must be
         * a Var rather than a sample_prob() call because the parent node
         * assumes this is a Var that they could copy and modify as a reference
         * to the sample prob column.
         *
         * However, this means the fixes for explain also needs to transform
         * this dummy Var into a FuncExpr of sample_prob().
         */
        if (cxt->first_node_with_sample_prob != (Plan *) cscan)
        {
            List *qptlist = cscan->scan.plan.targetlist;
            int natts = aqp_find_table_natts(cxt, cscan->scan.scanrelid);
            Var *sp_var = makeVar(cscan->scan.scanrelid,
                                  natts + 1,
                                  FLOAT8OID,
                                  0,
                                  InvalidOid,
                                  0);
            TargetEntry *tle = makeTargetEntry((Expr *) sp_var,
                                               list_length(qptlist) + 1,
                                               pstrdup("sample_prob"),
                                               false);
            cscan->scan.plan.targetlist = lappend(qptlist, tle);
        }
    }
}
static bool
aqp_check_func_exists(Node *node, Oid funcid){
    AQPCheckFuncExistsContext cxt;
    cxt.funcid = funcid;
    cxt.found = false;
    (void) expression_tree_walker(node, aqp_check_func_exists_imp, &cxt);
    return cxt.found;
}


/*
 * check if this funcexpr is the function
 */
static bool
aqp_check_func_exists_imp(Node *node, AQPCheckFuncExistsContext *cxt)
{
    if (node == NULL)
        return false;
    if (IsA(node, FuncExpr) && ((FuncExpr *) node)->funcid == cxt->funcid)
    {
        cxt->found = true;
        return true;
    }
    return expression_tree_walker(node, aqp_check_func_exists_imp, cxt);
}

static AttrNumber
aqp_find_table_natts(AQPSampleProbRewriterContext *cxt, Index scanrelid){
    RangeTblEntry *relational_rte =
        list_nth_node(RangeTblEntry, cxt->rtable, scanrelid-1);
    Relation  relation = table_open(relational_rte->relid, NoLock);
    TupleDesc descr    = RelationGetDescr(relation);
    AttrNumber natts   = descr->natts;
    table_close(relation, NoLock);
    return natts;
}

void
aqp_rewrite_sample_prob_dummy_var_for_explain(PlanState *state)
{
    check_stack_depth();

    if (state == NULL)
        return ;

    if (aqp_plan_is_index_sample_scan(state->plan))
    {
        CustomScan *cscan = (CustomScan *) state->plan;
        ListCell *lc;
        bool is_pswr = false;

        foreach(lc, cscan->custom_private)
        {
            AQPIndexSampleScanPrivate *private =
                (AQPIndexSampleScanPrivate *) lfirst(lc);
            if (AQPISSIsIndexOnly(private))
            {
                List *indextlist;
                if (AQPISSIsPSWRSubPlan(private))
                {
                    indextlist = ((AQPProgressiveSampleScanPrivate *) private)
                        ->indextlist;
                    is_pswr = true;
                }
                else
                {
                    indextlist = cscan->custom_scan_tlist;
                }

                if (list_length(indextlist) > 0)
                {
                    TargetEntry *tle = llast_node(TargetEntry, indextlist);
                    if (IsA(tle->expr, Var) &&
                        ((Var *) tle->expr)->varno == INDEX_VAR &&
                        ((Var *) tle->expr)->varattno ==
                            AQPSampleProbAttributeNumber)
                    {
                        FuncExpr *func;
                        Var *var = (Var *) tle->expr;
                        func = makeFuncExpr(/*funcid=*/aqp_sample_prob_oid,
                                            /*rettype=*/var->vartype,
                                            /*args=*/NIL,
                                            /*funccollid=*/var->varcollid,
                                            /*inputcollid=*/InvalidOid,
                                            /*fformat=*/COERCE_EXPLICIT_CALL);
                        tle->expr = (Expr *) func;
                        /* 
                         * No worries about leaking the old Var since we are at
                         * the end of an EXPLAIN query.
                         */
                    }
                }
            }
            else
            {
                ScanState *scanstate = (ScanState *) state;
                TupleDesc descr;
                List **p_qptlist;

                if (AQPISSIsPSWRSubPlan(private))
                {
                    p_qptlist = &((AQPProgressiveSampleScanPrivate *) private)
                        ->qptlist;
                    is_pswr = true;
                }
                else
                {
                    p_qptlist = &cscan->scan.plan.targetlist;
                }

                descr = RelationGetDescr(scanstate->ss_currentRelation);
                *p_qptlist = (List *)
                    aqp_rewrite_dummy_var_in_aqp_swrscan_for_explain(
                        (Node *) *p_qptlist,
                        cscan->scan.scanrelid,
                        descr->natts);
            }
        }
    
        if (is_pswr)
        {
            /* 
             * Also need to trasnform the dummy Var ref of the sample prob
             * column to FuncExpr on sample_prob(). However, we can simply look
             * at the last tle since we didn't replace sample_prob() in any
             * other tle.
             */
            List *qptlist = cscan->scan.plan.targetlist;
            if (qptlist != NIL)
            {
                TargetEntry *tle = llast_node(TargetEntry, qptlist);
                TupleDesc descr =
                    RelationGetDescr(((ScanState *) state)->ss_currentRelation);
                if (IsA(tle->expr, Var) &&
                    ((Var *) tle->expr)->varno == cscan->scan.scanrelid &&
                    ((Var *) tle->expr)->varattno == descr->natts + 1)
                {
                    FuncExpr *func = makeFuncExpr(
                        /*funcid=*/aqp_sample_prob_oid,
                        /*rettype=*/FLOAT8OID,
                        /*args=*/NIL,
                        /*funccollid=*/InvalidOid,
                        /*inputcollid=*/InvalidOid,
                        /*fformat=*/COERCE_EXPLICIT_CALL);
                    tle->expr = (Expr *) func;
                }
            }
        }

        return;
    }

    if (IsA(state->plan, SubqueryScan))
    {
        aqp_rewrite_sample_prob_dummy_var_for_explain(
            ((SubqueryScanState *) state)->subplan);
    }

    aqp_rewrite_sample_prob_dummy_var_for_explain(state->lefttree);
    aqp_rewrite_sample_prob_dummy_var_for_explain(state->righttree);
}

static Node*
aqp_rewrite_dummy_var_in_aqp_swrscan_for_explain(Node *node, Index scanrelid,
                                                 int natts)
{
    AQPRewriteDummyVarInAQPSWRScanForExplainContext cxt;
    cxt.scanrelid = scanrelid;
    cxt.natts = natts;
    return aqp_rewrite_dummy_var_in_aqp_swrscan_for_explain_impl(node, &cxt);
}

static Node*
aqp_rewrite_dummy_var_in_aqp_swrscan_for_explain_impl(
    Node *node,
    AQPRewriteDummyVarInAQPSWRScanForExplainContext *cxt)
{
    if (node == NULL)
        return NULL;

    if (IsA(node, Var))
    {
        Var *var = (Var *) node;
        if (var->varno == cxt->scanrelid && var->varattno == cxt->natts + 1)
        {
            FuncExpr *func = makeFuncExpr(/*funcid=*/aqp_sample_prob_oid,
                                          /*rettype=*/var->vartype,
                                          /*args=*/NIL,
                                          /*funccollid=*/var->varcollid,
                                          /*inputcollid=*/InvalidOid,
                                          /*fformat=*/COERCE_EXPLICIT_CALL);
            return (Node *) func;
        }
    }

    return expression_tree_mutator(node,
            aqp_rewrite_dummy_var_in_aqp_swrscan_for_explain_impl, cxt);
}

static Oid
aqp_lookup_fn_oid(const char *funcname, int nargs, const Oid *argtypes)
{
    List *name;
    Oid res;
    name = stringToQualifiedNameList(funcname);
    res = LookupFuncName(name, nargs, argtypes, /*missing_ok=*/true);
    list_free(name);
    return res;
}

static Oid
aqp_lookup_op_oid(const char *funcname, Oid oprleft, Oid oprright)
{
    List *name;
    Oid res;
    name = stringToQualifiedNameList(funcname);
    res = OpernameGetOprid(name, oprleft, oprright);
    list_free(name);
    return res;
}

static
bool aqp_lookup_fn_oids(void)
{
    Oid argtypes[4];

    if (aqp_fn_oid_cached)
        return true;

    aqp_fn_oid_cached = false;
    
    argtypes[0] = INTERNALOID;
    aqp_swr_tsm_handler_oid = aqp_lookup_fn_oid("aqp.swr", 1, argtypes);
    if (aqp_swr_tsm_handler_oid == InvalidOid)
        return false;

    argtypes[0] = INTERNALOID;
    aqp_pswr_tsm_handler_oid = aqp_lookup_fn_oid("aqp.pswr", 1, argtypes);
    if (aqp_pswr_tsm_handler_oid == InvalidOid)
        return false;

    aqp_sample_prob_oid = aqp_lookup_fn_oid("aqp.sample_prob", 0, NULL);
    if (aqp_sample_prob_oid == InvalidOid)
        return false;

    aqp_running_sample_size_oid = aqp_lookup_fn_oid("aqp.running_sample_size", 0, NULL);
    if (aqp_running_sample_size_oid == InvalidOid)
        return false;

    aqp_running_sample_budget_oid = aqp_lookup_fn_oid("aqp.running_sample_budget", 0, NULL);
    if (aqp_running_sample_budget_oid == InvalidOid)
        return false;

    aqp_running_state_id_oid = aqp_lookup_fn_oid("aqp.running_state_id", 0, NULL);
    if (aqp_running_state_id_oid == InvalidOid)
        return false;

    argtypes[0] = FLOAT8OID;
    aqp_sum_float8_oid = aqp_lookup_fn_oid("pg_catalog.sum", 1, argtypes);
    if (aqp_sum_float8_oid == InvalidOid)
        return false;
    
    aqp_float8_mul_opno = aqp_lookup_op_oid("pg_catalog.*",
                                            FLOAT8OID,
                                            FLOAT8OID);
    if (aqp_float8_mul_opno == InvalidOid)
        return false;
    aqp_float8_mul_funcid = get_opcode(aqp_float8_mul_opno);
    if (aqp_float8_mul_funcid == InvalidOid)
        return false;

    aqp_float8_div_opno = aqp_lookup_op_oid("pg_catalog./",
                                            FLOAT8OID,
                                            FLOAT8OID);
    if (aqp_float8_div_opno == InvalidOid)
        return false;
    aqp_float8_div_funcid = get_opcode(aqp_float8_div_opno);
    if (aqp_float8_div_funcid == InvalidOid)
        return false;

    argtypes[0] = FLOAT4OID;
    argtypes[1] = FLOAT8OID;
    aqp_float48_div_opno = aqp_lookup_op_oid("pg_catalog./",
                                             FLOAT4OID,
                                             FLOAT8OID);
    if (aqp_float48_div_opno == InvalidOid)
        return false;
    aqp_float48_div_funcid = get_opcode(aqp_float48_div_opno);
    if (aqp_float48_div_funcid == InvalidOid)
        return false;

    argtypes[0] = FLOAT8OID;
    aqp_erf_inv_oid = aqp_lookup_fn_oid("aqp.erf_inv", 1, argtypes);
    if (aqp_erf_inv_oid == InvalidOid)
        return false;
    
    argtypes[0] = ANYOID;
    aqp_approx_sum_oid = aqp_lookup_fn_oid("aqp.approx_sum", 1, argtypes);
    if (aqp_approx_sum_oid == InvalidOid)
        return false;

    argtypes[0] = ANYOID;
    argtypes[1] = FLOAT8OID;
    aqp_approx_sum_half_ci_oid = aqp_lookup_fn_oid("aqp.approx_sum_half_ci",
                                                   2, argtypes);
    if (aqp_approx_sum_half_ci_oid == InvalidOid)
        return false;
    
    aqp_approx_count_oid = aqp_lookup_fn_oid("aqp.approx_count", 0, argtypes);
    if (aqp_approx_count_oid == InvalidOid)
        return false;
    
    argtypes[0] = FLOAT8OID;
    aqp_approx_count_half_ci_oid =
        aqp_lookup_fn_oid("aqp.approx_count_star_half_ci", 1, argtypes);
    if (aqp_approx_count_half_ci_oid == InvalidOid)
        return false;
    
    argtypes[0] = ANYOID;
    aqp_approx_count_any_oid = aqp_lookup_fn_oid("aqp.approx_count", 1, argtypes);
    if (aqp_approx_count_any_oid == InvalidOid)
        return false;

    argtypes[0] = ANYOID;
    argtypes[1] = FLOAT8OID;
    aqp_approx_count_any_half_ci_oid =
        aqp_lookup_fn_oid("aqp.approx_count_half_ci", 2, argtypes);
    if (aqp_approx_count_any_half_ci_oid == InvalidOid)
        return false;

    argtypes[0] = FLOAT8OID;
    aqp_float8_accum_oid = aqp_lookup_fn_oid("aqp.float8_accum",
                                             1, argtypes);
    if (aqp_float8_accum_oid == InvalidOid)
        return false;

    argtypes[0] = FLOAT8ARRAYOID;
    argtypes[1] = FLOAT8OID;
    argtypes[2] = FLOAT8OID;
    aqp_clt_half_ci_final_func_oid =
        aqp_lookup_fn_oid("aqp.clt_half_ci_finalfunc",
                          3, argtypes);
    if (aqp_clt_half_ci_final_func_oid == InvalidOid)
        return false;

    argtypes[0] = FLOAT8OID;
    argtypes[1] = FLOAT8OID;
    argtypes[2] = FLOAT8OID;
    aqp_progressive_float8_accum_oid = aqp_lookup_fn_oid("aqp.progressive_float8_accum",
                                             3, argtypes);
    if (aqp_progressive_float8_accum_oid == InvalidOid)
        return false;

    argtypes[0] = FLOAT8OID;
    aqp_approx_progressive_count_half_ci_oid = 
        aqp_lookup_fn_oid("aqp.approx_progressive_count_star_half_ci", 1, argtypes);
    if (aqp_approx_progressive_count_half_ci_oid == InvalidOid)
        return false;

    argtypes[0] = FLOAT8ARRAYOID;
    argtypes[1] = FLOAT8OID;
    argtypes[2] = FLOAT8OID;
    aqp_progressive_clt_half_ci_final_func_oid = 
        aqp_lookup_fn_oid("aqp.progressive_clt_half_ci_finalfunc",
                          3, argtypes);
    if (aqp_progressive_clt_half_ci_final_func_oid == InvalidOid)
        return false;

    argtypes[0] = FLOAT8OID;
    aqp_approx_sum_internal_oid = aqp_lookup_fn_oid(
        "aqp.approx_sum_internal", 1, argtypes);
    if (aqp_approx_sum_internal_oid == InvalidOid)
        return false;

    argtypes[0] = INT8OID;
    argtypes[1] = FLOAT8OID;
    aqp_approx_sum_internal_accum_oid = aqp_lookup_fn_oid(
        "aqp.approx_sum_internal_accum", 2, argtypes);
    if (aqp_approx_sum_internal_accum_oid == InvalidOid)
        return false;

    argtypes[0] = INT8OID;
    aqp_approx_sum_internal_final_oid = aqp_lookup_fn_oid(
        "aqp.approx_sum_internal_final", 1, argtypes);
    if (aqp_approx_sum_internal_final_oid == InvalidOid)
        return false;

    argtypes[0] = INT8OID;
    argtypes[1] = FLOAT8OID;
    aqp_approx_sum_clt_half_ci_internal_final_oid = aqp_lookup_fn_oid(
        "aqp.approx_sum_clt_half_ci_internal_final", 2, argtypes);
    if (aqp_approx_sum_clt_half_ci_internal_final_oid == InvalidOid)
        return false;

    aqp_fn_oid_cached = true;
    return true;
}

TableSampleClause *
aqp_find_swr_sampler_in_plan(PlannerInfo *root)
{
    int i;
    TableSampleClause *swr_tsc = NULL;

    for (i = 1; i < root->simple_rel_array_size; ++i)
    {
        RangeTblEntry *rte = root->simple_rte_array[i];

        if (rte && rte->rtekind == RTE_RELATION &&
            rte->tablesample != NULL)
        {
            TableSampleClause *tsc = rte->tablesample;

            if (tsc->tsmhandler == aqp_pswr_tsm_handler_oid)
                return tsc;

            if (tsc->tsmhandler == aqp_swr_tsm_handler_oid &&
                swr_tsc == NULL)
                swr_tsc = tsc;
        }
    }

    return swr_tsc;
}

static void
aqp_record_sampler_info(CustomScan *cscan, AQPPostPlannerRewriteContext *ctx)
{
    AQPIndexSampleScanPrivate *private;
    
    private = linitial(cscan -> custom_private);

    /*
        the new agg implementation currently allocates one pswr control block per query level, not per sampler.
        for multitable wj plans, make every sampler under the same pswrctl node share the first pswrctl_info_paramid
        so executor init can fetch the same control state
     */

    if (ctx->sampler_found)
    {
     //   elog(ERROR, "ambiguous sampler found");
        Assert(private -> pswrctl_info_param != NULL);
        private -> pswrctl_info_param -> paramid = ctx -> pswrctl_info_paramid;
        return;
    }

 //   private = linitial(cscan->custom_private);

    ctx->sampler_found = true;
    ctx->pswrctl_info_paramid = private->pswrctl_info_param->paramid;
}

static void
aqp_post_planner_rewrite_impl(Plan *plan, AQPPostPlannerRewriteContext *ctx)
{
    if (plan == NULL)
        return;

    check_stack_depth();

    if (!ctx)
    {
        AQPPostPlannerRewriteContext ctx;
        memset(&ctx, 0, sizeof(AQPPostPlannerRewriteContext));
        aqp_post_planner_rewrite_impl(plan, &ctx);
        return ;
    }

    aqp_post_planner_rewrite_impl(plan->lefttree, ctx);
    aqp_post_planner_rewrite_impl(plan->righttree, ctx);

    if (IsA(plan, CustomScan))
    {
        CustomScan *cscan = (CustomScan *) plan;
        if (cscan->methods == &aqp_swrscan_methods)
        {
            aqp_fix_swrscan(cscan);
            aqp_record_sampler_info(cscan, ctx);
        }
        else if (cscan->methods == &aqp_swrindexonlyscan_methods)
        {
            aqp_fix_swrindexonlyscan(cscan);
            aqp_record_sampler_info(cscan, ctx);
        }
        else if (cscan->methods == &aqp_progressive_swrscan_methods)
        {
            aqp_fix_progressive_swrscan(cscan);
            aqp_record_sampler_info(cscan, ctx);
        }
        else if (cscan->methods == &aqp_pswrctl_methods)
        {
            if (!ctx->sampler_found)
                elog(ERROR, "no sampler found");
            aqp_fix_pswrctl(cscan, ctx->pswrctl_info_paramid);
        }
    }
    else if (IsA(plan, SubqueryScan))
    {
        /* 
         * Subquery may have independent sampler, so let the callee to
         * initialize a new rewrite context.
         */
        aqp_post_planner_rewrite_impl(((SubqueryScan *) plan)->subplan, NULL);
    }
}

static bool
aqp_query_has_swr(Query *query)
{
    ListCell *lc;

    foreach(lc, query->rtable)
    {
        RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);

        if (rte->rtekind != RTE_RELATION || rte->tablesample == NULL)
            continue;

        if (rte->tablesample->tsmhandler == aqp_swr_tsm_handler_oid)
            return true;
    }

    return false;
}

static bool
aqp_query_has_pswr(Query *query)
{
    ListCell *lc;

    foreach(lc, query->rtable)
    {
        RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);

        if (rte->rtekind != RTE_RELATION || rte->tablesample == NULL)
            continue;

        if (rte->tablesample->tsmhandler == aqp_pswr_tsm_handler_oid)
            return true;
    }
    
    return false;
}

static bool
aqp_query_has_agg(Query *query)
{
    ListCell *lc;
    Aggref *agg;

    foreach(lc, query->targetList)
    {
        TargetEntry *tle = lfirst_node(TargetEntry, lc);

        agg = aqp_find_first_aggref_in_node((Node *) tle->expr);
        if (agg != NULL)
            return true;
    }

    if (query->havingQual)
    {
        agg = aqp_find_first_aggref_in_node((Node *) query->havingQual);
        if (agg != NULL)
            return true;
    }

    return false;
}

static Aggref *
aqp_find_first_aggref_in_node(Node *node)
{
    if (node == NULL)
        return NULL;

    if (IsA(node, Aggref))
        return (Aggref *) node;

    if (IsA(node, TargetEntry))
        return aqp_find_first_aggref_in_node((Node *) ((TargetEntry *) node)->expr);

    if (IsA(node, FuncExpr))
    {
        ListCell *lc;
        foreach(lc, ((FuncExpr *) node)->args)
        {
            Aggref *agg = aqp_find_first_aggref_in_node((Node *) lfirst(lc));
            if (agg)
                return agg;
        }
        return NULL;
    }

    if (IsA(node, OpExpr))
    {
        ListCell *lc;
        foreach(lc, ((OpExpr *) node)->args)
        {
            Aggref *agg = aqp_find_first_aggref_in_node((Node *) lfirst(lc));
            if (agg)
                return agg;
        }
        return NULL;
    }

    if (IsA(node, RelabelType))
        return aqp_find_first_aggref_in_node((Node *) ((RelabelType *) node)->arg);

    if (IsA(node, CoerceViaIO))
        return aqp_find_first_aggref_in_node((Node *) ((CoerceViaIO *) node)->arg);

    if (IsA(node, ArrayCoerceExpr))
        return aqp_find_first_aggref_in_node((Node *) ((ArrayCoerceExpr *) node)->arg);

    return NULL;
}

static bool
aqp_query_first_agg_supports_new_impl(Query *query)
{
    ListCell *lc;
    Aggref *agg = NULL;

    foreach(lc, query->targetList)
    {
        TargetEntry *tle = lfirst_node(TargetEntry, lc);

        agg = aqp_find_first_aggref_in_node((Node *) tle->expr);
        if (agg != NULL)
            break;
    }

    if (agg == NULL && query->havingQual != NULL)
        agg = aqp_find_first_aggref_in_node((Node *) query->havingQual);

    if (agg == NULL)
        return false;

   // if (agg->aggstar)
     //   return false;

    switch (agg->aggfnoid)
    {
        case InvalidOid:
            return false;
        default:
            break;
    }

    return agg->aggfnoid == aqp_approx_sum_oid ||
           agg->aggfnoid == aqp_approx_sum_half_ci_oid ||
           agg->aggfnoid == aqp_approx_count_oid ||
           agg->aggfnoid == aqp_approx_count_any_oid ||
           agg->aggfnoid == aqp_approx_count_half_ci_oid ||
           agg->aggfnoid == aqp_approx_count_any_half_ci_oid;
}

