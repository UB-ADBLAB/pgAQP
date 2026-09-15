#include "aqp.h"

#include <access/abtree.h>
#include <access/genam.h>
#include <access/sysattr.h>
#include <access/tsmapi.h>
#include <catalog/pg_am.h>
#include <catalog/pg_type.h>
#include <catalog/pg_opfamily.h>
#include <nodes/makefuncs.h>
#include <nodes/nodeFuncs.h>
#include <nodes/parsenodes.h>
#include <nodes/pathnodes.h>
#include <optimizer/cost.h>
#include <optimizer/optimizer.h>
#include <optimizer/pathnode.h>
#include <optimizer/paths.h>
#include <optimizer/paramassign.h>
#include <optimizer/placeholder.h>
#include "optimizer/planmain.h"
#include <optimizer/restrictinfo.h>
#include <optimizer/tlist.h>
#include <utils/fmgroids.h>
#include <utils/selfuncs.h>
#include <utils/spccache.h>
#include <utils/syscache.h>
#include <utils/lsyscache.h>
#include <utils/typcache.h>

#include <math.h>

#include "aqp_planner.h"
#include "pg_indxpath.h"
#include "aqp_samplepath.h"
#include "aqp_swrscan.h"

static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook = NULL;
static set_join_pathlist_hook_type prev_set_join_pathlist_hook = NULL;
static join_search_hook_type prev_join_search_hook = NULL;

static NestPath *aqp_pending_swr_npath = NULL;

/*
 * Mapping from table attnums to indexed col attnums.
 *
 * Since we don't handle joins here, we don't have to match varnos here. This
 * saves use from doing O(n^2) searches as in indexed_tlist in setrefs.c.
 */
typedef struct AQPIndexedColMapping {
    AttrNumber  maxattno;
    AttrNumber  tabattno2idxattno[FLEXIBLE_ARRAY_MEMBER];
} AQPIndexedColMapping;

typedef struct AQPFixIndexColRefCtx {
    Index                   rel_varno;
    AQPIndexedColMapping    *idxcol_mapping;
} AQPFixIndexColRefCtx;

static void aqp_set_rel_pathlist(PlannerInfo *root,
                                 RelOptInfo *rel,
                                 Index rti,
                                 RangeTblEntry *rte);
static void aqp_clear_samplescan_path(RelOptInfo *rel);
static void aqp_add_swr_path(PlannerInfo *root,
                             RelOptInfo *rel,
                             TableSampleClause *tsc);
static List* aqp_build_swr_path(PlannerInfo *root,
                   RelOptInfo *rel,
                   IndexOptInfo *index,
                   IndexClauseSet *clauses,
                   TableSampleClause *tsc);
static AQPSWRPath* aqp_create_swr_path(PlannerInfo *root,
                                       IndexOptInfo *index,
                                       List *index_clauses,
                                       bool index_only_scan,
                                       Relids outer_relids,
                                       double loop_count,
                                       TableSampleClause *tsc);
static void aqp_cost_swr_path(PlannerInfo *root, AQPSWRPath *path,
                              double loop_count);
static List *aqp_pgport_extract_nonindex_conditions(List *qual_clauses,
                                                    List *indexclauses);
static CustomScan* aqp_make_swrindexonlyscan(List *qptlist,
                                             List *qpqual,
                                             Index scanrelid,
                                             Oid indexid,
                                             List *indexqual,
                                             List *indextlist,
                                             uint64 sample_size,
                                             Expr *repeatable_expr,
                                             Expr *sample_size_expr,
                                             Param *pswrctl_info_param);
static CustomScan* aqp_make_swrscan(List *qptlist,
                                    List *qpqual,
                                    Index scanrelid,
                                    Oid indexid,
                                    List *indexqual,
                                    List *indexqualorig,
                                    uint64 sample_size,
                                    Expr *repeatable_expr,
                                    Expr *sample_size_expr,
                                    Param *pswrctl_info_param);
static void aqp_pgport_fix_indexqual_references(PlannerInfo *root,
                                                IndexOptInfo *index,
                                                List *indexclauses,
                                                List **stripped_indexquals_p,
                                                List **fixed_indexquals_p);
static Node *aqp_pgport_fix_indexqual_clause(PlannerInfo *root,
                                             IndexOptInfo *index,
                                             int indexcol,
                                             Node *clause, List *indexcolnos);
static Node *aqp_pgport_fix_indexqual_operand(Node *node, IndexOptInfo *index,
                                              int indexcol);
static List * aqp_pgport_order_qual_clauses(PlannerInfo *root, List *clauses);
static AQPPSWRPath* aqp_create_progressive_swr_path(PlannerInfo *root,
                                                    RelOptInfo *rel,
                                                    List *swrpaths,
                                                    TableSampleClause *tsc);
static Plan* aqp_plan_swr_path(PlannerInfo *root,
                               RelOptInfo *rel,
                               CustomPath *best_path,
                               List *tlist,
                               List *scan_clauses,
                               List *custom_plans);
static Plan* aqp_plan_progressive_swr_path_impl(PlannerInfo *root,
                                                RelOptInfo *rel,
                                                CustomPath *best_path,
                                                List *tlist,
                                                List *scan_clauses,
                                                List *custom_plans,
                                                Param *pswrctl_info_param);
static Plan* aqp_plan_swr_path_impl(PlannerInfo *root,
                                    RelOptInfo *rel,
                                    CustomPath *best_path,
                                    List *tlist,
                                    List *clauses,
                                    List *custom_plans,
                                    Param *pswrctl_info_param);
static AQPIndexedColMapping *aqp_build_indexed_col_mapping(List *indextlist);
static AttrNumber aqp_lookup_indexed_col_attno(AQPIndexedColMapping *mapping,
                                               AttrNumber tabattno);
static Node *aqp_fix_indexcol_refs(Node *node,
                                   Index rel_varno,
                                   AQPIndexedColMapping *idxcol_mapping);
static Node *aqp_fix_indexcol_refs_impl(Node *node, AQPFixIndexColRefCtx *ctx);
static bool aqp_rel_has_tablesample_handler(PlannerInfo *root,
                                            RelOptInfo *rel,
                                            Oid tsmhandler);
static bool aqp_has_swr_path(RelOptInfo *rel);
static Path *aqp_pick_swr_path(RelOptInfo *rel, Relids required_outer, bool require_parameterized);
static bool aqp_is_swr_rel(PlannerInfo *root, RelOptInfo *rel);
static Expr *aqp_canonicalize_join_clause(Expr *clause, RelOptInfo *innerrel);
static void aqp_create_parameterized_swr_paths(PlannerInfo *root, RelOptInfo *rel, Relids outer_relids, List *join_clauses);
static void aqp_set_join_pathlist(PlannerInfo *root, RelOptInfo *joinrel, RelOptInfo *outerrel, RelOptInfo *innerrel, JoinType jointype, JoinPathExtraData *extra);
static RelOptInfo *aqp_join_search(PlannerInfo *root, int levels_needed, List *initial_rels);
/*static List* aqp_reparameterize_custom_path_by_child(PlannerInfo *root,
                                                     List *custom_private,
                                                     RelOptInfo *child_rel); */

static CustomPathMethods aqp_swr_path_methods = {
    AQPSWRScanPrivateName,
    aqp_plan_swr_path,
    aqp_reparameterize_custom_path_by_child
};

static CustomPathMethods aqp_progressive_swr_path_methods = {
    AQPPSWRScanPrivateName,
    aqp_plan_swr_path,
    aqp_reparameterize_custom_path_by_child
};

static void
aqp_set_rel_pathlist(PlannerInfo *root,
                     RelOptInfo *rel,
                     Index rti,
                     RangeTblEntry *rte)
{
    TableSampleClause *tsc = rte->tablesample;

    if (prev_set_rel_pathlist_hook)
        prev_set_rel_pathlist_hook(root, rel, rti, rte);

    if (!aqp_fn_oid_cached)
        return ;
        
    if (tsc && (tsc->tsmhandler == aqp_swr_tsm_handler_oid ||
                tsc->tsmhandler == aqp_pswr_tsm_handler_oid))
    {
        aqp_clear_samplescan_path(rel);
        aqp_add_swr_path(root, rel, tsc);
    }
}

static void
aqp_clear_samplescan_path(RelOptInfo *rel)
{
    ListCell    *p;

    foreach(p, rel->pathlist)
    {
        Path    *path = (Path *) lfirst(p);
        if (path->pathtype == T_SampleScan)
        {
            /* remove all sample scan path generated by the PG optimizer */
            rel->pathlist = foreach_delete_current(rel->pathlist, p);
            pfree(path);
        }
    }
    
    /* XXX PG 13.1: no path should remain at this point */
    Assert(rel->pathlist == NIL);
}

static void
aqp_add_swr_path(PlannerInfo *root,
                 RelOptInfo *rel,
                 TableSampleClause *tsc)
{
    ListCell            *lc;
    IndexClauseSet      rclauseset;
    List                *all_swrpaths = NIL;

    /* 
     * SWR path is similar to index path but is restricted to use AB-tree.
     * See optimizer/path/idxpath.c: create_index_paths().
     */
    foreach(lc, rel->indexlist)
    {
        IndexOptInfo    *index = (IndexOptInfo *) lfirst(lc);
        List            *paths;
        ListCell        *lc2;

        if (index->relam != ABTREE_AM_OID)
            continue;
        
        /* partial index whose predicate does not match the query predicates */
        if (index->indpred != NIL && !index->predOK)
            continue;

        MemSet(&rclauseset, 0, sizeof(rclauseset));
        aqp_pgport_match_restriction_clauses_to_index(root, index, &rclauseset);

        paths = aqp_build_swr_path(root, rel, index, &rclauseset, tsc);
        foreach(lc2, paths)
        {
            all_swrpaths = lappend(all_swrpaths, lfirst(lc2));
        }
    }
    
    /*
     * If pswr is enabled, we wrap all in a pswr path for us to make runtime
     * plan selections. Otherwise, we will let PG to pick the one that seems to
     * be the cheapest.
     *
     * TODO remove aqp_enable_pswr flag at some point
     */
    if (aqp_enable_pswr || tsc->tsmhandler == aqp_pswr_tsm_handler_oid)
    {
        if (all_swrpaths != NIL)
            all_swrpaths = list_make1(
                aqp_create_progressive_swr_path(root, rel, all_swrpaths, tsc));
    }

    foreach(lc, all_swrpaths)
    {
        add_path(rel, (Path *) lfirst(lc));
    }
}

