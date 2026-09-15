#include "aqp.h"

#include <nodes/makefuncs.h>
#include <nodes/nodeFuncs.h>
#include <nodes/pathnodes.h>
#include <nodes/print.h>
#include <optimizer/planner.h>
#include <utils/timestamp.h>

#include "aqp_math.h"
#include "aqp_pagg.h"
#include "aqp_planner.h"
#include "aqp_pswrctl_node.h"

/*typedef struct AQPCollectAggrefsAndProjsContext
{
    List    **approx_aggrefs;
    List    **other_aggrefs;
} AQPCollectAggrefsAndProjsContext; */

static void aqp_pagg_create_upper_paths(PlannerInfo *root,
                                        UpperRelationKind stage,
                                        RelOptInfo *input_rel,
                                        RelOptInfo *grouped_rel,
                                        void *extra);
static Path *aqp_rewrite_aggpath_with_pswrctl(AggPath *aggpath,
                                              TableSampleClause *tsc);
/*static void aqp_collect_aggrefs_and_projs(PathTarget *pathtarget,
                                          List **approx_aggrefs,
                                          List **other_aggrefs,
                                          PathTarget **new_pathtarget);
static Node* aqp_collect_aggrefs_and_projs_impl(
    Node *expr,
    AQPCollectAggrefsAndProjsContext *ctx);
static AttrNumber aqp_add_distinct_aggref_to_list(
    List **aggrefs,
    Aggref *aggref);*/
static AQPPSWRControlPathPrivate* aqp_create_pswrctlpath_private(
    TableSampleClause *tsc);
static PathTarget *aqp_create_dummy_pathtarget_for_pswrctlpath(
    PathTarget *agg_pathtarget);
inline static float8 combine_Sxx(uint64 n1, float8 Sx1, float8 Sxx1,
                                 uint64 n2, float8 Sx2, float8 Sxx2);
inline static float8 remove_Sxx(uint64 n3, float8 Sx3, float8 Sxx3,
                                uint64 n1, float8 Sx1, float8 Sxx1);
static int sort_cmp(const void *va, const void *vb, void *arg);
static int sort_cmp_var(const void *va, const void *vb, void *arg);

static create_upper_paths_hook_type prev_create_upper_paths_hook = NULL;

void
aqp_setup_create_upper_paths_hook() {
    prev_create_upper_paths_hook = create_upper_paths_hook; 
    create_upper_paths_hook = aqp_pagg_create_upper_paths;
}


static void
aqp_pagg_create_upper_paths(PlannerInfo *root,
                            UpperRelationKind stage,
                            RelOptInfo *input_rel,
                            RelOptInfo *grouped_rel,
                            void *extra)
{
    if (prev_create_upper_paths_hook)
    {
        prev_create_upper_paths_hook(root, stage, input_rel, grouped_rel, extra);
    }

    if (stage == UPPERREL_GROUP_AGG && aqp_use_new_agg_impl)
    {
        TableSampleClause *tsc;
        List *new_pathlist = NIL;
        ListCell *lc;
        bool skipped_non_agg_path = false;

        tsc = aqp_find_swr_sampler_in_plan(root);
        if (!tsc)
            return;

        foreach(lc, grouped_rel->pathlist)
        {
            Path *path = (Path *) lfirst(lc);

            if (!IsA(path, AggPath))
            {
                skipped_non_agg_path = true;
                continue;
            }

            new_pathlist = lappend(new_pathlist,
                aqp_rewrite_aggpath_with_pswrctl((AggPath *) path, tsc));
        }

        if (new_pathlist == NIL)
            elog(ERROR, "no supported AggPath found for aqp approximate aggregation");

        if (skipped_non_agg_path)
            elog(WARNING, "some non-AggPath group aggregate paths are not supported "
                        "by aqp new aggregation implementation and were ignored");

        grouped_rel->pathlist = new_pathlist;

        /*
        * PSWRControl is not parallel-aware yet. Do not let planner later pick an
        * unre-written partial aggregate path.
        */
        grouped_rel->partial_pathlist = NIL;
    }
    
    /*
    if (stage == UPPERREL_FINAL)
    {
        if (!aqp_find_swr_sampler_in_plan(root))
            return;
        elog_node_display(NOTICE, "final rel input", input_rel, true);
        elog_node_display(NOTICE, "final rel", grouped_rel, true);
    } */
}

static Path*
aqp_rewrite_aggpath_with_pswrctl(AggPath *aggpath, TableSampleClause *tsc)
{
    AQPPSWRControlPath *pswrctlpath;
    /*List *approx_aggrefs;
    List *other_aggrefs; */
    PathTarget *new_pathtarget;
    AQPPSWRControlPathPrivate *pswrctl_path_private;
    
    /*
    if (aggpath->groupClause != NIL)
    {
        elog(ERROR, "GROUP BY clause is not supported currently");
    } 
    */

    if (aggpath->qual != NIL)
    {
        elog(ERROR, "HAVING clause is not supported currently");
    }

    if ((aggpath->aggstrategy != AGG_PLAIN &&
         aggpath->aggstrategy != AGG_SORTED &&
         aggpath->aggstrategy != AGG_HASHED) ||
         aggpath->aggsplit != AGGSPLIT_SIMPLE)
    {
        elog(ERROR, "unsupported aggpath: %d %d",
             (int) aggpath->aggstrategy, aggpath->aggsplit);
    }
    
    /* 
     * We can't copy the targets to pswrctlpath right now because setrefs.c
     * will eventually complain about not being able to find the Vars in
     * the output of the agg, which is certainly the case (produces internal
     * transition states).
     *
     * Instead, we need to split the target expressions into unique transition
     * Aggref targets in Agg and the upper expression trees with references to
     * those Aggrefs in pswrctl node during the post-planner rewriting logic.
     * At this point, we only create dummy targets (Const) in pswrctlpath's
     * path target so that they still match in names and types.
     */
    /*
     * aqp_collect_aggrefs_and_projs(aggpath->path.pathtarget, &approx_aggrefs,
                                  &other_aggrefs, &new_pathtarget);
    aggpath->path.pathtarget->exprs = approx_aggrefs;
    aggpath->aggsplit = AGGSPLITOP_SKIPFINAL;
    if (other_aggrefs != NIL)
    {
        elog(ERROR, "unexpected aggrefs in approximate aggregation");
    }
    */
    
    new_pathtarget = aqp_create_dummy_pathtarget_for_pswrctlpath(
        aggpath->path.pathtarget);

    pswrctl_path_private = aqp_create_pswrctlpath_private(tsc);
    
    /* 
     * NOTE must not call makeNode(AQPPSWRControlPath) because there is no type
     * tag for it, although they are type aliases. (PG makeNode, a macro, finds
     * the tag name through a preprocessor concatenation of T_ and the first
     * arg as a raw token).
     */
    pswrctlpath = makeNode(CustomPath);
    pswrctlpath->path.pathtype = T_CustomScan;
    pswrctlpath->path.parent = aggpath->path.parent;
    pswrctlpath->path.pathtarget = new_pathtarget;
    pswrctlpath->path.param_info = NULL;
    pswrctlpath->path.parallel_aware = false;
    pswrctlpath->path.parallel_safe = false;
    pswrctlpath->path.parallel_workers = 0;
    pswrctlpath->path.rows = aggpath->path.rows;
    pswrctlpath->path.startup_cost = 0;
    pswrctlpath->path.total_cost = 0;
    pswrctlpath->path.pathkeys = aggpath->path.pathkeys;

    pswrctlpath->flags = 0;
    pswrctlpath->custom_paths = list_make1(aggpath);
    pswrctlpath->custom_private = list_make1(pswrctl_path_private);
    pswrctlpath->methods = &aqp_pswrctl_path_methods;
    
    return (Path *) pswrctlpath;
}

//static void
//aqp_collect_aggrefs_and_projs(PathTarget *pathtarget,
//                              List **approx_aggrefs,
//                              List **other_aggrefs,
//                              PathTarget **new_pathtarget)
//{
//    AQPCollectAggrefsAndProjsContext ctx;
//    ListCell *lc;
//    
//    *approx_aggrefs = NIL;
//    *other_aggrefs = NIL;
//    *new_pathtarget = makeNode(PathTarget);
//    
//    (*new_pathtarget)->exprs = NIL;
//    (*new_pathtarget)->sortgrouprefs = pathtarget->sortgrouprefs;
//    memcpy(&((*new_pathtarget)->cost), &pathtarget->cost, sizeof(QualCost));
//    (*new_pathtarget)->width = pathtarget->width;
//
//    ctx.approx_aggrefs = approx_aggrefs;
//    ctx.other_aggrefs = other_aggrefs;
//
//    foreach(lc, pathtarget->exprs)
//    {
//        Node *expr = (Node *) lfirst(lc);
//        Node *new_expr;
//
//        new_expr = aqp_collect_aggrefs_and_projs_impl(expr, &ctx);
//        (*new_pathtarget)->exprs = lappend((*new_pathtarget)->exprs, new_expr);
//    }
//}
//
//static Node*
//aqp_collect_aggrefs_and_projs_impl(Node *expr,
//                                   AQPCollectAggrefsAndProjsContext *ctx)
//{
//    if (IsA(expr, Aggref))
//    {
//        Aggref *aggref = (Aggref *) expr;
//
//        if (aggref->aggfnoid == aqp_approx_sum_internal_oid)
//        {
//            (void) aqp_add_distinct_aggref_to_list(ctx->approx_aggrefs,
//                                                   aggref);
//            return (Node *) aggref;
//        }
//        else
//        {
//            elog(ERROR, "exact aggregation is currently disallowed in aqp "
//                        "mode");
//        }
//    }
//    
//    return expression_tree_mutator(expr, aqp_collect_aggrefs_and_projs_impl,
//                                   ctx);
//}
//
//static AttrNumber
//aqp_add_distinct_aggref_to_list(List **aggrefs, Aggref *aggref)
//{
//    ListCell *lc;
//    Node *n0;
//
//    foreach(lc, *aggrefs)
//    {
//        n0 = (Node *) lfirst(lc);
//        if (equal(n0, aggref)) {
//            /* cell number starts from 0 but attrnumber starts from 1*/
//            return (AttrNumber) list_cell_number(*aggrefs, lc) + 1;
//        }
//    }
//    
//    *aggrefs = lappend(*aggrefs, aggref); 
//    return (AttrNumber) list_length(*aggrefs);
//}


static AQPPSWRControlPathPrivate*
aqp_create_pswrctlpath_private(TableSampleClause *tsc)
{
    AQPPSWRControlPathPrivate *p = palloc(sizeof(AQPPSWRControlPathPrivate));
    p->extnode.type = T_ExtensibleNode;
    p->extnode.extnodename = AQPPSWRControlPathPrivateName;
    
    if (tsc->tsmhandler == aqp_swr_tsm_handler_oid)
    {
        Assert(list_length(tsc->args) == 1);
        p->sample_size_expr = (Expr *) linitial(tsc->args);
        p->initial_phase_sample_size_expr = NULL;
        p->step_sample_size_expr = NULL;
        p->err0_expr = NULL;
        p->confidence0_expr = NULL;
    }
    else if (tsc->tsmhandler == aqp_pswr_tsm_handler_oid)
    {
        Assert(list_length(tsc->args) == 4);
        p->sample_size_expr = (Expr *) linitial(tsc->args);
        p->initial_phase_sample_size_expr = (Expr *) lsecond(tsc->args);
        /* TODO defaults to initial_phase_sample_size for now */
        p->step_sample_size_expr =
            (Expr *) copyObject(p->initial_phase_sample_size_expr);
        p->err0_expr = (Expr *) lthird(tsc->args);
        p->confidence0_expr = (Expr *) lfourth(tsc->args);
    }
    else
    {
        elog(ERROR, "unsupported aqp sampler");
    }

    return p;
}

static PathTarget *
aqp_create_dummy_pathtarget_for_pswrctlpath(PathTarget *agg_pathtarget)
{
    PathTarget *new_pathtarget;
    int nexprs;
    ListCell *lc;

    nexprs = list_length(agg_pathtarget->exprs);

    new_pathtarget = makeNode(PathTarget);
    new_pathtarget->exprs = NIL;

    foreach(lc, agg_pathtarget->exprs)
    {
        Const *dummy;
        Expr *expr = lfirst(lc);
        Oid typ = exprType((Node *) expr);
        int32 typmod = exprTypmod((Node *) expr);
        Oid coll = exprCollation((Node *) expr);

        dummy = makeNullConst(typ, typmod, coll);
        new_pathtarget->exprs = lappend(new_pathtarget->exprs, dummy);
    }

    if (agg_pathtarget->sortgrouprefs)
    {
        new_pathtarget->sortgrouprefs = (Index*) palloc(sizeof(Index) * nexprs);
        memcpy(new_pathtarget->sortgrouprefs, agg_pathtarget->sortgrouprefs,
               sizeof(Index) * nexprs);
    }

    /* This will eventually be the cost we'll pay, instead of Agg. */
    memcpy(&new_pathtarget->cost, &agg_pathtarget->cost, sizeof(QualCost));
    new_pathtarget->width = agg_pathtarget->width;

    return new_pathtarget;
}