static List*
aqp_build_swr_path(PlannerInfo *root,
                   RelOptInfo *rel,
                   IndexOptInfo *index,
                   IndexClauseSet *clauses,
                   TableSampleClause *tsc)
{
    List       *result = NIL;
    Path       *ipath;
    List       *index_clauses;
    Relids     outer_relids;
    double     loop_count;
    bool       index_only_scan;
    int        indexcol;

    /*
     * 1. Combine the per-column IndexClause lists into an overall list.
     *
     * In the resulting list, clauses are ordered by index key, so that the
     * column numbers form a nondecreasing sequence.  (This order is depended
     * on by btree and possibly other places.)  The list can be empty, if the
     * index AM allows that.
     *
     * found_lower_saop_clause is set true if we accept a ScalarArrayOpExpr
     * index clause for a non-first index column.  This prevents us from
     * assuming that the scan result is ordered.  (Actually, the result is
     * still ordered if there are equality constraints for all earlier
     * columns, but it seems too expensive and non-modular for this code to be
     * aware of that refinement.)
     *
     * We also build a Relids set showing which outer rels are required by the
     * selected clauses.  Any lateral_relids are included in that, but not
     * otherwise accounted for.
     */
    index_clauses = NIL;
    outer_relids = bms_copy(rel->lateral_relids);
    for (indexcol = 0; indexcol < index->nkeycolumns; indexcol++)
    {
        ListCell   *lc;

        foreach(lc, clauses->indexclauses[indexcol])
        {
            IndexClause *iclause = (IndexClause *) lfirst(lc);
            RestrictInfo *rinfo = iclause->rinfo;

            /* We might need to omit ScalarArrayOpExpr clauses */
            if (IsA(rinfo->clause, ScalarArrayOpExpr))
            {
                /* XXX sample scan shouldn't have any ScalarArryOpExpr. */
                continue;
            }

            /* OK to include this clause */
            index_clauses = lappend(index_clauses, iclause);
            outer_relids = bms_add_members(outer_relids,
                                           rinfo->clause_relids);
        }
    }

    /* We do not want the index's rel itself listed in outer_relids */
    outer_relids = bms_del_member(outer_relids, rel->relid);
    /* Enforce convention that outer_relids is exactly NULL if empty */
    if (bms_is_empty(outer_relids))
        outer_relids = NULL;

    /* Compute loop_count for cost estimation purposes */
    loop_count = pg_port_get_loop_count(root, rel->relid, outer_relids);

    /* 
     * 2. Index ordering is never useful for sample scans, so we skip
     * computing the path keys. 
     */

    /*
     * 3. Check if an index-only scan is possible.  If we're not building
     * plain indexscans, this isn't relevant since bitmap scans don't support
     * index data retrieval anyway.
     */
    index_only_scan = pg_port_check_index_only(rel, index);
    
    /* 
     * Generate the sample scan swr path here.
     */
    ipath = (Path*) aqp_create_swr_path(root, index, index_clauses,
                                        index_only_scan, outer_relids,
                                        loop_count, tsc);
    result = lappend(result, ipath);

    return result;
}

static AQPSWRPath*
aqp_create_swr_path(PlannerInfo *root,
                    IndexOptInfo *index,
                    List *index_clauses,
                    bool index_only_scan,
                    Relids outer_relids,
                    double loop_count,
                    TableSampleClause *tsc)
{
    RelOptInfo *rel = index->rel;
    AQPSWRPath *path;
    Node       *num_rows_node;
    int64      num_rows;

  //  Assert(outer_relids == NULL);

    //outer_relid is null for base-rel scans, non null parameterized wanderjoin inner scans - both are valid here.

    path = (AQPSWRPath*) palloc(sizeof(AQPSWRPath));
    
    path->cpath.path.type = T_CustomPath;
    path->cpath.path.pathtype = T_CustomScan;
    path->cpath.path.parent = rel;
    path->cpath.path.pathtarget = rel->reltarget;
    path->cpath.path.param_info = get_baserel_parampathinfo(root, rel,
                                                            outer_relids);
    path->cpath.path.parallel_aware = false;
    path->cpath.path.parallel_safe = false; /* XXX no parallel support in AB-tree */
    path->cpath.path.parallel_workers = 0;
    path->cpath.path.pathkeys = NIL;
    
    /* no backward scan or mark restore support in AB-tree */
    path->cpath.flags = 0;
    path->cpath.custom_paths = NIL;
    path->cpath.custom_private = NIL; /* XXX we don't use custom_private */
    path->cpath.methods = &aqp_swr_path_methods;

    /* index path info */
    path->indexinfo = index;
    path->indexclauses = index_clauses;
    path->index_only_scan = index_only_scan;
    
    /* sampling info */
    num_rows_node = (Node *) linitial(tsc->args);
    num_rows_node = eval_const_expressions(root, num_rows_node);
    if (IsA(num_rows_node, Const) &&
        !((Const *) num_rows_node)->constisnull)
    {
        /* guaranteed by tsm->parameterTypes */
        num_rows = DatumGetInt64(((Const *) num_rows_node)->constvalue);
        path->sample_size_expr = NULL; 
    }
    else
    {
        /* 
         * If we don't know the sample size, we set it to a somewhat arbitrary
         * number of 100. We don't want it to be too small because we don't
         * want to the cost estimizer to produce a cost that is unreasonably
         * small.
         */
        num_rows = 100;
        path->sample_size_expr = (Expr*) num_rows_node;
    }

    if (num_rows <= 0)
    {
        ereport(ERROR,
                errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("non-positive row count %ld in TABLESAMPLE swr",
                       num_rows));
    }

    path->sample_size = (uint64) num_rows;
    path->repeatable_expr = tsc->repeatable;

    aqp_cost_swr_path(root, path, loop_count);

    return path;
}

/*
 * aqp_cost_swr_path
 *      Determines and returns the cost of sampling an index using an AB-tree
 *
 *    This is adapted from cost_index() in src/backend/optimizer/path/costsize.c.
 *
 *    Below is the original description for cost_index():
 *
 * 'path' describes the indexscan under consideration, and is complete
 *        except for the fields to be set by this routine
 * 'loop_count' is the number of repetitions of the indexscan to factor into
 *        estimates of caching behavior
 *
 * In addition to rows, startup_cost and total_cost, cost_index() sets the
 * path's indextotalcost and indexselectivity fields.  These values will be
 * needed if the IndexPath is used in a BitmapIndexScan.
 *
 * NOTE: path->indexquals must contain only clauses usable as index
 * restrictions.  Any additional quals evaluated as qpquals may reduce the
 * number of returned tuples, but they won't reduce the number of tuples
 * we have to fetch from the table, so they don't reduce the scan cost.
 */