/* 
 * Following are new implementation of approx aggregation functions. 
 *
 * They fit better here than aqp_math.c as we need to refer back to the
 * pswrctl control structures.
 */

void
aqp_approx_sum_transstate_init(AQPPSWRControlState *state,
                               AQPApproxAggTransState *transstate)
{
    /* 
     * pswrctl should have allocated this with palloc0. If we ever want to
     * support re-execution, pswrctl must memset the entire tarnsstate to zero.
     */
    transstate->pswrctl_info = state->pswrctl_info;
    transstate->flags = 0;
    transstate->grouped = state->grouped;
    transstate->is_template = true;
}

static int
sort_cmp(const void *va, const void *vb, void *arg)
{
    SortSupport ssup = (SortSupport) arg;
    return ssup->comparator(*(Datum *) va, *(Datum *) vb, ssup);
}

static int
sort_cmp_var(const void *va, const void *vb, void *arg)
{
	uint32		aval = *((const int32 *) va);
	uint32		bval = *((const int32 *) vb);
    float8      *var_part = (float8 *) arg;

    if (var_part[aval] < var_part[bval])
        return 1;
    if (var_part[aval] > var_part[bval])
        return -1;

    return 0;
}

/*
 * This function combines Sxx1 and Sxx2 into a single Sxx3 such that
 * Sxx3 is the same value as if it is computed on the disjoint union
 * of the underlying float8 values that were used to compute Sxx1 and Sxx2.
 *
 * Note: We assume n1, n2 != 0 (different from float8_combine).
 */
inline static float8
combine_Sxx(uint64 n1, float8 Sx1, float8 Sxx1,
            uint64 n2, float8 Sx2, float8 Sxx2)
{
    float8 tmp;
    float8 Sxx3;
    
    tmp = Sx1 / n1 - Sx2 / n2;
    Sxx3 = Sxx1 + Sxx2 + (float8) n1 * n2 * tmp * tmp / (n1 + n2);
    return Sxx3;
}

/*
 * This function removes Sxx1 from Sxx3 assuming, Sxx1 was computed on a subset
 * of underlying float8 values that were used to compute Sxx3.
 *
 * Note: We assume n1, n2 != 0 (different from float8_combine).
 */
inline static float8
remove_Sxx(uint64 n3, float8 Sx3, float8 Sxx3,
           uint64 n1, float8 Sx1, float8 Sxx1)
{
    float8 n2;
    float8 tmp;
    float8 Sxx2;
        
    n2 = (float8) n3 - n1;
    tmp = Sx1 / n1 - (Sx3 - Sx1) / n2;
    Sxx2 = Sxx3 - Sxx1 - n1 * n2 * tmp * tmp / n3;
    return Sxx2;
}

inline static float8
remove_var(uint64 n3, float8 mu3, float8 var3,
           uint64 n1, float8 mu1, float8 var1)
{
    float8 n2;
    float8 tmp;
    float8 Sxx2;
    float8 var2;
        
    n2 = (float8) n3 - n1;
    tmp = mu1 - (mu3 * n3 - mu1 * n1) / n2;
    Sxx2 = var3 * n3 * (n3 - 1) - var1 * n1 * (n1 - 1) 
        - n1 * n2 * tmp * tmp / n3;
    var2 = Sxx2 / (n2 * (n2 - 1));
    return var2;
}

void
aqp_approx_sum_transstate_finalize(AQPApproxAggTransState *transstate)
{
    AQPPSWRCtlInfo pswrctl_info = transstate->pswrctl_info; 
    AQPPSWRControlState *pswrctl = pswrctl_info->pswrctl;
    int index;
    uint64 n; /* #accepted */
    uint64 N; /* #total */
    uint64 n_part;
    
    Assert(pswrctl_info->num_samples_fetched == pswrctl_info->sample_size);

    if (pswrctl->do_switch_to_uniform &&
        !pswrctl_info->want_partition_key)
        index = 0;
    else if (pswrctl->do_pswr == false ||
            pswrctl_info->want_partition_key ||
            aqp_batch_sampling)
        index = pswrctl->cur_partition;
    else
        index = pswrctl->cur_partition + 1;

    if (transstate->flags == AQP_APPROX_AGG_TRANSFLAG_WANT_STATISTICS)
    {
    
        TimestampTz tz1, tz2;
        long s;
        int us;
        tz1 = GetCurrentTimestamp();
        
    do {
        /* need to compute the prefix sum arrays */
        uint64 i;
        uint64 m; /* num distinct values */
        uint64 d; /* avg length of each initial partition */
        uint64 r; /* residual # of distinct values */
        uint64 c; /* number of distinct values in the current interval */
        uint64 j; /* the partition id (goes up to aqp_dp_intervals_count) */
        Datum *kv;
        AQPPrefixStats *ps;
        Datum *pse;
        SortSupport ssup;

        transstate->flags &= ~AQP_APPROX_AGG_TRANSFLAG_WANT_STATISTICS;

        n = pswrctl_info->next_kv_idx;

        if (pswrctl->grouped)
        {
            Datum *global_kv = pswrctl_info->recorded_kv_pairs;

            kv = (Datum *) palloc(sizeof(Datum) * 2 * n);

            for (i = 0; i < n; ++i)
            {
                kv[i << 1] = global_kv[i << 1];

                if (transstate->grouped_kv_values)
                {
                    if (i < transstate->grouped_kv_capacity &&
                        transstate->grouped_kv_value_set[i])
                        kv[(i << 1) + 1] = transstate->grouped_kv_values[i];
                    else
                        kv[(i << 1) + 1] = Float8GetDatum(0.0);
                }
                else
                {
                    kv[(i << 1) + 1] = global_kv[(i << 1) + 1];
                }
            }
        }
        else
        {
            kv = pswrctl_info->recorded_kv_pairs;
        }

        if (n <= 1)
        {
            transstate->n = n;
            if (n > 0)
                transstate->mu = kv[1];
                /* transstate->Sx = kv[1];
            transstate->Sxx = 0; */
            transstate->var = 0;
            pswrctl->nintvls = 0;
            break;
        }

        ssup = &pswrctl_info
            ->subplan_info[pswrctl_info->cur_plan_id].partition_att_sortsupp;

        qsort_arg(kv,
                  n,
                  sizeof(Datum) * 2,
                  sort_cmp,
                  ssup);
        
        m = 1;
        for (i = 1; i < n; ++i)
        {
            if (kv[(i << 1) - 2] != kv[i << 1]) ++m;
        }
        
        if (aqp_optimization_strategy == AQP_OPTIMIZATION_STRATEGY_OPT_STRAT)
        {   
            ps = pswrctl->prefix_stats;
            j = 1;
            memset(&ps[0], 0, sizeof(ps[0]));
            
            /*
            ps[1].Sx = DatumGetFloat8(kv[1]);
            ps[1].Sxx = 0;
            */

            ps[1].mu = DatumGetFloat8(kv[1]);
            ps[1].var = 0;

            for (i = 1; i < n; ++i)
            {
                float8 x, tmp;
                if (0 !=
                    ssup->comparator(kv[(i - 1) << 1], kv[i << 1], ssup))
                {   
                    ps[j].key = kv[i << 1];

                    /* new partition */
                    ++j;
                    memcpy(&ps[j], &ps[j-1], sizeof(ps[j]));
                    ps[j - 1].m = i;
                }
                /* now do accumulation. */

                /* TODO also do we need to do the prefix accumulation for 
                 * optimized even if we're not doing DP and we do not 
                 * need Sxx for arbitrary ranges */

                x = DatumGetFloat8(kv[(i << 1) + 1]);
                ps[j].key = kv[(n - 1) << 1];
                
                /*
                ps[j].Sx += x;
                tmp = x * (i + 1) - ps[j].Sx;
                ps[j].Sxx += tmp * tmp / ((float8) (i + 1) * (i));
                */

                tmp = x - ps[j].mu;
                ps[j].var = tmp * tmp / ((float8) (i + 1) * (i + 1)) +
                    ps[j].var * (i - 1) / (i + 1);
                ps[j].mu = (i * ps[j].mu + x)/(i + 1);
            }
        
            ps[j].m = n;

            pswrctl->nintvls = j;

            transstate->n = n;
            /*
            transstate->Sx = ps[j].Sx;
            transstate->Sxx = ps[j].Sxx;
            */
            transstate->mu = ps[j].mu;
            transstate->var = ps[j].var;
            
            transstate->mu_part[index] = transstate->mu;
            transstate->var_part[index] = transstate->var;

            transstate->n_part[index] = n;
        }
        else if (aqp_optimization_strategy == AQP_OPTIMIZATION_STRATEGY_EQUAL_STRAT)
        {
            pse = pswrctl->prefix_keys;
            j = 1;
            memset(&pse[0], 0, sizeof(pse[0]));
            for (i = 1; i < n; ++i)
            {
                if (0 !=
                    ssup->comparator(kv[(i - 1) << 1], kv[i << 1], ssup))
                {   
                    pse[j] = kv[i << 1];
                    ++j;
                }
                /* last partition. */
                pse[j] = kv[(n - 1) << 1];
            }

            pswrctl->nintvls = j;
        }
        else
        {
            uint32 tot_height = 0;

            ps = pswrctl->prefix_stats;
            if (m <= aqp_dp_intervals_count)
            {
                d = 1;
                r = 0;
            }
            else
            {
                d = m / aqp_dp_intervals_count;
                r = m % aqp_dp_intervals_count;
            }
            
            c = 1;
            j = 1;
            memset(&ps[0], 0, sizeof(ps[0]));
            
            
            ps[1].Sx = DatumGetFloat8(kv[1]);
            ps[1].Sxx = 0;
            

            ps[1].mu = DatumGetFloat8(kv[1]);
            ps[1].var = 0;

            if (aqp_pswr_tree)
                tot_height = pswrctl_info->samples_height[0];

            for (i = 1; i < n; ++i)
            {
                float8 x, tmp;

                if (0 !=
                    ssup->comparator(kv[(i - 1) << 1], kv[i << 1], ssup))
                {
                    ++c;

                    /* 
                    * The first r partitions have more more distinct values than
                    * others. Note that if we end up with 2, 2, 2, ..., 1, 1, 1,
                    * ..., this might cause some instability. Maybe we should
                    * allow the number of intervals to overflow by some constant
                    * times of the max?
                    *
                    * However, there's also a constraint that each initial
                    * partition must have at least two samples. Otherwise,
                    * we cannot derive any sample variance estimation.
                    */
                    if (i - ps[j - 1].m > 1
                        && c > (d + ((j <= r) ? 1 : 0)))
                    {

                        /* 
                        * This assignment happens here because we have a
                        * right-open interval for each initial intervals.
                        *
                        * For example, if x1 = 1 and x2 = 2, and we use each
                        * distinct value to create a new initial interval. Then
                        * the right boundary of interval 1 will be < 2, not <
                        * 1(!).
                        *
                        * XXX Technically, we could also use anything > x1 and <=
                        * x2, but this requires an arithmetic addition and
                        * division operation on the partitiion key data type,
                        * which we leave for future to explore.
                        */
                        ps[j].key = kv[i << 1];

                        /* new partition */
                        ++j;
                        Assert(j <= aqp_dp_intervals_count);
                        memcpy(&ps[j], &ps[j-1], sizeof(ps[j]));
                        ps[j - 1].m = i;
                        if (aqp_pswr_tree)
                        {
                            ps[j - 1].h = tot_height;
                        }
                        c = 1;
                    }
                }
                /* now do accumulation. */
                x = DatumGetFloat8(kv[(i << 1) + 1]);
                ps[j].key = kv[(n - 1) << 1];
                
                /*
                ps[j].Sx += x;
                tmp = x * (i + 1) - ps[j].Sx;
                ps[j].Sxx += tmp * tmp / ((float8) (i + 1) * (i));
                */

                tmp = x - ps[j].mu;
                ps[j].var = tmp * tmp / ((float8) (i + 1) * (i + 1)) +
                    ps[j].var * (i - 1) / (i + 1);
                ps[j].mu = (i * ps[j].mu + x)/(i + 1);

                if (aqp_pswr_tree)
                {
                    tot_height += pswrctl_info->samples_height[i];
                }
            }
        
            Assert(j <= aqp_dp_intervals_count);
            ps[j].m = n;
            if (aqp_pswr_tree)
            {
                ps[j].h = tot_height;
            }
            if (ps[j].m - ps[j - 1].m == 1)
            {
                /* 
                * This is a corner case, when we just created the last partition
                * which only has the very last item. In this case, we must merge
                * it back to the previous (or otherwise, we cannot derive
                * estimation of estimator variance).
                */
                memcpy(&ps[j - 1], &ps[j], sizeof(ps[j]));
                --j;
            }
            pswrctl->nintvls = j;

            transstate->n = n;

            /*
            transstate->Sx = ps[j].Sx;
            transstate->Sxx = ps[j].Sxx;
            */

            transstate->mu = ps[j].mu;
            transstate->var = ps[j].var;

            transstate->mu_part[index] = transstate->mu;
            transstate->var_part[index] = transstate->var;

            transstate->n_part[index] = n;

            if (aqp_pswr_tree)
                transstate->height_part[index] = (float8) ps[j].h / ps[j].m;
        }

    } while(0);
       
        tz2 = GetCurrentTimestamp();
        TimestampDifference(tz1, tz2, &s, &us);
        elog(INFO, "calcstats time = %f", (s * 1e3 + us * 1e-3));
    
    }

    
    N = pswrctl_info->num_samples_fetched;
    n = transstate->n;
    n_part = transstate->n_part[index];

    /*
     * There could also be index rejections (i.e., no tuple was returned) such
     * that n < N, in which case, we also need to incorporate those into the
     * calculation of Sxx.
     *
     * Intuitively, this is essentially adding N - n more zeros into the
     * underlying values accumulated, so this changes the first and second
     * moments of the values.
     */
    
    /*
    if (n != 0 && n < N)
    {
        float8 tmp;

        tmp = transstate->Sx / n;
        transstate->Sxx += (float8) n * (N - n) / N * tmp * tmp;
    }
    */

    /* partition-wise aggregation */

    /* transstate->mu_phase += transstate->Sx / N; */
    /* transstate->mu_phase += transstate->mu * (float8) n / N; */

    for (int i = 1; i <= N - n; i++)
    {
        if (transstate->flags == AQP_LEAF_PAGE_STATISTICS)
            transstate->var_part[index] = 0;
        else
            transstate->var_part[index] = transstate->mu_part[index] * 
                transstate->mu_part[index] / ((n_part + i) * (n_part + i)) + 
                (float8) (n_part + i - 2) / (n_part + i) * 
                transstate->var_part[index]; 

        transstate->mu_part[index] = (float8) (n_part + i - 1) / 
            (n_part + i) * transstate->mu_part[index];
        
        if (aqp_pswr_tree)
            transstate->height_part[index] = (float8) (n_part + i - 1) / 
                (n_part + i) * transstate->height_part[index];

        ++transstate->n_part[index];
    }

    if (!(pswrctl->do_switch_to_uniform && index == 0))
    {
        if (pswrctl->continue_descent == 1 || pswrctl->continue_descent == 2)
        {
            transstate->weights[index] = transstate->weights[transstate->split_idx];
            transstate->mu_phase += transstate->mu_part[index] 
                * transstate->weights[index];
            /* transstate->var_phase += transstate->var_part[index]
                * transstate->weights[index] * transstate->weights[index]; */
            transstate->var_phase += transstate->var_part[index]
                * (1 - transstate->weights[index]) * (1 - transstate->weights[index]);
            
            pswrctl->continue_descent = 2;
        }
        else
        {
            transstate->mu_phase += transstate->mu_part[index];

            /* if (N > 1)
                transstate->var_phase += transstate->Sxx / (N * (N - 1)); */
            
            transstate->var_phase += transstate->var_part[index];
        }
    }

    /* elog(INFO, "%d, mu_part: %f, var_part: %f", index, transstate->mu_part[index],transstate->var_part[index]); */

    /* phase-wise aggregation */
    if (pswrctl->cur_partition + 1 == pswrctl->cur_phase_n_partitions)
    {
        if (pswrctl->do_switch_to_uniform &&
            !pswrctl_info->want_partition_key &&
            index == 0)
        {
            float8 alpha =
                (float8) pswrctl->initial_and_uniform_sample_size /
                (float8) pswrctl->total_samples_fetched;

            transstate->mu_total =
                alpha * transstate->mu_part[0] +
                (1.0 - alpha) * transstate->mu_phase;

            transstate->var_total =
                alpha * alpha * transstate->var_part[0] +
                (1.0 - alpha) * (1.0 - alpha) * transstate->var_phase;
        }
        else if (!aqp_batch_sampling)
        {
            if (pswrctl->cur_phase == 0)
            {
                transstate->mu_total = transstate->mu_part[0];
                transstate->var_total = transstate->var_part[0];
                if (aqp_pswr_tree)
                    transstate->height = transstate->height_part[0];
            }
            else
            {
                /* 
                 * alpha is the weight of the new phase's estimator; 1 - alpha is
                 * the weight of the previous phases's accumulated estimators 
                 */
                
                /*
                float8 alpha = (float8) pswrctl->cur_phase_sample_size / 
                    pswrctl->total_samples_fetched;
                transstate->mu_total =
                    (1 - alpha) * transstate->mu_total +
                    alpha * transstate->mu_phase;
                transstate->var_total = 
                    (1 - alpha) * (1 - alpha) * transstate->var_total +
                    alpha * alpha * transstate->var_phase;
                */
    
                float8 alpha = (float8) pswrctl->initial_and_uniform_sample_size / 
                    pswrctl->total_samples_fetched;
                transstate->mu_total =
                    alpha * transstate->mu_part[0] +
                    (1 - alpha) * transstate->mu_phase;
                transstate->var_total = 
                    alpha * alpha * transstate->var_part[0] +
                    (1 - alpha) * (1 - alpha) * transstate->var_phase;
                /* elog(INFO, "ci: %f", aqp_erf_inv(pswrctl->confidence0) * sqrt(2 * transstate->var_total)); */
            }
        }
        else
        {
            if (!aqp_batch_sampling_dp)
                transstate->var_total_pre = transstate->var_total;
            if (pswrctl->continue_descent == 1 || pswrctl->continue_descent == 2)
            {
                /* float8 weight =  (pswrctl->cur_descent_partitions + 2)
                    / (float8) pswrctl->cur_descent_partitions; */

                transstate->mu_phase -= transstate->mu_part[transstate->split_idx]
                    * transstate->weights[index];
                
                transstate->mu_total = transstate->mu_phase;
                /* transstate->mu_total = transstate->mu_phase
                    - transstate->mu_part[transstate->split_idx]
                    * transstate->weights[index]; */

                /* transstate->var_total = transstate->var_phase
                    - transstate->var_part[transstate->split_idx] * weight
                    * transstate->weights[index] * transstate->weights[index]; */

                transstate->var_total = transstate->var_phase 
                    + transstate->var_part[index];
            }
            else
            {
                if (pswrctl->cur_phase == 0)
                {
                    transstate->mu_total = transstate->mu_phase;
                    transstate->var_total = transstate->var_phase;
                    if (aqp_batch_sampling_dp)
                        transstate->var_total_pre = transstate->var_total;
                }
                else
                {
                    float8 alpha = (float8) pswrctl->cur_phase_sample_size / 
                        pswrctl->total_samples_fetched;
                    transstate->mu_total =
                        (1 - alpha) * transstate->mu_total +
                        alpha * (transstate->mu_phase + transstate->mu_part[0]);
                    transstate->var_total = 
                        (1 - alpha) * (1 - alpha) * transstate->var_total +
                        alpha * alpha * transstate->var_phase;
                }
            }
        }
    }
}

void
aqp_pswr_cap_allocation_to_remaining_budget(AQPPSWRControlState *pswrctl)
{
    uint64 remaining_budget;
    uint64 tot_sample_size;
    uint64 sample_size;
    uint64 remaining_sample_size;
    uint64 i;
    uint64 j;
    uint64 k;
    uint64 first_partition;
    uint64 active_partitions;
    uint64 min_sample_partitions;
    bool enforce_min_sample;

    k = pswrctl->cur_phase_n_partitions;
    first_partition =
        (aqp_batch_sampling && !pswrctl->do_switch_to_uniform) ? 1 : 0;

    if (k == 0 || pswrctl->cur_phase_sample_size == 0)
        return;

    if (k <= first_partition)
        return;

    if (first_partition == 1)
    {
        uint64 total = 0;

        pswrctl->gpsa_opt_sample_size[0] = 0;
        if (pswrctl->gpsa_opt_sample_percent != NULL)
            pswrctl->gpsa_opt_sample_percent[0] = 0.0;

        for (i = first_partition; i < k; i++)
            total += pswrctl->gpsa_opt_sample_size[i];

        pswrctl->cur_phase_sample_size = total;

        if (total == 0)
            return;

        if (pswrctl->gpsa_opt_sample_percent != NULL)
        {
            for (i = first_partition; i < k; i++)
                pswrctl->gpsa_opt_sample_percent[i] =
                    (float8) pswrctl->gpsa_opt_sample_size[i] /
                    (float8) total;
        }
    }

    remaining_budget =
        pswrctl->sample_budget >
            pswrctl->total_samples_fetched + pswrctl->discarded_probe_samples ?
        pswrctl->sample_budget -
            pswrctl->total_samples_fetched - pswrctl->discarded_probe_samples : 0;

    if (remaining_budget == 0)
    {
        pswrctl->last_phase = true;
        pswrctl->cur_phase_sample_size = 0;
        return;
    }

    if (pswrctl->cur_phase_sample_size > remaining_budget)
        pswrctl->cur_phase_sample_size = remaining_budget;

    if (first_partition == 0 && k > 1)
    {
        uint64 min_phase_sample_size = (uint64) 30 * k;

        if (pswrctl->cur_phase_sample_size < min_phase_sample_size)
            pswrctl->cur_phase_sample_size =
                Min(min_phase_sample_size, remaining_budget);
    }

    if (k == 1 && first_partition == 0)
    {
        if (pswrctl->cur_phase_sample_size < 30)
            pswrctl->cur_phase_sample_size =
                Min((uint64) 30, remaining_budget);
        pswrctl->gpsa_opt_sample_percent[0] = 1.0;
        pswrctl->gpsa_opt_sample_size[0] = pswrctl->cur_phase_sample_size;
        return;
    }

    active_partitions = 0;
    for (i = first_partition; i < k; i++)
    {
        if (pswrctl->gpsa_opt_sample_size[i] > 0)
            active_partitions++;
    }

    tot_sample_size = 0;
    min_sample_partitions =
        first_partition == 0 ? k : active_partitions;

    enforce_min_sample =
        min_sample_partitions > 0 &&
        pswrctl->cur_phase_sample_size >=
            (uint64) 30 * min_sample_partitions;

    for (i = k; i > first_partition; i--)
    {
        uint64 idx = i - 1;

        remaining_sample_size =
            pswrctl->cur_phase_sample_size - tot_sample_size;

        if (remaining_sample_size == 0 ||
            pswrctl->gpsa_opt_sample_size[idx] == 0)
        {
            pswrctl->gpsa_opt_sample_size[idx] = 0;
            continue;
        }

        sample_size =
            ceil(pswrctl->cur_phase_sample_size *
                 pswrctl->gpsa_opt_sample_percent[idx]);

        if (enforce_min_sample && sample_size < 30)
            sample_size = 30;

        if (enforce_min_sample)
        {
            uint64 remaining_min_partitions;
            uint64 remaining_min_sample;
            uint64 max_sample_size;

            remaining_min_partitions = 0;
            for (j = first_partition; j < idx; j++)
            {
                if (pswrctl->gpsa_opt_sample_size[j] > 0)
                    remaining_min_partitions++;
            }
            remaining_min_sample = (uint64) 30 * remaining_min_partitions;

            if (remaining_sample_size > remaining_min_sample)
            {
                max_sample_size = remaining_sample_size - remaining_min_sample;
                if (sample_size > max_sample_size)
                    sample_size = max_sample_size;
            }
        }

        if (sample_size > remaining_sample_size)
            sample_size = remaining_sample_size;

        tot_sample_size += sample_size;
        pswrctl->gpsa_opt_sample_size[idx] = sample_size;
    }

    pswrctl->cur_phase_sample_size = tot_sample_size;
}