static void
aqp_cost_swr_path(PlannerInfo *root, AQPSWRPath *path, double loop_count)
{
    IndexOptInfo *index = path->indexinfo;
    RelOptInfo *baserel = index->rel;
    bool        indexonly = path->index_only_scan;
    List       *qpquals;
    Cost        startup_cost = 0;
    Cost        run_cost = 0;
    Cost        cpu_run_cost = 0;
    Cost        indexStartupCost;
    Cost        indexTotalCost;
    Selectivity indexSelectivity;
    double        indexCorrelation,
                csquared;
    double        spc_seq_page_cost,
                spc_random_page_cost;
    Cost        min_IO_cost,
                max_IO_cost;
    QualCost    qpqual_cost;
    Cost        cpu_per_tuple;
    double        tuples_fetched;
    double        pages_fetched;
    double      num_pages_fetched;

    /* Should only be applied to base relations */
    Assert(IsA(baserel, RelOptInfo) &&
           IsA(index, IndexOptInfo));
    Assert(baserel->relid > 0);
    Assert(baserel->rtekind == RTE_RELATION);
    Assert(index->relam == ABTREE_AM_OID);

    /*
     * Mark the path with the correct row estimate, and identify which quals
     * will need to be enforced as qpquals.  We need not check any quals that
     * are implied by the index's predicate, so we can use indrestrictinfo not
     * baserestrictinfo as the list of relevant restriction clauses for the
     * rel.
     */

    /*
            swr always returns exactly sample_size rows regardless of whether the path is parameterized. using ppi_rows here would assign
            same row count to both the parameterized and un-paramed paths, causeing add_path() to dominate + pfree the parameterized one (use after free)
       */

    path -> cpath.path.rows = (double) path -> sample_size;
    if (path->cpath.path.param_info)
    {
     //   path->cpath.path.rows = path->cpath.path.param_info->ppi_rows;
        /* qpquals come from the rel's restriction clauses and ppi_clauses */
        qpquals = list_concat(aqp_pgport_extract_nonindex_conditions(
                                path->indexinfo->indrestrictinfo,
                                path->indexclauses),
                              aqp_pgport_extract_nonindex_conditions(
                                path->cpath.path.param_info->ppi_clauses,
                                path->indexclauses));
    }
    else
    {
     //   path->cpath.path.rows = baserel->rows;
        /* qpquals come from just the rel's restriction clauses */
        qpquals = aqp_pgport_extract_nonindex_conditions(
                    path->indexinfo->indrestrictinfo,
                    path->indexclauses);
    }

    if (!enable_indexscan)
        startup_cost += disable_cost;
    /* we don't need to check enable_indexonlyscan; indxpath.c does that */

    if (index->tree_height == -1)
    {
        Relation rel = index_open(index->indexoid, NoLock);
        index->tree_height = _abt_getrootheight(rel);
        index_close(rel, NoLock);
    }
    
    /* XXX hard-coded AB-tree cost estimator here */

    get_tablespace_page_costs(index->reltablespace,
                              &spc_random_page_cost, NULL);

    indexStartupCost = 0;

    /* this is the number of accesses */
    num_pages_fetched = path->sample_size * loop_count;
    if (index->tree_height > 3)
    {
        /* For the top three levels, it is usually small enough. 
         *
         * Even with fan-out of 200, 1 + 200 + 200 * 200 = 40201 pages =
         * 314.070 MB (assuming 8KB pages), which is well within a typical
         * value for effective_cache_size (defaults to 4GB but we usually have
         * substantially larger systems nowadays). Starting from level 4,
         * though, we count each access as a random I/O. This will definintely
         * be an overcounting the the actual cost, but since we are not
         * comparing ourselves with any other access path, so that might be ok
         * for now.
         */
        num_pages_fetched = num_pages_fetched * (index->tree_height - 3);
    }

    num_pages_fetched = index_pages_fetched(num_pages_fetched, index->pages,
                        (double) index->pages, root);
    indexTotalCost = num_pages_fetched * spc_random_page_cost / loop_count;
    indexSelectivity =
        (1.0 - pow(1.0 - 1.0 / baserel->tuples, path->sample_size));
    indexCorrelation = 0;

    /*
     * Save amcostestimate's results for possible use in bitmap scan planning.
     * We don't bother to save indexStartupCost or indexCorrelation, because a
     * bitmap scan doesn't care about either.
     */
    path->indextotalcost = indexTotalCost;
    path->indexselectivity = indexSelectivity;

    /* all costs for touching index itself included here */
    startup_cost += indexStartupCost;
    run_cost += indexTotalCost - indexStartupCost;

    /* estimate number of main-table tuples fetched */
    tuples_fetched = path->sample_size;

    /* fetch estimated page costs for tablespace containing table */
    get_tablespace_page_costs(baserel->reltablespace,
                              &spc_random_page_cost,
                              &spc_seq_page_cost);

    /*----------
     * Estimate number of main-table pages fetched, and compute I/O cost.
     *
     * When the index ordering is uncorrelated with the table ordering,
     * we use an approximation proposed by Mackert and Lohman (see
     * index_pages_fetched() for details) to compute the number of pages
     * fetched, and then charge spc_random_page_cost per page fetched.
     *
     * When the index ordering is exactly correlated with the table ordering
     * (just after a CLUSTER, for example), the number of pages fetched should
     * be exactly selectivity * table_size.  What's more, all but the first
     * will be sequential fetches, not the random fetches that occur in the
     * uncorrelated case.  So if the number of pages is more than 1, we
     * ought to charge
     *        spc_random_page_cost + (pages_fetched - 1) * spc_seq_page_cost
     * For partially-correlated indexes, we ought to charge somewhere between
     * these two estimates.  We currently interpolate linearly between the
     * estimates based on the correlation squared (XXX is that appropriate?).
     *
     * If it's an index-only scan, then we will not need to fetch any heap
     * pages for which the visibility map shows all tuples are visible.
     * Hence, reduce the estimated number of heap fetches accordingly.
     * We use the measured fraction of the entire heap that is all-visible,
     * which might not be particularly relevant to the subset of the heap
     * that this query will fetch; but it's not clear how to do better.
     *----------
     */
    if (loop_count > 1)
    {
        /*
         * For repeated indexscans, the appropriate estimate for the
         * uncorrelated case is to scale up the number of tuples fetched in
         * the Mackert and Lohman formula by the number of scans, so that we
         * estimate the number of pages fetched by all the scans; then
         * pro-rate the costs for one scan.  In this case we assume all the
         * fetches are random accesses.
         */
        pages_fetched = index_pages_fetched(tuples_fetched * loop_count,
                                            baserel->pages,
                                            (double) index->pages,
                                            root);

        if (indexonly)
            pages_fetched = ceil(pages_fetched * (1.0 - baserel->allvisfrac));

        max_IO_cost = (pages_fetched * spc_random_page_cost) / loop_count;

        /*
         * In the perfectly correlated case, the number of pages touched by
         * each scan is selectivity * table_size, and we can use the Mackert
         * and Lohman formula at the page level to estimate how much work is
         * saved by caching across scans.  We still assume all the fetches are
         * random, though, which is an overestimate that's hard to correct for
         * without double-counting the cache effects.  (But in most cases
         * where such a plan is actually interesting, only one page would get
         * fetched per scan anyway, so it shouldn't matter much.)
         */
        pages_fetched = ceil(indexSelectivity * (double) baserel->pages);

        pages_fetched = index_pages_fetched(pages_fetched * loop_count,
                                            baserel->pages,
                                            (double) index->pages,
                                            root);

        if (indexonly)
            pages_fetched = ceil(pages_fetched * (1.0 - baserel->allvisfrac));

        min_IO_cost = (pages_fetched * spc_random_page_cost) / loop_count;
    }
    else
    {
        /*
         * Normal case: apply the Mackert and Lohman formula, and then
         * interpolate between that and the correlation-derived result.
         */
        pages_fetched = index_pages_fetched(tuples_fetched,
                                            baserel->pages,
                                            (double) index->pages,
                                            root);

        if (indexonly)
            pages_fetched = ceil(pages_fetched * (1.0 - baserel->allvisfrac));

        /* max_IO_cost is for the perfectly uncorrelated case (csquared=0) */
        max_IO_cost = pages_fetched * spc_random_page_cost;

        /* min_IO_cost is for the perfectly correlated case (csquared=1) */
        pages_fetched = ceil(indexSelectivity * (double) baserel->pages);

        if (indexonly)
            pages_fetched = ceil(pages_fetched * (1.0 - baserel->allvisfrac));

        if (pages_fetched > 0)
        {
            min_IO_cost = spc_random_page_cost;
            if (pages_fetched > 1)
                min_IO_cost += (pages_fetched - 1) * spc_seq_page_cost;
        }
        else
            min_IO_cost = 0;
    }

    /*
     * Now interpolate based on estimated index order correlation to get total
     * disk I/O cost for main table accesses.
     */
    csquared = indexCorrelation * indexCorrelation;

    run_cost += max_IO_cost + csquared * (min_IO_cost - max_IO_cost);

    /*
     * Estimate CPU costs per tuple.
     *
     * What we want here is cpu_tuple_cost plus the evaluation costs of any
     * qual clauses that we have to evaluate as qpquals.
     */
    cost_qual_eval(&qpqual_cost, qpquals, root);

    startup_cost += qpqual_cost.startup;
    cpu_per_tuple = cpu_tuple_cost + qpqual_cost.per_tuple;

    cpu_run_cost += cpu_per_tuple * tuples_fetched;

    /* tlist eval costs are paid per output row, not per tuple scanned */
    startup_cost += path->cpath.path.pathtarget->cost.startup;
    cpu_run_cost += path->cpath.path.pathtarget->cost.per_tuple * path->cpath.path.rows;

    run_cost += cpu_run_cost;

    path->cpath.path.startup_cost = startup_cost;
    path->cpath.path.total_cost = startup_cost + run_cost;
}

/*
 * extract_nonindex_conditions
 *
 * Given a list of quals to be enforced in an indexscan, extract the ones that
 * will have to be applied as qpquals (ie, the index machinery won't handle
 * them).  Here we detect only whether a qual clause is directly redundant
 * with some indexclause.  If the index path is chosen for use, createplan.c
 * will try a bit harder to get rid of redundant qual conditions; specifically
 * it will see if quals can be proven to be implied by the indexquals.  But
 * it does not seem worth the cycles to try to factor that in at this stage,
 * since we're only trying to estimate qual eval costs.  Otherwise this must
 * match the logic in create_indexscan_plan().
 *
 * qual_clauses, and the result, are lists of RestrictInfos.
 * indexclauses is a list of IndexClauses.
 */
static List *
aqp_pgport_extract_nonindex_conditions(List *qual_clauses, List *indexclauses)
{
    List       *result = NIL;
    ListCell   *lc;

    foreach(lc, qual_clauses)
    {
        RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

        if (rinfo->pseudoconstant)
            continue;            /* we may drop pseudoconstants here */
        if (is_redundant_with_indexclauses(rinfo, indexclauses))
            continue;            /* dup or derived from same EquivalenceClass */
        /* ... skip the predicate proof attempt createplan.c will try ... */
        result = lappend(result, rinfo);
    }
    return result;
}

/*
 * aqp_pgport_fix_indexqual_references
 *      Adjust indexqual clauses to the form the executor's indexqual
 *      machinery needs.
 *
 * We have three tasks here:
 *    * Select the actual qual clauses out of the input IndexClause list,
 *      and remove RestrictInfo nodes from the qual clauses.
 *    * Index keys must be represented by Var nodes with varattno set to the
 *      index's attribute number, not the attribute number in the original rel.
 *
 * *stripped_indexquals_p receives a list of the actual qual clauses.
 *
 * *fixed_indexquals_p receives a list of the adjusted quals.  This is a copy
 * that shares no substructure with the original; this is needed in case there
 * are subplans in it (we need two separate copies of the subplan tree, or
 * things will go awry).
 */
static void
aqp_pgport_fix_indexqual_references(PlannerInfo *root,
                                    IndexOptInfo *index,
                                    List *indexclauses, 
                                    List **stripped_indexquals_p,
                                    List **fixed_indexquals_p)
{
    List       *stripped_indexquals;
    List       *fixed_indexquals;
    ListCell   *lc;

    stripped_indexquals = fixed_indexquals = NIL;

    foreach(lc, indexclauses)
    {
        IndexClause *iclause = lfirst_node(IndexClause, lc);
        int            indexcol = iclause->indexcol;
        ListCell   *lc2;

        foreach(lc2, iclause->indexquals)
        {
            RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);
            Node       *clause = (Node *) rinfo->clause;

            stripped_indexquals = lappend(stripped_indexquals, clause);
            clause = aqp_pgport_fix_indexqual_clause(root, index, indexcol,
                                                     clause,
                                                     iclause->indexcols);
            fixed_indexquals = lappend(fixed_indexquals, clause);
        }
    }

    *stripped_indexquals_p = stripped_indexquals;
    *fixed_indexquals_p = fixed_indexquals;
}

/*
 * aqp_pgport_fix_indexqual_clause
 *      Convert a single indexqual clause to the form needed by the executor.
 *
 * We do not replace nestloop params now. We replace the index key
 * variables or expressions by index Var nodes.
 */
static Node *
aqp_pgport_fix_indexqual_clause(PlannerInfo *root, IndexOptInfo *index,
                                int indexcol,
                                Node *clause, List *indexcolnos)
{
    /*
     * Replace any outer-relation variables with nestloop params.
     *
     * This also makes a copy of the clause, so it's safe to modify it
     * in-place below.
     */
    /*clause = aqp_pgport_replace_nestloop_params(root, clause); */

    clause = copyObject(clause);

    if (IsA(clause, OpExpr))
    {
        OpExpr       *op = (OpExpr *) clause;

        /* Replace the indexkey expression with an index Var. */
        linitial(op->args) = aqp_pgport_fix_indexqual_operand(linitial(op->args),
                                                              index,
                                                              indexcol);
    }
    else if (IsA(clause, RowCompareExpr))
    {
        RowCompareExpr *rc = (RowCompareExpr *) clause;
        ListCell   *lca,
                   *lcai;

        /* Replace the indexkey expressions with index Vars. */
        Assert(list_length(rc->largs) == list_length(indexcolnos));
        forboth(lca, rc->largs, lcai, indexcolnos)
        {
            lfirst(lca) = aqp_pgport_fix_indexqual_operand(lfirst(lca),
                                                           index,
                                                           lfirst_int(lcai));
        }
    }
    else if (IsA(clause, ScalarArrayOpExpr))
    {
        ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;

        /* Replace the indexkey expression with an index Var. */
        linitial(saop->args) = aqp_pgport_fix_indexqual_operand(
            linitial(saop->args), index, indexcol);
    }
    else if (IsA(clause, NullTest))
    {
        NullTest   *nt = (NullTest *) clause;

        /* Replace the indexkey expression with an index Var. */
        nt->arg = (Expr *) aqp_pgport_fix_indexqual_operand((Node *) nt->arg,
                                                            index,
                                                            indexcol);
    }
    else
        elog(ERROR, "unsupported indexqual type: %d",
             (int) nodeTag(clause));

    return clause;
}