void
aqp_approx_sum_check_ci(AQPPSWRControlState *pswrctl)
{
    AQPApproxAggTransState *transstate = pswrctl->transstate;
    float8 var_coefficient;
    float8 res;
    
    res = aqp_erf_inv(pswrctl->confidence0) * sqrt(2 * transstate->var_total);

    if (aqp_batch_sampling)
    {
        bool target_met =
            pswrctl->err0 > 0.0 && res < pswrctl->err0;
        bool budget_used =
            pswrctl->total_samples_fetched + pswrctl->discarded_probe_samples >=
            pswrctl->sample_budget;

        if (target_met || budget_used)
        {
            pswrctl->last_phase = true;
            return;
        }
    }
    else
    {
        if (res < pswrctl->err0 || 
            pswrctl->total_samples_fetched + pswrctl->discarded_probe_samples >=
                pswrctl->sample_budget)
        {
            pswrctl->last_phase = true;
            return;
        }

        var_coefficient = sqrt(2) * aqp_erf_inv(pswrctl->confidence0)
            / pswrctl->err0;
    }

    if (!aqp_batch_sampling)
    {

        if (aqp_pswr_tree)
        {            
            /* emit output */
            {
                float8 c;
                float8 n0;
                /* float8 np; */
                float8 s1 = 0;
                float8 s2 = 0;
                float8 t1;
                float8 t2;

                if (pswrctl->do_switch_to_uniform &&
                    pswrctl->cur_phase_n_partitions == 1)
                {
                    AQPPrefixStats *ps = pswrctl->prefix_stats;
                    uint64 nintvls = pswrctl->nintvls;

                    if (transstate->var_part != NULL &&
                        pswrctl->initial_and_uniform_sample_size > 0 &&
                        transstate->var_part[0] > 0.0)
                    {
                        float8 actual_s;

                        actual_s = transstate->var_part[0] *
                                pswrctl->initial_and_uniform_sample_size;

                        s1 = sqrt(actual_s);
                        s2 = sqrt(actual_s);
                    }
                    else
                    {
                        float8 n;
                        float8 h;

                        if (ps == NULL || nintvls == 0 ||
                            ps[nintvls].m <= 1 || ps[nintvls].var <= 0.0)
                        {
                            pswrctl->last_phase = true;
                            return;
                        }

                        n = (float8) ps[nintvls].m;

                        if (ps[nintvls].h > 0)
                        {
                            h = (float8) ps[nintvls].h / n;
                            s1 = sqrt(h * ps[nintvls].var * n);
                            s2 = sqrt(ps[nintvls].var * n / h);
                        }
                        else
                        {
                            s1 = sqrt(ps[nintvls].var * n);
                            s2 = s1;
                        }
                    }
                }
                else
                {
                    for (int i = 1; i < pswrctl->cur_phase_n_partitions; i++)
                    {
                        s1 += sqrt(transstate->var_part[i] * transstate->height_part[i]);
                        if (transstate->height_part[i] == 0)
                            continue;
                        s2 += sqrt(transstate->var_part[i] / transstate->height_part[i]);

                        /* elog(INFO, "transstate->var_part[%d]:%f", i, sqrt(transstate->var_part[i])); */
                    }
                }

                c = var_coefficient * var_coefficient;
                n0 = pswrctl->total_samples_fetched;
                /* n0 = pswrctl->initial_phase_sample_size;
                np = pswrctl->total_samples_fetched - n0; */
                t1 = s1 * s2 * c / 2 - n0;
                
                t2 = t1 * t1 + (transstate->var_total * c - 1) * n0 * n0;
                if (t2 < 0)
                {
                    pswrctl->last_phase = true;
                    return;
                }
                t2 = sqrt(t2);
                if (t1 - t2 > 0)
                {
                    elog(WARNING, "t1 = %f > t2 = %f", t1, t2);
                    pswrctl->cur_phase_sample_size = ceil(t1 - t2);
                }
                else if (t1 + t2 > 0)
                {
                    pswrctl->cur_phase_sample_size = ceil(t1 + t2);
                }
                else
                {
                    pswrctl->last_phase = true;
                    return;
                }
            }
        }
        else
        {
            /* emit output */
            {
                float8 c;
                float8 n0;
                /* float8 np; */
                float8 s = 0;
                float8 t1;
                float8 t2;

                if (pswrctl->do_switch_to_uniform &&
                    pswrctl->cur_phase_n_partitions == 1)
                {
                    AQPPrefixStats *ps = pswrctl->prefix_stats;
                    uint64 nintvls = pswrctl->nintvls;

                    if (transstate->var_part != NULL &&
                        pswrctl->initial_and_uniform_sample_size > 0 &&
                        transstate->var_part[0] > 0.0)
                    {
                        s = sqrt(transstate->var_part[0] *
                                pswrctl->initial_and_uniform_sample_size);
                    }
                    else
                    {
                        if (ps == NULL || nintvls == 0 ||
                            ps[nintvls].m <= 1 || ps[nintvls].var <= 0.0)
                        {
                            pswrctl->last_phase = true;
                            return;
                        }

                        s = sqrt(ps[nintvls].var * ps[nintvls].m);
                    }
                }
                else
                {
                    for (int i = 1; i < pswrctl->cur_phase_n_partitions; i++)
                    {
                        s += sqrt(transstate->var_part[i]);
                    }
                }

                /* s = transstate->var_phase * transstate->n; */
                c = var_coefficient * var_coefficient;
                n0 = pswrctl->total_samples_fetched;
                /* n0 = pswrctl->initial_phase_sample_size;
                np = pswrctl->total_samples_fetched - n0; */
                /* s = transstate->var_phase * np; */
                t1 = s * s * c / 2 - n0;
                
                t2 = t1 * t1 + (transstate->var_total * c - 1) * n0 * n0;
                if (t2 < 0)
                {
                    pswrctl->last_phase = true;
                    return;
                }
                t2 = sqrt(t2);
                if (t1 - t2 > 0)
                {
                    elog(WARNING, "t1 = %f > t2 = %f", t1, t2);
                    /*
                    if (aqp_pswr_to_swr && 
                        ceil(t1 - t2) > pswrctl->cur_phase_sample_size)
                    {
                        pswrctl->nintvls = 1;
                        aqp_gpsa_compute_metainfo(pswrctl);
                        pswrctl->continue_descent = 1;
                        return;
                    }
                    */
                    pswrctl->cur_phase_sample_size = ceil(t1 - t2);
                }
                else if (t1 + t2 > 0)
                {   
                    /*
                    if (aqp_pswr_to_swr && 
                        ceil(t1 + t2) > pswrctl->cur_phase_sample_size)
                    {
                        pswrctl->nintvls = 1;
                        aqp_gpsa_compute_metainfo(pswrctl);
                        pswrctl->continue_descent = 1;
                        return;
                    }
                    */
                    pswrctl->cur_phase_sample_size = ceil(t1 + t2);
                }
                else
                {
                    pswrctl->last_phase = true;
                    return;
                }
            }
        }
        
        aqp_pswr_cap_allocation_to_remaining_budget(pswrctl);

        if (pswrctl->last_phase || pswrctl->cur_phase_sample_size == 0)
            return;
    }
    else
    {
        pswrctl->descent_try_sample_size = 0;
        if (aqp_batch_sampling_dp)
        {
            if (pswrctl->initial_sample_size_total > pswrctl->step_sample_size)
            {
                pswrctl->continue_descent = 3;
                elog(INFO, "total_partitions:%d", pswrctl->cur_phase_n_partitions);
                elog(INFO, "initial_sample_size:%lu", pswrctl->initial_phase_sample_size);
                elog(INFO, "initial_sample_size_descent:%lu", pswrctl->total_samples_fetched);
            }            
continue_descent_whether_dp:
            if (pswrctl->continue_descent == 0)
            {
                transstate->sort_idx = (uint32 *) palloc0(sizeof(uint32) * 
                                        (pswrctl->cur_phase_n_partitions));
                for (int i = 0; i < pswrctl->cur_phase_n_partitions; i++)
                    transstate->sort_idx[i] = i;
                qsort_arg(transstate->sort_idx,
                        pswrctl->cur_phase_n_partitions,
                        sizeof(uint32),
                        sort_cmp_var,
                        (void *) transstate->var_part);

                pswrctl->continue_descent = 1;
                return;
            }
            else if (pswrctl->continue_descent == 2)
            {
                aqp_continue_descent_dp(pswrctl);
                 
                if (pswrctl->continue_descent == 0)
                {
                    transstate->var_total_pre = transstate->var_total_next;
                    goto continue_descent_whether_dp;
                }
            }

            if (fabs(sqrt(transstate->var_total_next) - sqrt(transstate->var_total_pre)) 
                        < aqp_subtree_stop_k * sqrt(transstate->var_total_pre)
                || sqrt(transstate->var_total_next) > sqrt(transstate->var_total_pre)
                || pswrctl->continue_descent == -1 || pswrctl->continue_descent == 3)
            {
                float8 s = 0;
                int n_par = 0;

                if (pswrctl->continue_descent != 3 && pswrctl->continue_descent_dp == 0)
                {
                    elog(INFO, "total_partitions:%d", pswrctl->cur_phase_n_partitions);
                    elog(INFO, "initial_sample_size:%lu", pswrctl->initial_phase_sample_size);
                    elog(INFO, "initial_sample_size_descent:%lu", pswrctl->total_samples_fetched);
                }
                
                pswrctl->continue_descent = 3;

                if (pswrctl->continue_descent_dp == 1)
                    return;

                {
                    float8 c;
                    float8 n0;
                    float8 t1;
                    float8 t2;

                    for (int i = 1; i < pswrctl->cur_phase_n_partitions; i++)
                    {
                        if (transstate->used[i] == 1)
                            continue;
                        s += sqrt(transstate->var_part[i]);
                        n_par++;
                    }

                    if (pswrctl->err0 == 0.0)
                    {
                        if (transstate->mu_total == 0.0)
                            elog(ERROR,
                                "relative CI target is undefined because the phase0 estimate is zero");

                        pswrctl->err0 = fabs(transstate->mu_total) * pswrctl->relative_ci;
                        elog(INFO, "relative CI target: %f", pswrctl->err0);
                    }

                    var_coefficient = sqrt(2) * aqp_erf_inv(pswrctl->confidence0)
                        / pswrctl->err0;
                    
                    c = var_coefficient * var_coefficient;

                    n0 = pswrctl->total_samples_fetched - pswrctl->leaf_page_sample_size;
                    t1 = s * s * c / 2 - n0;
                    
                    t2 = t1 * t1 + 
                        (transstate->var_total * c - 1) * n0 * n0;
                    if (t2 < 0)
                    {
                        pswrctl->last_phase = true;
                        return;
                    }
                    t2 = sqrt(t2);
                    if (t1 - t2 > 0)
                    {
                        elog(WARNING, "t1 = %f > t2 = %f", t1, t2);
                        pswrctl->cur_phase_sample_size = ceil(t1 - t2);
                    }
                    else if (t1 + t2 > 0)
                    {
                        pswrctl->cur_phase_sample_size = ceil(t1 + t2);
                    }
                    else
                    {
                        pswrctl->last_phase = true;
                        return;
                    }
                }
            
                {
                    uint64 tot_sample_size;
                    uint64 sample_size;
                    uint64 i;
                    
                    tot_sample_size = 0;
            
                    for (i = pswrctl->cur_phase_n_partitions - 1; i >= 1; i--)
                    {
                        if (transstate->used[i] == 1)
                        {
                            sample_size = 0;
                            transstate->mu_part[i] = 0;
                            transstate->var_part[i] = 0;
                        }
                        else
                        {
                            sample_size =
                                ceil(pswrctl->cur_phase_sample_size * 
                                        sqrt(transstate->var_part[i]) / s);

                            if (sample_size  < 30)
                                sample_size = 30;
                        }
                        tot_sample_size += sample_size;
                        pswrctl->gpsa_opt_sample_size[i] = sample_size;
                    }

                    pswrctl->cur_phase_sample_size = tot_sample_size;

                    elog(INFO, "cur_phase_n_partitions:%d", n_par);
                    elog(INFO, "cur_phase_sample_size:%lu", pswrctl->cur_phase_sample_size);
                    
                }
            }
            else
            {
                transstate->var_total_pre = transstate->var_total_next;
                pswrctl->continue_descent = 0;
                goto continue_descent_whether_dp;
            }
        }
        else
        {
            if (fabs(sqrt(transstate->var_total) - sqrt(transstate->var_total_pre)) 
                        < aqp_subtree_stop_k * sqrt(transstate->var_total_pre)
                || sqrt(transstate->var_total_next) > sqrt(transstate->var_total_pre)
                || pswrctl->initial_sample_size_total > pswrctl->step_sample_size
                || pswrctl->continue_descent == -1 || pswrctl->continue_descent == 3)
            {
                float8 s = 0;

                /*
                pswrctl->gpsa_opt_sample_size = 
                    (uint64 *) palloc0(sizeof(uint64) * pswrctl->cur_phase_n_partitions);
                */
                if (pswrctl->continue_descent != 3)
                {
                    elog(INFO, "total_partitions:%d", pswrctl->cur_phase_n_partitions);
                    elog(INFO, "initial_sample_size:%lu", pswrctl->initial_phase_sample_size);
                    elog(INFO, "initial_sample_size_descent:%lu", pswrctl->total_samples_fetched);
                }
    
                pswrctl->continue_descent = 3;
    
                if (pswrctl->err0 == 0.0)
                {
                    if (transstate->mu_total == 0.0)
                        elog(ERROR,
                            "relative CI target is undefined because the phase0 estimate is zero");

                    pswrctl->err0 = fabs(transstate->mu_total) * pswrctl->relative_ci;
                    elog(INFO, "relative CI target: %f", pswrctl->err0);
                }

                var_coefficient = sqrt(2) * aqp_erf_inv(pswrctl->confidence0)
                    / pswrctl->err0;

                for (int i = 1; i < pswrctl->cur_phase_n_partitions; i++)
                {
                    if (transstate->used[i] == 1)
                        continue;
                    s += sqrt(transstate->var_part[i]);
                }

                /* emit output */
                {
                    float8 c;
                    float8 n0;
                    float8 t1;
                    float8 t2;

                    c = var_coefficient * var_coefficient;

                    n0 = pswrctl->total_samples_fetched - pswrctl->leaf_page_sample_size;
                    t1 = s * s * c / 2 - n0;
                    
                    t2 = t1 * t1 + 
                        (transstate->var_total * c - 1) * n0 * n0;
                    if (t2 < 0)
                    {
                        pswrctl->last_phase = true;
                        return;
                    }
                    t2 = sqrt(t2);
                    if (t1 - t2 > 0)
                    {
                        elog(WARNING, "t1 = %f > t2 = %f", t1, t2);
                        pswrctl->cur_phase_sample_size = ceil(t1 - t2);
                    }
                    else if (t1 + t2 > 0)
                    {
                        pswrctl->cur_phase_sample_size = ceil(t1 + t2);
                    }
                    else
                    {
                        pswrctl->last_phase = true;
                        return;
                    }
                }
            
                {
                    uint64 tot_sample_size;
                    uint64 sample_size;
                    uint64 i;
                    
                    tot_sample_size = 0;
            
                    for (i = pswrctl->cur_phase_n_partitions - 1; i >= 1; i--)
                    {
                        if (transstate->used[i] == 1)
                        {
                            sample_size = 0;
                            transstate->mu_part[i] = 0;
                            transstate->var_part[i] = 0;
                        }
                        else
                        {
                            sample_size =
                                ceil(pswrctl->cur_phase_sample_size * 
                                        sqrt(transstate->var_part[i]) / s);

                            if (sample_size  < 30)
                                sample_size = 30;
                        }
                        tot_sample_size += sample_size;
                        pswrctl->gpsa_opt_sample_size[i] = sample_size;
                    }

                    pswrctl->cur_phase_sample_size = tot_sample_size;

                    aqp_pswr_cap_allocation_to_remaining_budget(pswrctl);
                    if (pswrctl->last_phase || pswrctl->cur_phase_sample_size == 0)
                        return;
                        
                    elog(INFO, "cur_phase_sample_size:%lu", pswrctl->cur_phase_sample_size);
                    
                }
            }
            else
            {
                elog(INFO, "go down");
                transstate->sort_idx = (uint32 *) palloc0(sizeof(uint32) * 
                                        (pswrctl->cur_phase_n_partitions));
                for (int i = 0; i < pswrctl->cur_phase_n_partitions; i++)
                    transstate->sort_idx[i] = i;
                qsort_arg(transstate->sort_idx,
                        pswrctl->cur_phase_n_partitions,
                        sizeof(uint32),
                        sort_cmp_var,
                        (void *) transstate->var_part);

                pswrctl->continue_descent = 1;
            }
        }
    }
}