/*
 * aqp_pgport_fix_indexqual_operand
 *      Convert an indexqual expression to a Var referencing the index column.
 *
 * We represent index keys by Var nodes having varno == INDEX_VAR and varattno
 * equal to the index's attribute number (index column position).
 *
 * Most of the code here is just for sanity cross-checking that the given
 * expression actually matches the index column it's claimed to.
 */
static Node *
aqp_pgport_fix_indexqual_operand(Node *node, IndexOptInfo *index, int indexcol)
{
    Var           *result;
    int            pos;
    ListCell   *indexpr_item;

    /*
     * Remove any binary-compatible relabeling of the indexkey
     */
    if (IsA(node, RelabelType))
        node = (Node *) ((RelabelType *) node)->arg;

    Assert(indexcol >= 0 && indexcol < index->ncolumns);

    if (index->indexkeys[indexcol] != 0)
    {
        /* It's a simple index column */
        if (IsA(node, Var) &&
            ((Var *) node)->varno == index->rel->relid &&
            ((Var *) node)->varattno == index->indexkeys[indexcol])
        {
            result = (Var *) copyObject(node);
            result->varno = INDEX_VAR;
            result->varattno = indexcol + 1;
            return (Node *) result;
        }
        else
            elog(ERROR, "index key does not match expected index column");
    }

    /* It's an index expression, so find and cross-check the expression */
    indexpr_item = list_head(index->indexprs);
    for (pos = 0; pos < index->ncolumns; pos++)
    {
        if (index->indexkeys[pos] == 0)
        {
            if (indexpr_item == NULL)
                elog(ERROR, "too few entries in indexprs list");
            if (pos == indexcol)
            {
                Node       *indexkey;

                indexkey = (Node *) lfirst(indexpr_item);
                if (indexkey && IsA(indexkey, RelabelType))
                    indexkey = (Node *) ((RelabelType *) indexkey)->arg;
                if (equal(node, indexkey))
                {
                    result = makeVar(INDEX_VAR, indexcol + 1,
                                     exprType(lfirst(indexpr_item)), -1,
                                     exprCollation(lfirst(indexpr_item)),
                                     0);
                    return (Node *) result;
                }
                else
                    elog(ERROR, "index key does not match expected index column");
            }
            indexpr_item = lnext(index->indexprs, indexpr_item);
        }
    }

    /* Oops... */
    elog(ERROR, "index key does not match expected index column");
    return NULL;                /* keep compiler quiet */
}

/*
 * aqp_pgport_order_qual_clauses
 *        Given a list of qual clauses that will all be evaluated at the same
 *        plan node, sort the list into the order we want to check the quals
 *        in at runtime.
 *
 * When security barrier quals are used in the query, we may have quals with
 * different security levels in the list.  Quals of lower security_level
 * must go before quals of higher security_level, except that we can grant
 * exceptions to move up quals that are leakproof.  When security level
 * doesn't force the decision, we prefer to order clauses by estimated
 * execution cost, cheapest first.
 *
 * Ideally the order should be driven by a combination of execution cost and
 * selectivity, but it's not immediately clear how to account for both,
 * and given the uncertainty of the estimates the reliability of the decisions
 * would be doubtful anyway.  So we just order by security level then
 * estimated per-tuple cost, being careful not to change the order when
 * (as is often the case) the estimates are identical.
 *
 * Although this will work on either bare clauses or RestrictInfos, it's
 * much faster to apply it to RestrictInfos, since it can re-use cost
 * information that is cached in RestrictInfos.  XXX in the bare-clause
 * case, we are also not able to apply security considerations.  That is
 * all right for the moment, because the bare-clause case doesn't occur
 * anywhere that barrier quals could be present, but it would be better to
 * get rid of it.
 *
 * Note: some callers pass lists that contain entries that will later be
 * removed; this is the easiest way to let this routine see RestrictInfos
 * instead of bare clauses.  This is another reason why trying to consider
 * selectivity in the ordering would likely do the wrong thing.
 */
static List *
aqp_pgport_order_qual_clauses(PlannerInfo *root, List *clauses)
{
    typedef struct
    {
        Node       *clause;
        Cost        cost;
        Index        security_level;
    } QualItem;
    int            nitems = list_length(clauses);
    QualItem   *items;
    ListCell   *lc;
    int            i;
    List       *result;

    /* No need to work hard for 0 or 1 clause */
    if (nitems <= 1)
        return clauses;

    /*
     * Collect the items and costs into an array.  This is to avoid repeated
     * cost_qual_eval work if the inputs aren't RestrictInfos.
     */
    items = (QualItem *) palloc(nitems * sizeof(QualItem));
    i = 0;
    foreach(lc, clauses)
    {
        Node       *clause = (Node *) lfirst(lc);
        QualCost    qcost;

        cost_qual_eval_node(&qcost, clause, root);
        items[i].clause = clause;
        items[i].cost = qcost.per_tuple;
        if (IsA(clause, RestrictInfo))
        {
            RestrictInfo *rinfo = (RestrictInfo *) clause;

            /*
             * If a clause is leakproof, it doesn't have to be constrained by
             * its nominal security level.  If it's also reasonably cheap
             * (here defined as 10X cpu_operator_cost), pretend it has
             * security_level 0, which will allow it to go in front of
             * more-expensive quals of lower security levels.  Of course, that
             * will also force it to go in front of cheaper quals of its own
             * security level, which is not so great, but we can alleviate
             * that risk by applying the cost limit cutoff.
             */
            if (rinfo->leakproof && items[i].cost < 10 * cpu_operator_cost)
                items[i].security_level = 0;
            else
                items[i].security_level = rinfo->security_level;
        }
        else
            items[i].security_level = 0;
        i++;
    }

    /*
     * Sort.  We don't use qsort() because it's not guaranteed stable for
     * equal keys.  The expected number of entries is small enough that a
     * simple insertion sort should be good enough.
     */
    for (i = 1; i < nitems; i++)
    {
        QualItem    newitem = items[i];
        int            j;

        /* insert newitem into the already-sorted subarray */
        for (j = i; j > 0; j--)
        {
            QualItem   *olditem = &items[j - 1];

            if (newitem.security_level > olditem->security_level ||
                (newitem.security_level == olditem->security_level &&
                 newitem.cost >= olditem->cost))
                break;
            items[j] = *olditem;
        }
        items[j] = newitem;
    }

    /* Convert back to a list */
    result = NIL;
    for (i = 0; i < nitems; i++)
        result = lappend(result, items[i].clause);

    return result;
}

static AQPPSWRPath*
aqp_create_progressive_swr_path(PlannerInfo *root,
                                RelOptInfo *rel,
                                List *swrpaths,
                                TableSampleClause *tsc)
{
    AQPPSWRPath *path;
    AQPSWRPath  *first_path;

    Assert(swrpaths != NIL);

    path = (AQPPSWRPath*) palloc(sizeof(AQPPSWRPath));

    path->cpath.path.type = T_CustomPath;
    path->cpath.path.pathtype = T_CustomScan;
    path->cpath.path.parent = rel;
    path->cpath.path.pathtarget = rel->reltarget;
    path->cpath.path.param_info = get_baserel_parampathinfo(root, rel, NULL);

    /* TODO no parallel support in AB-tree for now */
    path->cpath.path.parallel_aware = false;
    path->cpath.path.parallel_safe = false;
    path->cpath.path.parallel_workers = 0;
    path->cpath.path.pathkeys = NIL;
    
    /* no backward scan or mark restore support in AB-tree */
    path->cpath.flags = 0;
    path->cpath.custom_paths = NIL;
    path->cpath.custom_private = NIL;
    path->cpath.methods = &aqp_progressive_swr_path_methods;

    /* index path info */
    path->swrpaths = swrpaths;

    /* 
     * NOTE We copy the first sample path's sampling info and cost
     * estimation, which should be the same for all sample paths anyway.
     */
    first_path = linitial(swrpaths);
    if (tsc->tsmhandler != aqp_pswr_tsm_handler_oid)
    {
        /* This is the old behavior. TODO remove this branch */
        path->sample_size_expr = first_path->sample_size_expr;
        path->sample_size = first_path->sample_size;
    }
    else
    {
        /* 
         * tsc will be fetched by the upper level pswrctl path & node,
         * which will tell us what to do in each sample batch.
         */
        path->sample_size_expr = NULL;
        path->sample_size = 0; /* dummy values */
    }
    path->repeatable_expr = first_path->repeatable_expr;
    
    /* XXX It is technically incorrect to use the same cost estimation as
     * the first path's but PG won't care since there's only one pswr path for
     * this baserel anyway.
     */
    path->cpath.path.rows = first_path->cpath.path.rows;
    path->cpath.path.startup_cost = first_path->cpath.path.startup_cost;
    path->cpath.path.total_cost = first_path->cpath.path.total_cost;

    return path;
}

static Plan*
aqp_plan_swr_path(PlannerInfo *root,
                  RelOptInfo *rel,
                  CustomPath *best_path,
                  List *tlist,
                  List *scan_clauses,
                  List *custom_plans)
{
    bool is_swr = best_path->methods == &aqp_swr_path_methods;
    Param *pswrctl_info_param;

    Assert(is_swr ||
        best_path->methods == &aqp_progressive_swr_path_methods);
    
    if (aqp_use_new_agg_impl)
    {
        pswrctl_info_param = generate_new_exec_param(root, INT8OID, -1,
                                                    InvalidOid);
    }
    else
    {
        pswrctl_info_param = NULL;
    }

    if (is_swr)
        return aqp_plan_swr_path_impl(root, rel, best_path, tlist, scan_clauses,
                                      custom_plans, pswrctl_info_param);

    return aqp_plan_progressive_swr_path_impl(root, rel, best_path, tlist,
                                              scan_clauses, custom_plans,
                                              pswrctl_info_param);
}