void
aqp_approx_sum_transstate_reset_partition(AQPApproxAggTransState *transstate)
{
    transstate->n = 0;
    /*
    transstate->Sx = 0;
    transstate->Sxx = 0;
    */
    transstate->mu = 0;
    transstate->var = 0;
}

void
aqp_approx_sum_transstate_reset_phase(AQPApproxAggTransState *transstate)
{
    transstate->mu_phase = 0;
    transstate->var_phase = 0;
}

PG_FUNCTION_INFO_V1(aqp_approx_sum_internal_accum);
Datum
aqp_approx_sum_internal_accum(PG_FUNCTION_ARGS)
{
    AQPApproxAggTransState *transstate = (AQPApproxAggTransState *)
        PG_GETARG_POINTER(0);
    AQPPSWRCtlInfo pswrctl_info = transstate->pswrctl_info;
    int index;
    float8 x,
           tmp, inv_prob_mul;
    int j;

    if (transstate->is_template &&
        pswrctl_info->pswrctl->grouped)
    {
        MemoryContext aggcontext;
        MemoryContext oldcontext;
        AQPApproxAggTransState *newstate;

        if (!AggCheckCallContext(fcinfo, &aggcontext))
            elog(ERROR, "aqp internal aggregate called outside aggregate context");

        oldcontext = MemoryContextSwitchTo(aggcontext);

        newstate = (AQPApproxAggTransState *)
            palloc0(sizeof(AQPApproxAggTransState));
        memcpy(newstate, transstate, sizeof(AQPApproxAggTransState));

        newstate->is_template = false;

        {
            AQPPSWRControlState *pswrctl = pswrctl_info->pswrctl;
            int nparts = 1;

            if (pswrctl->do_pswr && !aqp_batch_sampling)
            {
                nparts = pswrctl->cur_phase_n_partitions + 1;
                if (nparts < 1)
                    nparts = 1;
            }

            newstate->mu_part = (float8 *) palloc0(sizeof(float8) * nparts);
            newstate->var_part = (float8 *) palloc0(sizeof(float8) * nparts);
            newstate->n_part = (uint32 *) palloc0(sizeof(uint32) * nparts);

            if (aqp_pswr_tree)
                newstate->height_part = (float8 *) palloc0(sizeof(float8) * nparts);

            if (pswrctl->do_pswr && pswrctl_info->want_partition_key)
            {
                newstate->grouped_kv_capacity = pswrctl->initial_phase_sample_size;
                newstate->grouped_kv_values =
                    (Datum *) palloc0(sizeof(Datum) * newstate->grouped_kv_capacity);
                newstate->grouped_kv_value_set =
                    (bool *) palloc0(sizeof(bool) * newstate->grouped_kv_capacity);
            }
        }

        MemoryContextSwitchTo(oldcontext);

        transstate = newstate;
        pswrctl_info = transstate->pswrctl_info;
    }

    index = pswrctl_info->pswrctl->cur_partition;

    /* 
     * XXX Different from the SQL-compliant float8_accum(), we removed the inf
     * checks so if we could have float overflow. we won't be able to report
     * any error if that happens.
     */

    if (pswrctl_info->rejected)
    {
        PG_RETURN_POINTER(transstate);
    }

    if (pswrctl_info->pswrctl->do_switch_to_uniform &&
        !pswrctl_info->want_partition_key)
        index = 0;
    else if (pswrctl_info->pswrctl->do_pswr == false ||
            pswrctl_info->want_partition_key ||
            aqp_batch_sampling)
        index = pswrctl_info->pswrctl->cur_partition;
    else
        index = pswrctl_info->pswrctl->cur_partition + 1;

 //   x = PG_GETARG_FLOAT8(1) * pswrctl_info->inv_prob;
    inv_prob_mul = 1.0;
    if (pswrctl_info->sampler_ctl)
    {
        bool saw_sampler_prob = false;

        for (j = 0; j < pswrctl_info->nsamplers; ++j)
        {
            if (pswrctl_info->sampler_ctl[j].num_samples_fetched == 0)
                continue;

            inv_prob_mul *= pswrctl_info->sampler_ctl[j].inv_prob;
            saw_sampler_prob = true;
        }

        if (!saw_sampler_prob)
            inv_prob_mul = pswrctl_info->inv_prob;
    }
    else
    {
        inv_prob_mul = pswrctl_info->inv_prob;
    }

    x = PG_GETARG_FLOAT8(1) * inv_prob_mul;

    if (transstate->flags == AQP_APPROX_AGG_TRANSFLAG_WANT_STATISTICS 
        && (aqp_optimization_strategy == AQP_OPTIMIZATION_STRATEGY_DP 
            || aqp_optimization_strategy ==  AQP_OPTIMIZATION_STRATEGY_OPT_STRAT))
    {
        uint64 sample_idx = pswrctl_info->next_kv_idx - 1;

        pswrctl_info->recorded_kv_pairs[(sample_idx << 1) + 1] =
            Float8GetDatum(x);

        if (transstate->grouped && transstate->grouped_kv_values)
        {
            if (sample_idx >= transstate->grouped_kv_capacity)
                elog(ERROR, "grouped pswr recorded sample index out of range");

            transstate->grouped_kv_values[sample_idx] = Float8GetDatum(x);
            transstate->grouped_kv_value_set[sample_idx] = true;
        }

        PG_RETURN_POINTER(transstate);
    }

    ++transstate->n_part[index];
    ++transstate->n;
    /* transstate->Sx += x; */

    tmp = x - transstate->mu_part[index];
    if (transstate->flags == AQP_LEAF_PAGE_STATISTICS)
        transstate->var_part[index] = 0;
    else
        transstate->var_part[index] = tmp * tmp / ((float8) 
                transstate->n_part[index] * transstate->n_part[index]) + 
                (float8) (transstate->n_part[index] - 2) / 
                transstate->n_part[index] * transstate->var_part[index];

    transstate->mu_part[index] = ((transstate->n_part[index] - 1) * 
            transstate->mu_part[index] + x) / transstate->n_part[index];

    if (aqp_pswr_tree)
        transstate->height_part[index] = ((transstate->n_part[index] - 1) * 
            transstate->height_part[index] + pswrctl_info->pswrctl->tree_level) 
            / transstate->n_part[index];

    /* See backend/utils/adt/float.c: float8_accum(). */
    /*
    if (transstate->n > 1)
    {
    */
        /* 
        * TODO perhaps we should implement a few different versions of
        * accumulation functions so that when it is unnecessary to maintain
        * Sxx, we do not.
        */
    /*
        tmp = x * transstate->n - transstate->Sx;
        transstate->Sxx += tmp * tmp / (transstate->n * (transstate->n - 1));
    }
    */

    PG_RETURN_POINTER(transstate);
}

PG_FUNCTION_INFO_V1(aqp_approx_sum_internal_final);
Datum
aqp_approx_sum_internal_final(PG_FUNCTION_ARGS)
{
    AQPApproxAggTransState *transstate = (AQPApproxAggTransState *)
        PG_GETARG_POINTER(0);
    /* 
     * XXX a small bug here: if we end up with a completely empty 
     * underlying range, this should be NULL instead of zero. Fix this later.
     */
    PG_RETURN_FLOAT8(transstate->mu_total);
}

PG_FUNCTION_INFO_V1(aqp_approx_sum_clt_half_ci_internal_final);
Datum
aqp_approx_sum_clt_half_ci_internal_final(PG_FUNCTION_ARGS)
{
    AQPApproxAggTransState *transstate = (AQPApproxAggTransState *)
        PG_GETARG_POINTER(0);
    float8 confidence = PG_GETARG_FLOAT8(1);
    float8 res;
    
    /* 
     * XXX a small bug here: if we end up with a completely empty 
     * underlying range, this should be NULL instead of zero. Fix this later.
     */
    res = aqp_erf_inv(confidence) * sqrt(2 * transstate->var_total);
    PG_RETURN_FLOAT8(res);
}

void
aqp_gpsa_compute_metainfo_optimized_stratified(AQPPSWRControlState *pswrctl)
{
    AQPPrefixStats *ps = pswrctl->prefix_stats;
    uint64 nintvls = pswrctl->nintvls;
    float8 *sigma = pswrctl->gpsa_opt_cost;
    float8 var_coefficient;
    float8 ssigma;
    uint64 i;

    var_coefficient = sqrt(2) * aqp_erf_inv(pswrctl->confidence0)
        / pswrctl->err0;

    if (ps[nintvls].m == nintvls)
    {
        uint64 sample_size;
        uint64 tot_sample_size;
        /* float8 n0 = pswrctl->total_samples_fetched;
        float8 sample_budget = pswrctl->sample_budget; */

        tot_sample_size = 0;
        pswrctl->cur_phase_n_partitions = nintvls;
        /*
        sample_size = (sample_budget - n0) / 2 / nintvls;
        */

        /* make sure each partition has some minimum sample size */
        /*
        if (sample_size  < 30)
            sample_size = 30;
        */
        sample_size = 30;

        for (int k = 1; k <= nintvls; k++)
        {
            tot_sample_size += sample_size;
            pswrctl->gpsa_opt_sample_size[k - 1] = sample_size;
            pswrctl->gpsa_opt_ub[k - 1] = ps[k].key;
            pswrctl->gpsa_opt_sample_percent[k - 1] = (float8) 1 / nintvls;

            /* elog(INFO, "%d:sample_size:%lu", k-1,sample_size); */
        }

        pswrctl->cur_phase_sample_size = tot_sample_size;
        return;
    }

    i = nintvls;
    ssigma = 0;

    /* TODO do we need to do the prefix accumulation for optimized even if 
     * we're not doing DP and we do not need Sxx for arbitrary ranges */

    while (i > 1)
    {
        float8 n;

        n = (float8) ps[i].m - ps[i-1].m;
        if (n == 1)
        {
            sigma[i] = 0;
        }
        else
        {
            /*
            sigma[i] = remove_Sxx(ps[i].m, ps[i].Sx, ps[i].Sxx,
                                   ps[i-1].m, ps[i-1].Sx, ps[i-1].Sxx);
            */
            sigma[i] = remove_var(ps[i].m, ps[i].mu, ps[i].var,
                                   ps[i-1].m, ps[i-1].mu, ps[i-1].var);
            if (sigma[i] < 0)
            {
                sigma[i] = 0;
            }
            sigma[i] = sqrt(sigma[i] * n) * (n / ps[nintvls].m);
        }
        ssigma += sigma[i];

        i--;
    }
    if (ps[i].m == 1)
    {
        sigma[i] = 0;
    }
    else
    {
        sigma[i] = sqrt(ps[i].var * ps[i].m) * (
               (float8) ps[i].m / ps[nintvls].m);
    }
    ssigma += sigma[i];

    if (ssigma == 0)
    {
        uint64 sample_size;
        uint64 tot_sample_size;
        /* float8 n0 = pswrctl->total_samples_fetched;
        float8 sample_budget = pswrctl->sample_budget; */

        tot_sample_size = 0;
        pswrctl->cur_phase_n_partitions = nintvls;
        
        /*
        sample_size = (sample_budget - n0) / 2 / nintvls;
        */

        /* make sure each partition has some minimum sample size */
        /*
        if (sample_size  < 30)
            sample_size = 30;
        */
        sample_size = 30;

        for (int k = 1; k <= nintvls; k++)
        {
            tot_sample_size += sample_size;
            pswrctl->gpsa_opt_sample_size[k - 1] = sample_size;
            pswrctl->gpsa_opt_ub[k - 1] = ps[k].key;
            pswrctl->gpsa_opt_sample_percent[k - 1] = (float8) 1 / nintvls;

            /* elog(INFO, "%d:sample_size:%lu", k-1,sample_size); */
        }

        pswrctl->cur_phase_sample_size = tot_sample_size;
        return;
    }
    
    {
        float8 s;
        float8 c;
        float8 n0;
        float8 t1;
        float8 t2;

        s = ssigma * ssigma;
        c = var_coefficient * var_coefficient;
        n0 = pswrctl->total_samples_fetched;
        t1 = s * c / 2 - n0;
        
        t2 = t1 * t1 + (pswrctl->transstate[0].var_total * c - 1) * n0 * n0;
        if (t2 < 0)
        {
            pswrctl->cur_phase_n_partitions = 0;
            pswrctl->cur_phase_sample_size = 0;
            return;
        }
        t2 = sqrt(t2);
        if (t1 - t2 > 0)
        {
            elog(WARNING, "t1 = %f > t2 = %f", t1, t2);
            pswrctl->cur_phase_sample_size = ceil(t1 - t2);
        }
        else if (t1 + t2 > 0)
        {
            pswrctl->cur_phase_sample_size = ceil(t1 + t2);
        }
        else
        {
            pswrctl->cur_phase_n_partitions = 0;
            pswrctl->cur_phase_sample_size = 0;
            return;
        }
    }

    if (nintvls == 1)
    {
        pswrctl->cur_phase_n_partitions = 1;
        pswrctl->gpsa_opt_sample_percent[0] = 1.0;
        return;
    }

    {
        uint64 tot_sample_size;
        uint64 sample_size;
        float8 s2;

        pswrctl->cur_phase_n_partitions = nintvls;

        tot_sample_size = 0;
        s2 = 0;
        for (int k = nintvls; k >= 1; k--)
        {
            s2 += sigma[k];
            sample_size =
                ceil(pswrctl->cur_phase_sample_size * (sigma[k] / ssigma));

            pswrctl->gpsa_opt_sample_percent[k - 1] = sigma[k] / ssigma;

            /* make sure each partition has some minimum sample size */
            if (sample_size  < 30)
                sample_size = 30;
            tot_sample_size += sample_size;
            pswrctl->gpsa_opt_sample_size[k - 1] = sample_size;
            pswrctl->gpsa_opt_ub[k - 1] = ps[k].key;
   
            /* elog(INFO, "%d:sample_size:%lu,", k-1, sample_size); */
        }

        if (abs(s2 - ssigma) > 1e-5)
        {
            elog(ERROR, "s2 = %f != s = %f", s2, ssigma);
        }
        
        pswrctl->cur_phase_sample_size = tot_sample_size;
    }
}

void
aqp_gpsa_compute_metainfo_equal_stratified(AQPPSWRControlState *pswrctl)
{ 
    Datum *ps = pswrctl->prefix_keys;
    AQPApproxAggTransState *transstate = pswrctl->transstate;
    uint64 nintvls = pswrctl->nintvls;
    /* float8 n0 = pswrctl->total_samples_fetched; */
    /* float8 sample_budget = pswrctl->sample_budget; */
    float8 res;
    
    res = aqp_erf_inv(pswrctl->confidence0) * sqrt(2 * transstate->var_total);

    if (res < pswrctl->err0 || 
            pswrctl->total_samples_fetched + pswrctl->discarded_probe_samples >=
            pswrctl->sample_budget)
    {
        pswrctl->cur_phase_n_partitions = 0;
        pswrctl->cur_phase_sample_size = 0;
        return; 
    }


    /* emit output */
    {
        uint64 sample_size;
        uint64 tot_sample_size;

        tot_sample_size = 0;
        pswrctl->cur_phase_n_partitions = nintvls;
        /* sample_size = (sample_budget - n0) / 2 / nintvls; */

        /* make sure each partition has some minimum sample size */
        /* 
        if (sample_size  < 30)
            sample_size = 30;
        */

        sample_size = 30;

        for (int k = 1; k <= nintvls; k++)
        {
            tot_sample_size += sample_size;
            pswrctl->gpsa_opt_sample_size[k - 1] = sample_size;
            pswrctl->gpsa_opt_ub[k - 1] = ps[k];
            pswrctl->gpsa_opt_sample_percent[k - 1] = (float8) 1 / nintvls;

            /* elog(INFO, "%d:sample_size:%lu", k-1,sample_size); */
        }

        pswrctl->cur_phase_sample_size = tot_sample_size;
    }
}

void
aqp_gpsa_compute_metainfo(AQPPSWRControlState *pswrctl)
{
    AQPPrefixStats *ps = pswrctl->prefix_stats;
    uint64 nintvls = pswrctl->nintvls;
    float8 *cost = pswrctl->gpsa_opt_cost;
    int *prev_idx = pswrctl->gpsa_opt_prev_idx;
    uint64 k;
    float8 per_part_cost = aqp_dp_linear_coefficient;
    float8 min_cost;
    float8 var_coefficient;
    uint64 i;

    var_coefficient = sqrt(2) * aqp_erf_inv(pswrctl->confidence0)
        / pswrctl->err0;
    
    k = 1;
    for (i = 0; i < nintvls; ++i)
    {
        /*
        cost[i] = sqrt(ps[i + 1].Sxx / (ps[i + 1].m - 1)) * (
            (float8) ps[i + 1].m / ps[nintvls].m);
        */
        cost[i] = sqrt(ps[i + 1].var * ps[i + 1].m) * (
            (float8) ps[i + 1].m / ps[nintvls].m);
    }
    min_cost = var_coefficient * cost[nintvls - 1];
    min_cost = min_cost * min_cost + per_part_cost;

    while (k < nintvls)
    {
        uint64 i;
        uint64 j;
        float8 sigma;
        float8 cur_cost;

        
        /* try one more partition */
        ++k;

        /* 
         * Compute the best final cost if we have k partitions. 
         * 
         * j - 1: the index of the last partition boundary of the (k-1)th
         * partition
         * ps[j]: the stats accumulated up the (j-1)th boundary
         * ps[nintvls]: the stats accumulated up the the end
         */
        sigma = get_float8_infinity();
        for (j = k - 1; j < nintvls; ++j)
        {
            float8 c;
            float8 n;

            n = (float8) ps[nintvls].m - ps[j].m;
            /*
            c = remove_Sxx(ps[nintvls].m, ps[nintvls].Sx, ps[nintvls].Sxx,
                           ps[j].m, ps[j].Sx, ps[j].Sxx);
            c = sqrt(c / (n - 1)) * ((float8) n / ps[nintvls].m);
            */
            c = remove_var(ps[nintvls].m, ps[nintvls].mu, ps[nintvls].var,
                           ps[j].m, ps[j].mu, ps[j].var);
            c = sqrt(c * n) * ((float8) n / ps[nintvls].m);
            c += cost[j - 1];
            if (c < sigma)
            {
                prev_idx[(k - 2) * nintvls + nintvls - 1] = j;
                sigma = c;
            }
        }

        cur_cost = var_coefficient * sigma;
        cur_cost = cur_cost * cur_cost;
        cur_cost += per_part_cost * k;

        if (cur_cost >= min_cost)
        {
            --k;
            break;
        }

        cost[nintvls - 1] = sigma;

        /* 
         * Otherwise, compute all the costs for k partitions. 
         *
         * i - 1: the index of the last partition boundary we're computing
         * the optimal cost for.
         */
        for (i = nintvls - 1; i >= k; --i)
        {
            cost[i - 1] = get_float8_infinity();
            /*
             * j - 1: the index of the last partition boundary of the (k-1)th
             * partition
             * ps[j]: the stats accumulated up to the (j-1)th boundary
             * ps[i]: the stats accumulated up to the ith boundary
             */
            for (j = k - 1; j < i; ++j)
            {
                float8 c;
                float8 n;

                n = (float8) ps[i].m - ps[j].m;
                /*
                c = remove_Sxx(ps[i].m, ps[i].Sx, ps[i].Sxx,
                               ps[j].m, ps[j].Sx, ps[j].Sxx);
                c = sqrt(c / (n - 1)) * (n / ps[nintvls].m);
                */
                c = remove_var(ps[i].m, ps[i].mu, ps[i].var,
                            ps[j].m, ps[j].mu, ps[j].var);
                c = sqrt(c * n) * ((float8) n / ps[nintvls].m);
                c += cost[j - 1];
                if (c < cost[i - 1])
                {
                    prev_idx[(k - 2) * nintvls + i - 1] = j;
                    cost[i - 1] = c;
                }
            }
        }
    }

    /* emit output */
    {
        float8 s;
        float8 c;
        float8 n0;
        float8 t1;
        float8 t2;

        s = cost[nintvls - 1] * cost[nintvls - 1];
        c = var_coefficient * var_coefficient;
        n0 = pswrctl->total_samples_fetched;
        t1 = s * c / 2 - n0;
        
        t2 = t1 * t1 + (pswrctl->transstate[0].var_total * c - 1) * n0 * n0;
        if (t2 < 0)
        {
            pswrctl->cur_phase_n_partitions = 0;
            pswrctl->cur_phase_sample_size = 0;
            return;
        }
        t2 = sqrt(t2);
        if (t1 - t2 > 0)
        {
            elog(WARNING, "t1 = %f > t2 = %f", t1, t2);
            pswrctl->cur_phase_sample_size = ceil(t1 - t2);
        }
        else if (t1 + t2 > 0)
        {
            pswrctl->cur_phase_sample_size = ceil(t1 + t2);
        }
        else
        {
            pswrctl->cur_phase_n_partitions = 0;
            pswrctl->cur_phase_sample_size = 0;
            return;
        }
    }

    if (k == 1)
    {
        pswrctl->cur_phase_n_partitions = 1;
        pswrctl->gpsa_opt_sample_percent[k - 1] = 1.0;
        return;
    }

    {
        float8 s;
        uint64 tot_sample_size;
        float8 sigma;
        uint64 sample_size;
        float8 s2;
        uint64 i;

        pswrctl->cur_phase_n_partitions = k;

        /* 
         * i - 1: the kth partition boundary's index
         * j - 1: the (k-1)th partition boundary's index 
         */
        s = cost[nintvls - 1];
        i = nintvls;
        tot_sample_size = 0;
        s2 = 0;
        while (k > 1)
        {
            uint64 j;
            float8 n;

            j = prev_idx[(k - 2) * nintvls + i - 1];
            n = (float8) ps[i].m - ps[j].m;
            /*
            sigma = remove_Sxx(ps[i].m, ps[i].Sx, ps[i].Sxx,
                               ps[j].m, ps[j].Sx, ps[j].Sxx);
            sigma = sqrt(sigma / (n - 1)) * (n / ps[nintvls].m);
            */
            sigma = remove_var(ps[i].m, ps[i].mu, ps[i].var,
                               ps[j].m, ps[j].mu, ps[j].var);
            sigma = sqrt(sigma * n) * ((float8) n / ps[nintvls].m);
            
            s2 += sigma;
            sample_size =
                ceil(pswrctl->cur_phase_sample_size * (sigma / s));

            pswrctl->gpsa_opt_sample_percent[k - 1] = sigma / s;
 
            /* make sure each partition has some minimum sample size */
            if (sample_size  < 30)
                sample_size = 30;
            tot_sample_size += sample_size;
            pswrctl->gpsa_opt_sample_size[k - 1] = sample_size;
            pswrctl->gpsa_opt_ub[k - 1] = ps[i].key;

            i = j;
            --k;

            /* elog(INFO, "%lu:sample_size:%lu", k, sample_size); */
        }

        /*
         * The lmost partition.
         */

        /*
        sigma = sqrt(ps[i].Sxx / (ps[i].m - 1)) * (
            (float8) ps[i].m / ps[nintvls].m);
        */
        sigma = sqrt(ps[i].var * ps[i].m) * (
            (float8) ps[i].m / ps[nintvls].m);
        s2 += sigma;
        sample_size =
            ceil(pswrctl->cur_phase_sample_size * (sigma / s));
        pswrctl->gpsa_opt_sample_percent[0] = sigma / s;
        if (sample_size < 30)
            sample_size = 30;
        tot_sample_size += sample_size;
        pswrctl->gpsa_opt_sample_size[0] = sample_size;
        pswrctl->gpsa_opt_ub[0] = ps[i].key;
        
        /* elog(INFO, "%lu:sample_size:%lu", k-1, sample_size); */

        if (abs(s2 - s) > 1e-5)
        {
            elog(ERROR, "s2 = %f != s = %f", s2, s);
        }
        
        pswrctl->cur_phase_sample_size = tot_sample_size;
    }
}