/*
 * This is adapted from create_indexscan_plan() in optimizer/plan/createplan.c,
 * except that we are using AB-tree for sampling.
 */
static Plan*
aqp_plan_swr_path_impl(PlannerInfo *root,
                       RelOptInfo *rel,
                       CustomPath *best_path,
                       List *tlist,
                       List *scan_clauses,
                       List *custom_plans,
                       Param *pswrctl_info_param)
{
    Scan                *scan;
    AQPSWRPath          *path = (AQPSWRPath *) best_path;
    List                *indexclauses = path->indexclauses;
    Index               baserelid = rel->relid;
    Oid                 indexoid = path->indexinfo->indexoid;
    List                *qpqual;
    List                *stripped_indexquals;
    List                *fixed_indexquals;
    ListCell            *l;
    bool                indexonly = path->index_only_scan;
    
    /*
     * We should be sampling a base relation.
     */
    Assert(baserelid > 0);
    Assert(path->cpath.path.parent->rtekind == RTE_RELATION);
    Assert(custom_plans == NIL);
    Assert(baserelid == path->cpath.path.parent->relid);

    /*
     * Extract the index qual expressions (stripped of RestrictInfos) from the
     * IndexClauses list, and prepare a copy with index Vars substituted for
     * table Vars.  (This step does not replace_nestloop_params on the
     * fixed_indexquals.)
     */
    aqp_pgport_fix_indexqual_references(root, path->indexinfo,
                                        path->indexclauses,
                                        &stripped_indexquals,
                                        &fixed_indexquals);
     
    /*
     * The qpqual list must contain all restrictions not automatically handled
     * by the index, other than pseudoconstant clauses which will be handled
     * by a separate gating plan node.  All the predicates in the indexquals
     * will be checked (either by the index itself, or by aqp_swrscan.c),
     * but if there are any "special" operators involved then they must be
     * included in qpqual.  The upshot is that qpqual must contain
     * scan_clauses minus whatever appears in indexquals.
     *
     * is_redundant_with_indexclauses() detects cases where a scan clause is
     * present in the indexclauses list or is generated from the same
     * EquivalenceClass as some indexclause, and is therefore redundant with
     * it, though not equal.  (The latter happens when indxpath.c prefers a
     * different derived equality than what generate_join_implied_equalities
     * picked for a parameterized scan's ppi_clauses.)  Note that it will not
     * match to lossy index clauses, which is critical because we have to
     * include the original clause in qpqual in that case.
     *
     * In some situations (particularly with OR'd index conditions) we may
     * have scan_clauses that are not equal to, but are logically implied by,
     * the index quals; so we also try a predicate_implied_by() check to see
     * if we can discard quals that way.  (predicate_implied_by assumes its
     * first input contains only immutable functions, so we have to check
     * that.)
     *
     * Note: if you change this bit of code you should also look at
     * extract_nonindex_conditions() in costsize.c.
     */
    qpqual = NIL;
    foreach(l, scan_clauses)
    {
        RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);

        if (rinfo->pseudoconstant)
            continue;            /* we may drop pseudoconstants here */
        if (is_redundant_with_indexclauses(rinfo, indexclauses))
            continue;            /* dup or derived from same EquivalenceClass */
        if (!contain_mutable_functions((Node *) rinfo->clause) &&
            predicate_implied_by(list_make1(rinfo->clause), stripped_indexquals,
                                 false))
            continue;            /* provably implied by indexquals */
        qpqual = lappend(qpqual, rinfo);
    }

    /* Sort clauses into best execution order */
    qpqual = aqp_pgport_order_qual_clauses(root, qpqual);

    /* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
    qpqual = extract_actual_clauses(qpqual, false);

    /*
     * XXX We probably need to replace the nestloop params here if we were
     * to support correlated variable references in nested queries, even if we
     * don't support joining a sampled table as the inner side (i.e., no outer
     * vars).
     */

    /* Finally ready to build the plan node. */
    if (indexonly)
        /* 
         * Different from create_indexscan_plan, we pass stripped_indexquals
         * (the indexquals that have Vars with the original varno rather than
         * INDX_VAR) here, mainly because we will put that into custom_exprs,
         * whose references are going to be fixed by fix_upper_expr and that
         * will complain about not being able to find the INDEX_VAR varno'd
         * Vars in the indextlist.
         */
        scan = (Scan *) aqp_make_swrindexonlyscan(tlist,
                                                  qpqual,
                                                  baserelid,
                                                  indexoid,
                                                  stripped_indexquals,
                                                  path->indexinfo->indextlist,
                                                  path->sample_size,
                                                  path->repeatable_expr,
                                                  path->sample_size_expr,
                                                  pswrctl_info_param);
    else
        scan = (Scan *) aqp_make_swrscan(tlist,
                                         qpqual,
                                         baserelid,
                                         indexoid,
                                         fixed_indexquals,
                                         stripped_indexquals,
                                         path->sample_size,
                                         path->repeatable_expr,
                                         path->sample_size_expr,
                                         pswrctl_info_param);

    /* no need to copy_generic_path_info: done in create_customscan_plan() */

    return (Plan*) scan;
}

static CustomScan*
aqp_make_swrindexonlyscan(List *qptlist,
                          List *qpqual,
                          Index scanrelid,
                          Oid indexid,
                          List *indexqual,
                          List *indextlist,
                          uint64 sample_size,
                          Expr *repeatable_expr,
                          Expr *sample_size_expr,
                          Param *pswrctl_info_param)
{
    CustomScan                      *node = makeNode(CustomScan); 
    Plan                            *plan = &node->scan.plan;
    AQPIndexSampleScanPrivate       *private;

    private = palloc(sizeof(AQPIndexSampleScanPrivate));

    plan->targetlist = qptlist;
    plan->qual = qpqual;
    plan->lefttree = NULL;
    plan->righttree = NULL;

    /* 
     * Providing scanrelid will make ExecInitCustomScan to open the relation
     * for us before it calls BeginCustomScan.
     */
    node->scan.scanrelid = scanrelid;
    /* no backward scan or mark/restore support */
    node->flags = 0;
    node->custom_plans = NIL;
    /* 
     * We put indexqual into the custom_exprs until setrefs.c fixes the
     * expressions. We wait until standard_planner() returns to move it
     * into private->indexqual.
     */
    node->custom_exprs = indexqual;
    node->custom_private = list_make1(private);
    /*
     * Providing the indextlist as custom_scan_tlist will make
     * ExecInitCustomScan to initialize a scan slot with the type derived from
     * this target list, as well as projection info and result slot correctly.
     */
    node->custom_scan_tlist = indextlist;
    /* node->custom_relids is set by create_customscan_plan() */
    node->methods = &aqp_swrindexonlyscan_methods;
    
    private->extnode.type = T_ExtensibleNode;
    private->extnode.extnodename = AQPSWRIndexOnlyScanPrivateName;
    private->flags = AQP_ISSFLAG_INDEXONLY;
    private->indexid = indexid;
    private->indexqual = NIL;
    private->indexqualorig = NIL;
    private->sample_size = sample_size;
    private->repeatable_expr = repeatable_expr;
    private->sample_size_expr = sample_size_expr;
    private->pswrctl_info_param = pswrctl_info_param;

    return node;
}

static CustomScan*
aqp_make_swrscan(List *qptlist,
                 List *qpqual,
                 Index scanrelid,
                 Oid indexid,
                 List *indexqual,
                 List *indexqualorig,
                 uint64 sample_size,
                 Expr *repeatable_expr,
                 Expr *sample_size_expr,
                 Param *pswrctl_info_param)
{
    CustomScan                  *node = makeNode(CustomScan); 
    Plan                        *plan = &node->scan.plan;
    AQPIndexSampleScanPrivate   *private;

    private = palloc(sizeof(AQPIndexSampleScanPrivate));
    
    plan->targetlist = qptlist;
    plan->qual = qpqual;
    plan->lefttree = NULL;
    plan->righttree = NULL;

    /* 
     * Providing scanrelid will make ExecInitCustomScan to open the relation
     * for us before it calls BeginCustomScan.
     */
    node->scan.scanrelid = scanrelid;
    node->custom_plans = NIL;
    /* no backward scan or mark/restore support */
    node->flags = 0;
    /*
     * Similar to aqp_make_swrindexonlyscan() but we can pass them as
     * the regular custom_expres, so that nestloop var replacement happens
     * in create_customscan_plan().
     */
    node->custom_exprs = list_make2(indexqual, indexqualorig);
    node->custom_private = list_make1(private);
    /*
     * Must be NIL because we're returning heap tuples.
     */
    node->custom_scan_tlist = NIL;
    /* node->custom_relids is set by create_customscan_plan() */
    node->methods = &aqp_swrscan_methods;

    private->extnode.type = T_ExtensibleNode;
    private->extnode.extnodename = AQPSWRScanPrivateName;
    private->flags = 0;
    private->indexid = indexid;
    /* We still need fix_scan_list to fix the range table offsets. */
    private->indexqual = NIL;
    private->indexqualorig = NIL;
    private->sample_size = sample_size;
    private->repeatable_expr = repeatable_expr;
    private->sample_size_expr = sample_size_expr;
    private->pswrctl_info_param = pswrctl_info_param;

    return node;
}