void
aqp_gpsa_compute_metainfo_height(AQPPSWRControlState *pswrctl)
{
    AQPPrefixStats *ps = pswrctl->prefix_stats;
    uint64 nintvls = pswrctl->nintvls;
    /* float8 *height = pswrctl->gpsa_opt_height;
    float8 *sgm = pswrctl->gpsa_opt_sigma; */
    float8 *cost = pswrctl->gpsa_opt_cost;
    float8 *cost2 = pswrctl->gpsa_opt_cost2;
    int *prev_idx = pswrctl->gpsa_opt_prev_idx;
    uint64 k;
    float8 per_part_cost = aqp_dp_linear_coefficient * pswrctl->tree_level;
    float8 min_cost;
    float8 var_coefficient;
    uint64 i;

    var_coefficient = sqrt(2) * aqp_erf_inv(pswrctl->confidence0)
        / pswrctl->err0;
    
    k = 1;
    for (i = 0; i < nintvls; ++i)
    {
        /* float8 sgm;
        float8 height; */
        /*
        cost[i] = sqrt(ps[i + 1].Sxx / (ps[i + 1].m - 1)) * (
            (float8) ps[i + 1].m / ps[nintvls].m);
        */

        /*
        sgm = sqrt(ps[i + 1].var * ps[i + 1].m) * (
            (float8) ps[i + 1].m / ps[nintvls].m);
        
        height = (float8) ps[i + 1].h / ps[i + 1].m;
        */

        cost[i] = sqrt((float8) ps[i + 1].h / ps[i + 1].m 
            * ps[i + 1].var * ps[i + 1].m) * (
            (float8) ps[i + 1].m / ps[nintvls].m);
        cost2[i] = sqrt(ps[i + 1].var * ps[i + 1].m / 
            ((float8) ps[i + 1].h / ps[i + 1].m)) * (
            (float8) ps[i + 1].m / ps[nintvls].m);
    }
    min_cost = var_coefficient * cost[nintvls - 1];
    min_cost = min_cost * min_cost + per_part_cost;

    while (k < nintvls)
    {
        uint64 i;
        uint64 j;
        float8 sigma;
        float8 cur_cost;
        float8 sigma2;

        
        /* try one more partition */
        ++k;

        /* 
         * Compute the best final cost if we have k partitions. 
         * 
         * j - 1: the index of the last partition boundary of the (k-1)th
         * partition
         * ps[j]: the stats accumulated up the (j-1)th boundary
         * ps[nintvls]: the stats accumulated up the the end
         */
        sigma = get_float8_infinity();
        sigma2 = get_float8_infinity();
        for (j = k - 1; j < nintvls; ++j)
        {
            float8 c;
            float8 n;
            float8 c2;

            n = (float8) ps[nintvls].m - ps[j].m;
            /*
            c = remove_Sxx(ps[nintvls].m, ps[nintvls].Sx, ps[nintvls].Sxx,
                           ps[j].m, ps[j].Sx, ps[j].Sxx);
            c = sqrt(c / (n - 1)) * ((float8) n / ps[nintvls].m);
            */
            c = remove_var(ps[nintvls].m, ps[nintvls].mu, ps[nintvls].var,
                           ps[j].m, ps[j].mu, ps[j].var);
            c2 = sqrt(c * n / ((float8) (ps[nintvls].h - ps[j].h) / n)) 
                * ((float8) n / ps[nintvls].m);
            c = sqrt(c * n * (float8) (ps[nintvls].h - ps[j].h) / n) 
                * ((float8) n / ps[nintvls].m);
            c += cost[j - 1];
            c2 += cost2[j - 1];
            if (c < sigma)
            {
                prev_idx[(k - 2) * nintvls + nintvls - 1] = j;
                sigma = c;
                sigma2 = c2;
            }
        }

        cur_cost = var_coefficient * sigma;
        cur_cost = cur_cost * cur_cost;
        cur_cost += per_part_cost * k;

        if (cur_cost >= min_cost)
        {
            --k;
            break;
        }

        cost[nintvls - 1] = sigma;
        cost2[nintvls - 1] = sigma2;

        /* 
         * Otherwise, compute all the costs for k partitions. 
         *
         * i - 1: the index of the last partition boundary we're computing
         * the optimal cost for.
         */
        for (i = nintvls - 1; i >= k; --i)
        {
            cost[i - 1] = get_float8_infinity();
            cost2[i - 1] = get_float8_infinity();
            /*
             * j - 1: the index of the last partition boundary of the (k-1)th
             * partition
             * ps[j]: the stats accumulated up to the (j-1)th boundary
             * ps[i]: the stats accumulated up to the ith boundary
             */
            for (j = k - 1; j < i; ++j)
            {
                float8 c;
                float8 n;
                float8 c2;

                n = (float8) ps[i].m - ps[j].m;
                /*
                c = remove_Sxx(ps[i].m, ps[i].Sx, ps[i].Sxx,
                               ps[j].m, ps[j].Sx, ps[j].Sxx);
                c = sqrt(c / (n - 1)) * (n / ps[nintvls].m);
                */
                c = remove_var(ps[i].m, ps[i].mu, ps[i].var,
                            ps[j].m, ps[j].mu, ps[j].var);
                c2 = sqrt(c * n / ((float8) (ps[i].h - ps[j].h) / n)) 
                    * ((float8) n / ps[nintvls].m);
                c = sqrt(c * n * ((float8) (ps[i].h - ps[j].h) / n)) 
                    * ((float8) n / ps[nintvls].m);
                c += cost[j - 1];
                c2 += cost2[j - 1];
                if (c < cost[i - 1])
                {
                    prev_idx[(k - 2) * nintvls + i - 1] = j;
                    cost[i - 1] = c;
                    cost2[i - 1] = c2;
                }
            }
        }
    }

    /* emit output */
    {
        float8 s;
        float8 c;
        float8 n0;
        float8 t1;
        float8 t2;

        s = cost[nintvls - 1] * cost2[nintvls - 1];
        c = var_coefficient * var_coefficient;
        n0 = pswrctl->total_samples_fetched;
        t1 = s * c / 2 - n0;
        
        t2 = t1 * t1 + (pswrctl->transstate[0].var_total * c - 1) * n0 * n0;
        if (t2 < 0)
        {
            pswrctl->cur_phase_n_partitions = 0;
            pswrctl->cur_phase_sample_size = 0;
            return;
        }
        t2 = sqrt(t2);
        if (t1 - t2 > 0)
        {
            elog(WARNING, "t1 = %f > t2 = %f", t1, t2);
            pswrctl->cur_phase_sample_size = ceil(t1 - t2);
        }
        else if (t1 + t2 > 0)
        {
            pswrctl->cur_phase_sample_size = ceil(t1 + t2);
        }
        else
        {
            pswrctl->cur_phase_n_partitions = 0;
            pswrctl->cur_phase_sample_size = 0;
            return;
        }
    }

    if (k == 1)
    {
        pswrctl->cur_phase_n_partitions = 1;
        pswrctl->gpsa_opt_sample_percent[k - 1] = 1.0;
        return;
    }

    {
        /* float8 s; */
        float8 ss;
        uint64 tot_sample_size;
        float8 sigma2;
        uint64 sample_size;
        float8 s2;
        uint64 i;

        pswrctl->cur_phase_n_partitions = k;

        /* 
         * i - 1: the kth partition boundary's index
         * j - 1: the (k-1)th partition boundary's index 
         */
        /* s = cost[nintvls - 1]; */
        ss = cost2[nintvls - 1];
        i = nintvls;
        tot_sample_size = 0;
        s2 = 0;
        while (k > 1)
        {
            uint64 j;
            float8 n;

            j = prev_idx[(k - 2) * nintvls + i - 1];
            n = (float8) ps[i].m - ps[j].m;
            /*
            sigma = remove_Sxx(ps[i].m, ps[i].Sx, ps[i].Sxx,
                               ps[j].m, ps[j].Sx, ps[j].Sxx);
            sigma = sqrt(sigma / (n - 1)) * (n / ps[nintvls].m);
            */
            sigma2 = remove_var(ps[i].m, ps[i].mu, ps[i].var,
                               ps[j].m, ps[j].mu, ps[j].var);
            sigma2 = sqrt(sigma2 * n / ((float8) (ps[i].h - ps[j].h) / n)) 
                * ((float8) n / ps[nintvls].m);
            
            s2 += sigma2;
            sample_size =
                ceil(pswrctl->cur_phase_sample_size * (sigma2 / ss));

            pswrctl->gpsa_opt_sample_percent[k - 1] = sigma2 / ss;
 
            /* make sure each partition has some minimum sample size */
            if (sample_size  < 30)
                sample_size = 30;
            tot_sample_size += sample_size;
            pswrctl->gpsa_opt_sample_size[k - 1] = sample_size;
            pswrctl->gpsa_opt_ub[k - 1] = ps[i].key;

            i = j;
            --k;

            /* elog(INFO, "%lu:sample_size:%lu", k, sample_size); */
        }

        /*
         * The lmost partition.
         */

        /*
        sigma = sqrt(ps[i].Sxx / (ps[i].m - 1)) * (
            (float8) ps[i].m / ps[nintvls].m);
        */
        sigma2 = sqrt(ps[i].var * ps[i].m / ((float8) ps[i].h / ps[i].m)) 
            * ((float8) ps[i].m / ps[nintvls].m);
        s2 += sigma2;
        sample_size =
            ceil(pswrctl->cur_phase_sample_size * (sigma2 / ss));
        pswrctl->gpsa_opt_sample_percent[0] = sigma2 / ss;
        if (sample_size < 30)
            sample_size = 30;
        tot_sample_size += sample_size;
        pswrctl->gpsa_opt_sample_size[0] = sample_size;
        pswrctl->gpsa_opt_ub[0] = ps[i].key;
        
        /* elog(INFO, "%lu:sample_size:%lu", k-1, sample_size); */

        if (abs(s2 - ss) > 1e-5)
        {
            elog(ERROR, "s2 = %f != ss = %f", s2, ss);
        }
        
        pswrctl->cur_phase_sample_size = tot_sample_size;
    }
}