static Plan*
aqp_plan_progressive_swr_path_impl(PlannerInfo *root,
                                   RelOptInfo *rel,
                                   CustomPath *best_path,
                                   List *tlist,
                                   List *scan_clauses,
                                   List *custom_plans,
                                   Param *pswrctl_info_param)
{
    AQPPSWRPath         *path = (AQPPSWRPath *) best_path;
    CustomScan          *pswrscan;
    Plan                *plan;
    List                *privates = NIL;
    ListCell            *lc; 
    Param               *running_sample_size_param;
    Param               *running_sample_budget_param;
    Param               *running_state_id_param;

    /*
     * We should be sampling a base relation.
     */
    Assert(rel->relid > 0);
    Assert(path->cpath.path.parent->rtekind == RTE_RELATION);
    Assert(custom_plans == NIL);
    Assert(rel->relid == path->cpath.path.parent->relid);
   

    /* Set a param for ni */
    /* 
     * These are for the old impl of progressive sampling's aggregation
     * operators.
     */
    if (!aqp_use_new_agg_impl)
    {
        running_sample_size_param = generate_new_exec_param(root, FLOAT8OID, -1,
                                                            InvalidOid);

        running_sample_budget_param = generate_new_exec_param(root, FLOAT8OID,
                                                              -1, InvalidOid);

        running_state_id_param = generate_new_exec_param(root, FLOAT8OID, -1,
                                                         InvalidOid);
    }
    else
    {
        running_sample_size_param = NULL;
        running_sample_budget_param = NULL;
        running_state_id_param = NULL;
    }

    
    /* 
     * We plan each individual swr path, extract their private structures, and
     * then discard the generated CustomPlans. They will be ultimately packed
     * into a single CustomScan node that represents the PSWR plan.
     */
    foreach(lc, path->swrpaths)
    {
        AQPSWRPath *swrpath = (AQPSWRPath*) lfirst(lc);
        CustomScan *cscan;
        AQPProgressiveSampleScanPrivate *private;
        /*Oid indexoid = swrpath->indexinfo->indexoid;
        Relation indexrel = index_open(indexoid, NoLock);*/
        HeapTuple htup;
	    Form_pg_type indexkey_type;
        TypeCacheEntry *typentry;
        Param *lower_param;
        Param *upper_param;


        cscan = (CustomScan *) aqp_plan_swr_path_impl(root, rel,
                                                      (CustomPath *) swrpath,
                                                      tlist,
                                                      scan_clauses,
                                                      custom_plans,
                                                      pswrctl_info_param);

        private = (AQPProgressiveSampleScanPrivate *)
            repalloc(linitial(cscan->custom_private),
                     sizeof(AQPProgressiveSampleScanPrivate));
        private->issp.extnode.extnodename =
            AQPISSIsIndexOnly(private) ? AQPPSWRIndexOnlyScanPrivateName
                                       : AQPPSWRScanPrivateName;
        private->issp.flags |= AQP_ISSFLAG_PSWR_SUBPLAN;

        /* 
         * We will set this after the plan is fixed by setrefs.c
         * Note that cscan->scan.plan.targetlist is always the original qptlist
         * at this point so we don't need to save it here.
         */
        private->qptlist = NIL;
        private->qpqual = cscan->scan.plan.qual;

        if (!aqp_use_new_agg_impl)
        {
            /* Set two params for lowerkey and upperkey */
            lower_param = generate_new_exec_param(root, 
                    swrpath->indexinfo->opcintype[0], -1, 
                    swrpath->indexinfo->indexcollations[0]);
            upper_param = generate_new_exec_param(root,
                    swrpath->indexinfo->opcintype[0], -1,
                    swrpath->indexinfo->indexcollations[0]);

            private->running_sample_size_param = running_sample_size_param;
            private->running_sample_budget_param = running_sample_budget_param;
            private->running_state_id_param = running_state_id_param;

            /* 
            lower_param = makeNode(Param);
            lower_param->paramkind = PARAM_EXEC;
            lower_param->paramid = 0;
            lower_param->paramtype = swrpath->indexinfo->opcintype[0];
            lower_param->paramcollid = swrpath->indexinfo->indexcollations[0];
            lower_param->location = -1;

            upper_param = makeNode(Param);
            upper_param->paramkind = PARAM_EXEC;
            upper_param->paramid = 1;
            upper_param->paramtype = swrpath->indexinfo->opcintype[0];
            upper_param->paramcollid = swrpath->indexinfo->indexcollations[0];
            upper_param->location = -1;
            */
            
            private->lower_param = lower_param;
            private->upper_param = upper_param;
        }

        private->lower_opfuncid = 
            get_opcode(get_opfamily_member(swrpath->indexinfo->opfamily[0],
                                           swrpath->indexinfo->opcintype[0],
                                           swrpath->indexinfo->opcintype[0],
                                           4));
        private->upper_opfuncid = 
            get_opcode(get_opfamily_member(swrpath->indexinfo->opfamily[0],
                                           swrpath->indexinfo->opcintype[0],
                                           swrpath->indexinfo->opcintype[0],
                                           2));

    
        if (AQPISSIsIndexOnly(private))
        {
            private->indextlist = cscan->custom_scan_tlist;
            private->indexattr = 1;
        }
        else
        {
            private->indextlist = NIL;
            private->indexattr = swrpath->indexinfo->indexkeys[0];
            /*
            htup2 = SearchSysCache1(OPFAMILYOID, private->indexopfamily);
            index_type2 = (Form_pg_opfamily) GETSTRUCT(htup2);
            */
        }

        /* Collect infomation for comparator */
        /* RegProcedure cmp_proc;

        cmp_proc = get_opfamily_proc(swrpath->indexinfo->opfamily[0],
                                     swrpath->indexinfo->opcintype[0],
                                     swrpath->indexinfo->opcintype[0],
                                        ABTORDER_PROC); */

        htup = SearchSysCache1(TYPEOID, swrpath->indexinfo->opcintype[0]);
        indexkey_type = (Form_pg_type) GETSTRUCT(htup);

        private->typlen = indexkey_type->typlen;
        private->typbyval = indexkey_type->typbyval;

        ReleaseSysCache(htup);

        /* if (index_getprocid(indexrel, swrpath->indexinfo->indexkeys[0], ABTORDER_PROC) != InvalidOid)
		{
			fmgr_info_copy(&(private->compareFn),
						   index_getprocinfo(indexrel, swrpath->indexinfo->indexkeys[0], ABTORDER_PROC),
						   CurrentMemoryContext);
		}
		else
		{		*/	

        typentry = lookup_type_cache(swrpath->indexinfo->opcintype[0], TYPECACHE_CMP_PROC_FINFO);
        fmgr_info_copy(&(private->compareFn),
                        &(typentry->cmp_proc_finfo),
                        CurrentMemoryContext);

        /*index_close(indexrel, NoLock);*/
        /*private->indextype = swrpath->indexinfo->opcintype[0];
        private->indexopfamily = swrpath->indexinfo->opfamily[0];*/
        /*
        private->supportCollation = swrpath->indexinfo->indexcollations[0];
        */
        
        privates = lappend(privates, private);

        /* Discard the unused cscan node. */
        if (AQPISSIsIndexOnly(private))
        {
            private->issp.indexqual = cscan->custom_exprs;
            list_free(cscan->custom_private);
        }
        else
        {
            private->issp.indexqual = (List *) linitial(cscan->custom_exprs);
            private->issp.indexqualorig = (List *) lsecond(cscan->custom_exprs);
            list_free(cscan->custom_exprs);
            list_free(cscan->custom_private);
        }
        pfree(cscan);

    }
    
    /* 
     * Make the pswrscan node and filling the boilerplates.
     */
    pswrscan = makeNode(CustomScan);
    plan = &pswrscan->scan.plan;

    plan->targetlist = tlist;
    plan->qual = NIL; /* Will be filled during execution time. */
    plan->lefttree = NULL;
    plan->righttree = NULL;

    pswrscan->scan.scanrelid = rel->relid;
    pswrscan->custom_plans = NIL;
    pswrscan->flags = 0;
    pswrscan->custom_exprs = NIL;
    pswrscan->custom_private = privates;
    pswrscan->custom_scan_tlist = NIL;
    /* pswrscan->custom_relids is set by create_customscan_plan() */
    pswrscan->methods = &aqp_progressive_swrscan_methods;

    /* 
     * pswrscan->custom_exprs consists of the following:
     *
     * 1. A dummy variable to custom_exprs with varno being rel->relid. If
     * setrefs.c fixes the expr, the actual varno may be incremented with an
     * rtoffset if this is a subquery plan. We can then find out our new varno
     * for us to manually replace varno in swrindexonly plan with INDEX_VAR.
     *
     * 2. Subplan specific expressions. See below.
     */
    {
        Var *var = makeNode(Var);
        var->varno = rel->relid;
        var->varattno = SelfItemPointerAttributeNumber;
        var->vartype = TIDOID;
        var->vartypmod = 0;
        var->varcollid = InvalidOid;
        var->varlevelsup = 0;
        var->varnosyn = 0;
        var->varattnosyn = SelfItemPointerAttributeNumber;
        var->location = -1;

        pswrscan->custom_exprs = lappend(pswrscan->custom_exprs, var);
    }

    foreach(lc, pswrscan->custom_private)
    {
        AQPProgressiveSampleScanPrivate *private =
            (AQPProgressiveSampleScanPrivate *) lfirst(lc);
    
        /*
         * SWRIndexOnly: original index qual, qpqual, indextlist
         *
         * SWR: fixed index qual, original index qual, qpqual
         */
        if (AQPISSIsIndexOnly(private))
        {
            pswrscan->custom_exprs = lappend(pswrscan->custom_exprs,
                                             private->issp.indexqual);
            pswrscan->custom_exprs = lappend(pswrscan->custom_exprs,
                                             private->qpqual);
            pswrscan->custom_exprs = lappend(pswrscan->custom_exprs,
                                             private->indextlist);
        }
        else
        {
            pswrscan->custom_exprs = lappend(pswrscan->custom_exprs,
                                             private->issp.indexqual);
            pswrscan->custom_exprs = lappend(pswrscan->custom_exprs,
                                             private->issp.indexqualorig);
            pswrscan->custom_exprs = lappend(pswrscan->custom_exprs,
                                             private->qpqual);
        }
        elog(NOTICE, "plan %ld: %s %s",
                lc - pswrscan->custom_private->elements,
                AQPISSIsIndexOnly(private) ? "indexonlyswr": "swr",
                get_rel_name(private->issp.indexid));
    }

    return (Plan*) pswrscan;
}

void
aqp_fix_swrscan_exprs(Plan *plan)
{
    if (plan == NULL)
        return;
    
    if (IsA(plan, CustomScan))
    {
        CustomScan *cscan = (CustomScan *) plan;
        if (cscan->methods == &aqp_swrscan_methods)
        {
            aqp_fix_swrscan(cscan);
        }
        else if (cscan->methods == &aqp_swrindexonlyscan_methods)
        {
            aqp_fix_swrindexonlyscan(cscan);
        }
        else if (cscan->methods == &aqp_progressive_swrscan_methods)
        {
            aqp_fix_progressive_swrscan(cscan);
        }
    }
    else if (IsA(plan, SubqueryScan))
    {
        aqp_fix_swrscan_exprs(((SubqueryScan  *) plan)->subplan);
    }

    aqp_fix_swrscan_exprs(plan->lefttree);
    aqp_fix_swrscan_exprs(plan->righttree);
}

void
aqp_fix_swrscan(CustomScan *cscan)
{
    AQPIndexSampleScanPrivate *private =
        (AQPIndexSampleScanPrivate *) linitial(cscan->custom_private);
    List *indexqual = (List *) linitial(cscan->custom_exprs);
    List *indexqualorig = (List *) lsecond(cscan->custom_exprs);

    /* fix the indexqual and custom_scan_tlist fields */ 
    list_free(cscan->custom_exprs);
    cscan->custom_exprs = NIL;
    private->indexqual = indexqual;
    private->indexqualorig = indexqualorig;
}

void
aqp_fix_swrindexonlyscan(CustomScan *cscan)
{
    AQPIndexSampleScanPrivate *private =
        (AQPIndexSampleScanPrivate *) linitial(cscan->custom_private);

    private->indexqual = cscan->custom_exprs;
    cscan->custom_exprs = NULL;
}

void
aqp_fix_progressive_swrscan(CustomScan *cscan)
{
    ListCell    *lc_private; 
    ListCell    *lc_exprs;
    Var         *dummy_var;
    Index       rel_varno;
    List        *qptlist;
    
    /* 
     * Find the fixed varno for the baserel, see comment in
     * aqp_progressive_swr_path().
     */
    lc_exprs = list_head(cscan->custom_exprs);
    dummy_var = lfirst_node(Var, lc_exprs);
    rel_varno = dummy_var->varno;
    pfree(dummy_var);
    
    /* 
     * The fixed qptlist, from the plan itself. Note that this tlist could be
     * set by some upper caller to create_customscan_plan(), e.g.,
     * create_projection_plan(), so we might not have any chance of fixing it
     * yet.
     */
    qptlist = copyObject(cscan->scan.plan.targetlist);
    
    /*
     * Find and fix subplan specific exprs.
     */
    lc_exprs = lnext(cscan->custom_exprs, lc_exprs);
    foreach(lc_private, cscan->custom_private)
    {
        AQPProgressiveSampleScanPrivate *private =
            (AQPProgressiveSampleScanPrivate *) lfirst(lc_private);
        
        Assert(AQPISSIsPSWRSubPlan(private));
        if (AQPISSIsIndexOnly(private))
        {
            /* SWRIndexOnly */
            List *indexqual;
            List *qpqual;
            List *indextlist;
            AQPIndexedColMapping *idxcol_mapping;
            
            indexqual = lfirst_node(List, lc_exprs);
            lc_exprs = lnext(cscan->custom_exprs, lc_exprs);
            qpqual = lfirst_node(List, lc_exprs);
            lc_exprs = lnext(cscan->custom_exprs, lc_exprs);
            indextlist = lfirst_node(List, lc_exprs);
            lc_exprs = lnext(cscan->custom_exprs, lc_exprs);
            
            /* the indexqual is not fixed to refer to index columns yet */
            idxcol_mapping = aqp_build_indexed_col_mapping(indextlist); 
            private->issp.indexqual =
                (List *) aqp_fix_indexcol_refs((Node *) indexqual,
                                               rel_varno,
                                               idxcol_mapping);

            /* fix tlist and qpqual */
            private->qptlist = (List *) aqp_fix_indexcol_refs((Node *) qptlist,
                                                              rel_varno,
                                                              idxcol_mapping);
            private->qpqual = (List *) aqp_fix_indexcol_refs((Node *) qpqual,
                                                              rel_varno,
                                                              idxcol_mapping);

            /* indextlist must not be fixed to refer to index columns */
            private->indextlist = indextlist;
            pfree(idxcol_mapping);
        }
        else
        {
            /* SWR */
            private->issp.indexqual = lfirst_node(List, lc_exprs);
            lc_exprs = lnext(cscan->custom_exprs, lc_exprs);

            private->issp.indexqualorig = lfirst_node(List, lc_exprs);
            lc_exprs = lnext(cscan->custom_exprs, lc_exprs);

            private->qpqual = lfirst_node(List, lc_exprs);
            lc_exprs = lnext(cscan->custom_exprs, lc_exprs);

            private->qptlist = copyObject(qptlist);

            private->indextlist = NIL;
        }
    }

    /*
     * We store a copy of the original qptlist in custom_exprs in case we want
     * it back at some point.
     */
    cscan->custom_exprs = qptlist;
}

static AQPIndexedColMapping *
aqp_build_indexed_col_mapping(List *indextlist)
{
    AQPIndexedColMapping *mapping;
    ListCell    *lc; 
    AttrNumber maxattno = 0;
    
    foreach(lc, indextlist)
    {
        TargetEntry *tle = lfirst_node(TargetEntry, lc);
        Var *var;
        
        Assert(tle->expr);
        if (!IsA(tle->expr, Var))
        {
            elog(ERROR, "unexpected non-var indexed column in AB-tree");
        }
        var = (Var *) tle->expr;
        if (var->varattno > maxattno)
            maxattno = var->varattno;
    }

    mapping = (AQPIndexedColMapping *)
        palloc0(offsetof(AQPIndexedColMapping, tabattno2idxattno)
                + sizeof(AttrNumber) * (maxattno + 1));
    
    mapping->maxattno = maxattno;
    foreach(lc, indextlist)
    {
        TargetEntry *tle = lfirst_node(TargetEntry, lc);
        Var *var = (Var *) tle->expr;
        mapping->tabattno2idxattno[var->varattno] = tle->resno; 
    }
    return mapping;
}

static AttrNumber
aqp_lookup_indexed_col_attno(AQPIndexedColMapping *mapping,
                             AttrNumber tabattno)
{
    /* XXX can sysattr ever appear in index col refs? */
    if (tabattno < 0)
        return tabattno;
    if (tabattno > mapping->maxattno)
        return InvalidAttrNumber;
    return mapping->tabattno2idxattno[tabattno];
}

static Node *
aqp_fix_indexcol_refs(Node *node,
                      Index rel_varno,
                      AQPIndexedColMapping *idxcol_mapping)
{
    AQPFixIndexColRefCtx ctx;

    ctx.rel_varno = rel_varno;
    ctx.idxcol_mapping = idxcol_mapping;

    return aqp_fix_indexcol_refs_impl(node, &ctx);
}

static Node *
aqp_fix_indexcol_refs_impl(Node *node,
                           AQPFixIndexColRefCtx *ctx)
{
    if (node == NULL)
        return NULL;

    if (IsA(node, Var))
    {
        Var *var = (Var *) node;
        Var *newvar;
        AttrNumber idxattno; 
        
        if (var->varno != ctx->rel_varno)
        {
            elog(ERROR, "unexpectecd table reference: %d, expecting %d",
                 var->varno, ctx->rel_varno);
        }
        idxattno = aqp_lookup_indexed_col_attno(ctx->idxcol_mapping,
                                                var->varattno);

        if (idxattno == InvalidAttrNumber)
        {
            elog(ERROR, "index column not found for table column %d",
                 var->varattno);
        }
        
        newvar = copyObject(var);
        newvar->varno = INDEX_VAR;
        newvar->varattno = idxattno;
        return (Node *) newvar;
    }
    return expression_tree_mutator(node, aqp_fix_indexcol_refs_impl, ctx);
}

List*
aqp_reparameterize_custom_path_by_child(PlannerInfo *root,
                                        List *custom_private,
                                        RelOptInfo *child_rel)
{
    /* 
     * TODO to implement this, we need to move the additional fields in
     * AQPSWRPath into custom_private.
     *
     * NOTE we probably can't support partitioned nested loop right now
     * anyway
     */
    Assert(custom_private == NIL);
    return NIL;
}

static bool aqp_has_swr_path(RelOptInfo *rel) {
    ListCell *lc;
    foreach(lc, rel -> pathlist) 
    {
        Path *path = (Path *) lfirst(lc);
        if(path -> pathtype != T_CustomScan) 
        {
            continue;
        }
        if(strcmp(((CustomPath *) path) -> methods -> CustomName, AQPSWRScanPrivateName) == 0 ||
           strcmp(((CustomPath *) path) -> methods -> CustomName, AQPPSWRScanPrivateName) == 0) 
        {
            return true;
        }
    }
    return false;
}

static Path *aqp_pick_swr_path(RelOptInfo *rel, Relids required_outer, bool require_parameterized) {
    ListCell *lc;
    Path *best = NULL;
    foreach(lc, rel -> pathlist) {
        Path *path = (Path *) lfirst(lc);
        CustomPath *cpath;
        if(path -> pathtype != T_CustomScan) {
            continue;
        }
        if(require_parameterized) {
            if(path -> param_info == NULL) {
                continue;
            }
            if(required_outer != NULL && !bms_overlap(PATH_REQ_OUTER(path), required_outer)) {
                continue;
            }
        } 
        else if(path -> param_info != NULL) {
            continue;
        }
        cpath = (CustomPath *) path;
        if(strcmp(cpath -> methods -> CustomName, AQPSWRScanPrivateName) != 0 &&
           strcmp(cpath -> methods -> CustomName, AQPPSWRScanPrivateName) != 0) {
            continue;
        }
        if(best == NULL || compare_path_costs(path, best, TOTAL_COST) < 0) {
            best = path;
        }
    }
    return best;
}

static bool aqp_is_swr_rel(PlannerInfo *root, RelOptInfo *rel) {
    if(rel -> reloptkind == RELOPT_BASEREL) {
        return aqp_has_swr_path(rel);
    }
    if(rel -> reloptkind == RELOPT_JOINREL) {
        int relid = -1;
        while((relid = bms_next_member(rel -> relids, relid)) >= 0) {
            RelOptInfo *base_rel = root -> simple_rel_array[relid];
            if(base_rel == NULL) {
                continue;
            }
            if(!aqp_has_swr_path(base_rel)) {
                return false;
            }
        }
        return true;
    }
    return false;
}

static bool
aqp_rel_has_tablesample_handler(PlannerInfo *root, RelOptInfo *rel,
                                Oid tsmhandler)
{
    int relid;

    if (rel->reloptkind == RELOPT_BASEREL)
    {
        RangeTblEntry *rte;

        if (rel->relid <= 0 || rel->relid >= root->simple_rel_array_size)
            return false;

        rte = root->simple_rte_array[rel->relid];
        return rte != NULL &&
               rte->rtekind == RTE_RELATION &&
               rte->tablesample != NULL &&
               rte->tablesample->tsmhandler == tsmhandler;
    }

    relid = -1;
    while ((relid = bms_next_member(rel->relids, relid)) >= 0)
    {
        RangeTblEntry *rte;

        if (relid <= 0 || relid >= root->simple_rel_array_size)
            continue;

        rte = root->simple_rte_array[relid];
        if (rte != NULL &&
            rte->rtekind == RTE_RELATION &&
            rte->tablesample != NULL &&
            rte->tablesample->tsmhandler == tsmhandler)
            return true;
    }

    return false;
}