void
aqp_continue_descent_dp(AQPPSWRControlState *pswrctl)
{
    uint64 nintvls = pswrctl->cur_descent_partitions;
    int *prev_idx;
    AQPPrefixStats *ps;
    float8 *cost;
    float8 cost_p = 0;
    uint32 start_id = pswrctl->transstate->dp_start_idx;
    uint64 k;
    float8 per_part_cost = aqp_dp_linear_coefficient * pswrctl->tree_level;
    float8 min_cost;
    float8 var_coefficient;
    uint64 i;
    uint64 n_par_before = 0;

    var_coefficient = sqrt(2) * aqp_erf_inv(pswrctl->confidence0)
        / pswrctl->err0;
    
    prev_idx = (int *) palloc(sizeof(int) * (nintvls - 1) * nintvls);
    cost = (float8 *) palloc(sizeof(float8) * nintvls);
    ps = (AQPPrefixStats *) palloc0(sizeof(AQPPrefixStats) * (nintvls + 1));
    k = 1;

    for (i = 1; i <= nintvls; ++i)
    {
        ps[i].m = ps[i - 1].m + pswrctl->transstate->n_part[start_id + i - 1];
        ps[i].mu = pswrctl->transstate->mu_part[start_id + i - 1];
        ps[i].var = pswrctl->transstate->var_part[start_id + i - 1];
    }

    for (int i = 1; i < start_id; i++)
    {
        if (pswrctl->transstate->used[i] == 1)
            continue;
        n_par_before++;
        cost_p += sqrt(pswrctl->transstate->var_part[i]);
    }

    for (i = 0; i < nintvls; ++i)
    {
        /*
        cost[i] = sqrt(ps[i + 1].Sxx / (ps[i + 1].m - 1)) * (
            (float8) ps[i + 1].m / ps[nintvls].m);
        */
        cost[i] = sqrt(ps[i + 1].var * ps[i + 1].m) * (
            (float8) ps[i + 1].m / ps[nintvls].m);
    }
    min_cost = var_coefficient * (cost[nintvls - 1] + cost_p);
    min_cost = min_cost * min_cost + per_part_cost * (n_par_before + 1);

    while (k < nintvls)
    {
        uint64 i;
        uint64 j;
        float8 sigma;
        float8 cur_cost;

        
        /* try one more partition */
        ++k;

        /* 
         * Compute the best final cost if we have k partitions. 
         * 
         * j - 1: the index of the last partition boundary of the (k-1)th
         * partition
         * ps[j]: the stats accumulated up the (j-1)th boundary
         * ps[nintvls]: the stats accumulated up the the end
         */
        sigma = get_float8_infinity();
        for (j = k - 1; j < nintvls; ++j)
        {
            float8 c;
            float8 n;

            n = (float8) ps[nintvls].m - ps[j].m;
            /*
            c = remove_Sxx(ps[nintvls].m, ps[nintvls].Sx, ps[nintvls].Sxx,
                           ps[j].m, ps[j].Sx, ps[j].Sxx);
            c = sqrt(c / (n - 1)) * ((float8) n / ps[nintvls].m);
            */
            c = remove_var(ps[nintvls].m, ps[nintvls].mu, ps[nintvls].var,
                           ps[j].m, ps[j].mu, ps[j].var);
            c = sqrt(c * n) * ((float8) n / ps[nintvls].m);
            c += cost[j - 1];
            if (c < sigma)
            {
                prev_idx[(k - 2) * nintvls + nintvls - 1] = j;
                sigma = c;
            }
        }

        cur_cost = var_coefficient * (sigma + cost_p);
        cur_cost = cur_cost * cur_cost;
        cur_cost += per_part_cost * (n_par_before + k);

        if (cur_cost >= min_cost)
        {
            --k;
            break;
        }

        cost[nintvls - 1] = sigma;

        /* 
         * Otherwise, compute all the costs for k partitions. 
         *
         * i - 1: the index of the last partition boundary we're computing
         * the optimal cost for.
         */
        for (i = nintvls - 1; i >= k; --i)
        {
            cost[i - 1] = get_float8_infinity();
            /*
             * j - 1: the index of the last partition boundary of the (k-1)th
             * partition
             * ps[j]: the stats accumulated up to the (j-1)th boundary
             * ps[i]: the stats accumulated up to the ith boundary
             */
            for (j = k - 1; j < i; ++j)
            {
                float8 c;
                float8 n;

                n = (float8) ps[i].m - ps[j].m;
                /*
                c = remove_Sxx(ps[i].m, ps[i].Sx, ps[i].Sxx,
                               ps[j].m, ps[j].Sx, ps[j].Sxx);
                c = sqrt(c / (n - 1)) * (n / ps[nintvls].m);
                */
                c = remove_var(ps[i].m, ps[i].mu, ps[i].var,
                            ps[j].m, ps[j].mu, ps[j].var);
                c = sqrt(c * n) * ((float8) n / ps[nintvls].m);
                c += cost[j - 1];
                if (c < cost[i - 1])
                {
                    prev_idx[(k - 2) * nintvls + i - 1] = j;
                    cost[i - 1] = c;
                }
            }
        }
    }

    /* set k and boundaries */
    {
        pswrctl->dp_k_partitions = k;
        pswrctl->dp_prev_idx = repalloc(pswrctl->dp_prev_idx, k * sizeof(int));
        
        pswrctl->transstate->var_total_next = 0;
 
        if (k == 1)
        {
            float8 mu_part = 0;

            pswrctl->transstate->var_total_pre = pswrctl->transstate->var_0;
            elog(INFO, "before var: %f", sqrt(pswrctl->transstate->var_total_pre));

            pswrctl->transstate->used[pswrctl->transstate->split_idx] = 0;

            pswrctl->transstate->var_part[pswrctl->transstate->split_idx] = 
                - pswrctl->transstate->var_part[pswrctl->transstate->split_idx] 
                * (pswrctl->cur_descent_partitions + 1) * (pswrctl->cur_descent_partitions + 1)
                / (float8) (pswrctl->cur_descent_partitions * pswrctl->cur_descent_partitions 
                    + 2 * pswrctl->cur_descent_partitions);

            for (int l = start_id; l < start_id + nintvls; l++)
            {   
                mu_part += pswrctl->transstate->mu_part[l] * pswrctl->transstate->weights[l];
                pswrctl->transstate->var_part[pswrctl->transstate->split_idx] += 
                    pswrctl->transstate->var_part[l] * (1 - pswrctl->transstate->weights[l]) 
                    * (1 - pswrctl->transstate->weights[l]);
            
                pswrctl->transstate->used[l] = 1;
                pswrctl->transstate->mu_part[l] = 0;
                pswrctl->transstate->var_part[l] = 0;
            }
            pswrctl->transstate->mu_part[pswrctl->transstate->split_idx] =
                mu_part + pswrctl->transstate->mu_part[pswrctl->transstate->split_idx]
                * pswrctl->transstate->weights[pswrctl->transstate->split_idx];

            pswrctl->transstate->weights[pswrctl->transstate->split_idx] = 
                pswrctl->transstate->weights[pswrctl->transstate->split_idx] 
                * (nintvls + 1) / (float8) nintvls;
            
            for (int i = 0; i < pswrctl->cur_phase_n_partitions; i++)
            {
                if (pswrctl->transstate->used[i] == 1)
                    continue;
                pswrctl->transstate->var_total_next += pswrctl->transstate->var_part[i];
            }

            pswrctl->continue_descent = 0;
        }
        else if (k == nintvls)
        {
            elog(INFO, "before var: %f", sqrt(pswrctl->transstate->var_total_pre));
            pswrctl->transstate->var_total_next = pswrctl->transstate->var_total;
        }
        else
        {
            float8 sigma;
            float8 s2;
            uint64 i;
            float8 mu;
            float8 weight;

            elog(INFO, "before var: %f", sqrt(pswrctl->transstate->var_total_pre));

            i = nintvls;
            s2 = 0;
            for (int l = start_id; l < start_id + nintvls; l++)
            {
                pswrctl->transstate->mu_phase -= 
                    pswrctl->transstate->mu_part[l] * pswrctl->transstate->weights[l];

                pswrctl->transstate->used[l] = 1;
            }

            pswrctl->transstate->mu_phase += 
                pswrctl->transstate->mu_part[pswrctl->transstate->split_idx] 
                * pswrctl->transstate->weights[pswrctl->transstate->split_idx];

            weight = pswrctl->transstate->weights[pswrctl->transstate->split_idx] 
                * (nintvls + 1) / (float8) nintvls * k / (float8) (k + 1);

            while (k > 1)
            {
                uint64 j;
                float8 n;
                
                mu = 0;

                j = prev_idx[(k - 2) * nintvls + i - 1];
                pswrctl->dp_prev_idx[k - 1] = i;

                n = (float8) ps[i].m - ps[j].m;
                /*
                sigma = remove_Sxx(ps[i].m, ps[i].Sx, ps[i].Sxx,
                                ps[j].m, ps[j].Sx, ps[j].Sxx);
                sigma = sqrt(sigma / (n - 1)) * (n / ps[nintvls].m);
                */
                sigma = remove_var(ps[i].m, ps[i].mu, ps[i].var,
                                ps[j].m, ps[j].mu, ps[j].var);
                sigma = sigma * n * ((float8) n / ps[nintvls].m) 
                    * ((float8) n / ps[nintvls].m);
                
                s2 += sigma;

                for (int l = i; l > j; l--)
                    mu += ps[l].mu;

                pswrctl->transstate->weights[pswrctl->cur_phase_n_partitions + k - 1] = weight;
                pswrctl->transstate->mu_part[pswrctl->cur_phase_n_partitions + k - 1] = mu;
                pswrctl->transstate->var_part[pswrctl->cur_phase_n_partitions + k - 1] = sigma;

                pswrctl->transstate->mu_phase += mu * weight;

                i = j;
                --k;
            }

            mu = 0;

            pswrctl->dp_prev_idx[0] = i; 
            
            sigma = ps[i].var * ps[i].m * ((float8) ps[i].m / ps[nintvls].m) 
                * ((float8) ps[i].m / ps[nintvls].m);
            s2 += sigma;

            for (int l = i; l > 0; l--)
                mu += ps[l].mu;
            
            pswrctl->transstate->weights[pswrctl->cur_phase_n_partitions + k - 1] = weight;
            pswrctl->transstate->mu_part[pswrctl->cur_phase_n_partitions + k - 1] = mu;
            pswrctl->transstate->var_part[pswrctl->cur_phase_n_partitions + k - 1] = sigma;

            pswrctl->transstate->mu_phase += weight * mu;

            pswrctl->transstate->weights[pswrctl->transstate->split_idx] = weight;
            pswrctl->transstate->mu_phase -= 
                weight * pswrctl->transstate->mu_part[pswrctl->transstate->split_idx];

            for (int l = pswrctl->cur_phase_n_partitions - 1; l >= 1; l--)
            {
                if (pswrctl->transstate->used[l] == 1)
                {
                    /* pswrctl->transstate->mu_part[l] = 0; */
                    pswrctl->transstate->var_part[l] = 0;
                }

                pswrctl->transstate->var_total_next += pswrctl->transstate->var_part[l];
            }

            pswrctl->transstate->var_total_next += s2;
            pswrctl->cur_phase_n_partitions += pswrctl->dp_k_partitions;
            pswrctl->continue_descent_dp = 1;
        }

        /* if (sqrt(pswrctl->transstate->var_total_next) > sqrt(pswrctl->transstate->var_total_pre))
            elog(INFO, "11111"); */
        elog(INFO, "after var: %f", sqrt(pswrctl->transstate->var_total_next));
        elog(INFO, "before-partition: %d, after-partitions: %d", 
            pswrctl->cur_phase_n_partitions - pswrctl->dp_k_partitions, pswrctl->cur_phase_n_partitions);
        return;
    }
}