static TableSampleClause *aqp_get_tablesample_clause(PlannerInfo *root, RelOptInfo *rel) {
    RangeTblEntry *rte;
    if(rel -> relid == 0 || rel -> relid >= root -> simple_rel_array_size) {
        return NULL;
    }
    rte = root -> simple_rte_array[rel -> relid];
    if(rte == NULL || rte -> rtekind != RTE_RELATION) {
        return NULL;
    }
    return rte -> tablesample;
}
static Expr *aqp_canonicalize_join_clause(Expr *clause, RelOptInfo *innerrel) {
    OpExpr *op;
    Node *leftarg;
    Node *rightarg;
    bool left_is_inner = false;
    bool right_is_inner = false;
    if(!IsA(clause, OpExpr)) {
        return clause;
    }
    op = (OpExpr *) clause;
    if(list_length(op -> args) != 2) {
        return clause;
    }
    leftarg = strip_implicit_coercions(linitial(op -> args));
    rightarg = strip_implicit_coercions(lsecond(op -> args));

    if(IsA(leftarg, Var)) {
        left_is_inner = (((Var *) leftarg) -> varno == innerrel -> relid);
    }
    if(IsA(rightarg, Var)) {
        right_is_inner = (((Var *) rightarg) -> varno == innerrel -> relid);
    }
    if(right_is_inner && !left_is_inner) {
        OpExpr *newop = (OpExpr *) copyObject(op);
        Oid commutator = get_commutator(newop -> opno);
        Node *tmp;
        tmp = linitial(newop -> args);
        linitial(newop -> args) = lsecond(newop -> args);
        lsecond(newop -> args) = tmp;
        if(OidIsValid(commutator)) {
            newop -> opno = commutator;
            newop -> opfuncid = get_opcode(commutator);
        }
        return (Expr *) newop;
    }
    return clause;
}

static void aqp_create_parameterized_swr_paths(PlannerInfo *root, RelOptInfo *rel, Relids outer_relids, List *join_clauses) {
    TableSampleClause *tsc;
    ListCell *lc;
    
    tsc = aqp_get_tablesample_clause(root, rel);
    if(tsc == NULL)
        return;

    if (tsc->tsmhandler == aqp_pswr_tsm_handler_oid)
        return;

    foreach(lc, rel -> indexlist) {
        IndexOptInfo *index = (IndexOptInfo *) lfirst(lc);
        IndexClauseSet rclauseset;
        bool found_clause = false;
        int indexcol;
        if(index -> relam != ABTREE_AM_OID) {
            continue;
        }
        if(index -> indpred != NIL && !index -> predOK) {
            continue;
        }
        MemSet(&rclauseset, 0, sizeof(rclauseset));
        for(indexcol = 0; indexcol < index -> nkeycolumns; ++indexcol) {
            AttrNumber index_attno = index -> indexkeys[indexcol];
            ListCell *lc2;
            if(index_attno <= 0) {
                continue;
            }
            foreach(lc2, join_clauses) {
                RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc2);
                Expr *canonicalized_clause;
                RestrictInfo *canonical_rinfo;
                IndexClause *iclause;
                OpExpr *op;
                Node *leftarg;

                if(!bms_is_member(rel -> relid, rinfo -> clause_relids) || !bms_overlap(rinfo -> clause_relids, outer_relids) || !IsA(rinfo -> clause, OpExpr)) {
                    continue;
                }
                canonicalized_clause = aqp_canonicalize_join_clause(rinfo -> clause, rel);
                op = (OpExpr *) canonicalized_clause;
                if(list_length(op -> args) != 2) {
                    continue;
                }
                leftarg = strip_implicit_coercions(linitial(op -> args));
                if(!IsA(leftarg, Var)) {
                    continue;
                }
                if(((Var *) leftarg) -> varno != rel -> relid || ((Var *) leftarg) -> varattno != index_attno) {
                    continue;
                }
                iclause = makeNode(IndexClause);
                iclause -> indexcol = indexcol;
                iclause -> indexcols = list_make1_int(indexcol);

                canonical_rinfo = makeNode(RestrictInfo);
                *canonical_rinfo = *rinfo;
                canonical_rinfo -> clause = canonicalized_clause;

                iclause -> rinfo = canonical_rinfo;
                iclause -> indexquals = list_make1(canonical_rinfo);
                iclause -> lossy = false;

                rclauseset.indexclauses[indexcol] = lappend(rclauseset.indexclauses[indexcol], iclause);
                found_clause = true;
            }
        }
        if(found_clause) {
            List *paths = aqp_build_swr_path(root, rel, index, &rclauseset,tsc);
            ListCell *lc3;
            foreach(lc3, paths) {
                Path *path = (Path *) lfirst(lc3);
                if(path -> param_info != NULL && bms_overlap(path -> param_info -> ppi_req_outer, outer_relids)){
                    rel -> pathlist = lappend(rel -> pathlist, path);
                }
            }
        }
    }
}

static RelOptInfo *
aqp_join_search(PlannerInfo *root, int levels_needed, List *initial_rels)
{
    RelOptInfo *result;
    RelOptInfo *pswr_driver = NULL;
    List *join_order = NIL;
    ListCell *lc;
    bool has_swr = false;
    bool save_enable_mergejoin;
    bool save_enable_hashjoin;

    foreach(lc, initial_rels)
    {
        RelOptInfo *rel = (RelOptInfo *) lfirst(lc);

        if (aqp_has_swr_path(rel))
            has_swr = true;

        if (pswr_driver == NULL &&
            aqp_rel_has_tablesample_handler(root, rel,
                                            aqp_pswr_tsm_handler_oid))
            pswr_driver = rel;
    }

    if(!has_swr) {
        if(prev_join_search_hook) {
            return prev_join_search_hook(root, levels_needed, initial_rels);
        }
        return standard_join_search(root, levels_needed, initial_rels);
    }

    save_enable_hashjoin = enable_hashjoin;
    save_enable_mergejoin = enable_mergejoin;
    enable_hashjoin = false;
    enable_mergejoin = false;

    if (pswr_driver != NULL)
    {
        result = pswr_driver;

        foreach(lc, initial_rels)
        {
            RelOptInfo *rel = (RelOptInfo *) lfirst(lc);

            if (rel != pswr_driver)
                join_order = lappend(join_order, rel);
        }
    }
    else
    {
        result = (RelOptInfo *) linitial(initial_rels);

        for_each_cell(lc, initial_rels, list_second_cell(initial_rels))
            join_order = lappend(join_order, lfirst(lc));
    }

    foreach(lc, join_order)
    {
        RelOptInfo *next_rel = (RelOptInfo *) lfirst(lc);
        RelOptInfo *joinrel = make_join_rel(root, result, next_rel);

        if(joinrel == NULL) {
            elog(ERROR, "aqp: cannot build swr join tree between relids %s and %d",
                 bmsToString(result->relids), next_rel->relid);
        }

        if(aqp_pending_swr_npath != NULL) {
            joinrel->pathlist = NIL;
            joinrel->partial_pathlist = NIL;
            add_path(joinrel, (Path *) aqp_pending_swr_npath);
            aqp_pending_swr_npath = NULL;
        }

        set_cheapest(joinrel);
        result = joinrel;
    }

    enable_hashjoin = save_enable_hashjoin;
    enable_mergejoin = save_enable_mergejoin;

    return result;
}

static void aqp_set_join_pathlist(PlannerInfo *root, RelOptInfo *joinrel, RelOptInfo *outerrel, RelOptInfo *innerrel, JoinType jointype, JoinPathExtraData *extra) {
    bool outer_is_swr = aqp_is_swr_rel(root, outerrel);
    bool inner_is_swr = aqp_is_swr_rel(root, innerrel);
    bool outer_is_pswr = aqp_rel_has_tablesample_handler(root, outerrel,
                                                         aqp_pswr_tsm_handler_oid);
    bool inner_is_pswr = aqp_rel_has_tablesample_handler(root, innerrel,
                                                         aqp_pswr_tsm_handler_oid);
    RelOptInfo *swr_outer;
    RelOptInfo *swr_inner;
    Path *outer_path;
    Path *inner_path;
    JoinCostWorkspace workspace;
    Relids required_outer;
    NestPath *npath;

    if(!outer_is_swr || !inner_is_swr) {
        if(prev_set_join_pathlist_hook) {
            prev_set_join_pathlist_hook(root, joinrel, outerrel, innerrel, jointype, extra);
        }
        return;
    }
    if(outerrel->reloptkind == RELOPT_BASEREL &&
       innerrel->reloptkind == RELOPT_BASEREL)
    {
        if (outer_is_pswr && !inner_is_pswr)
        {
            swr_outer = outerrel;
            swr_inner = innerrel;
        }
        else if (inner_is_pswr && !outer_is_pswr)
        {
            swr_outer = innerrel;
            swr_inner = outerrel;
        }
        else if(outerrel->relid < innerrel->relid)
        {
            swr_outer = outerrel;
            swr_inner = innerrel;
        }
        else
        {
            swr_outer = innerrel;
            swr_inner = outerrel;
        }
    }
    else if(innerrel -> reloptkind == RELOPT_BASEREL) {
        swr_outer = outerrel;
        swr_inner = innerrel;
    }
    else if(outerrel -> reloptkind == RELOPT_BASEREL) {
        swr_outer = innerrel;
        swr_inner = outerrel;
    }
    else {
        return;
    }
    if(swr_outer != outerrel) {
        return;
    }
    aqp_create_parameterized_swr_paths(root, swr_inner, swr_outer -> relids, extra -> restrictlist);
    if(swr_outer -> reloptkind == RELOPT_BASEREL) {
        outer_path = aqp_pick_swr_path(swr_outer, NULL, false);
    }else {
        outer_path = swr_outer -> cheapest_total_path;
    }
    inner_path = aqp_pick_swr_path(swr_inner, swr_outer -> relids, true);
    if(outer_path == NULL || inner_path == NULL) {
        return;
    }
    initial_cost_nestloop(root, &workspace, jointype, outer_path, inner_path, extra);
    required_outer = calc_nestloop_required_outer(swr_outer -> relids, PATH_REQ_OUTER(outer_path), swr_inner -> relids, PATH_REQ_OUTER(inner_path));
    npath = create_nestloop_path(root, joinrel, jointype, &workspace, extra, outer_path, inner_path, extra -> restrictlist, NIL, required_outer);
    aqp_pending_swr_npath = npath;
}

void
aqp_setup_sample_path_hook(void)
{
    prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
    set_rel_pathlist_hook = aqp_set_rel_pathlist;

    prev_set_join_pathlist_hook = set_join_pathlist_hook;
    set_join_pathlist_hook = aqp_set_join_pathlist;

    prev_join_search_hook = join_search_hook;
    join_search_hook = aqp_join_search;
}

