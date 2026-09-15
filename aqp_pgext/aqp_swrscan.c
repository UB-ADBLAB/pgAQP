#include "aqp.h"

#include <miscadmin.h>
#include <access/abtree.h>
#include <access/attnum.h>
#include <access/genam.h>
#include <access/tableam.h>
#include <access/visibilitymap.h>
#include <catalog/pg_type.h>
#include <catalog/pg_amop.h>
#include <executor/executor.h>
#include <executor/tuptable.h>
#include <executor/nodeIndexscan.h>
#include <nodes/nodeFuncs.h>
#include <nodes/readfuncs.h>
#include <storage/itemptr.h>
#include <utils/builtins.h>
#include <utils/datum.h>
#include <utils/epoch.h>
#include <utils/lsyscache.h>
#include <utils/syscache.h>
#include <utils/timestamp.h>

#include <utils/fmgrprotos.h>

#include "pg_explain.h"
#include "aqp_planner.h"
#include "aqp_swrscan.h"
#include "aqp_explain.h"
#include "aqp_pswrctl_node.h"
#include "aqp_pagg.h"

#define AQPSWRScanName "AQPSWRScan"
#define AQPSWRScanStateName "AQPSWRScanState"
#define AQPSWRIndexOnlyScanName "AQPSWRIndexOnlyScan"
#define AQPSWRIndexOnlyScanStateName "AQPSWRIndexOnlyScanState"
#define AQPPSWRScanName "AQPProgressiveSWRScanName"
#define AQPPSWRScanStateName "AQPProgressiveSWRScanName"

/* Be aware of multiple evaluation of arguments! */
#define swap_ptr(a, b) \
    do { \
        if (&(a) != &(b)) { \
            *(uintptr_t*)&(a) ^= *(uintptr_t*)&(b); \
            *(uintptr_t*)&(b) ^= *(uintptr_t*)&(a); \
            *(uintptr_t*)&(a) ^= *(uintptr_t*)&(b); \
        } \
    } while (0) \

static void AQPIndexSampleScanPrivate_copy(ExtensibleNode *newnode,
                                           const ExtensibleNode *oldnode);
static bool AQPIndexSampleScanPrivate_equal(const ExtensibleNode *a,
                                            const ExtensibleNode *b);
static void AQPIndexSampleScanPrivate_out(StringInfo str,
                                          const ExtensibleNode *node);
static void AQPIndexSampleScanPrivate_read(ExtensibleNode *node);

static void aqp_swr_scan_begin(CustomScanState *node,
                               EState *estate,
                               int eflags);
static TupleTableSlot* aqp_swr_scan_exec(PlanState *node);
static void aqp_swr_scan_end(CustomScanState *node);
static void aqp_swr_scan_rescan(CustomScanState *node);
static void aqp_swr_scan_explain(CustomScanState *node,
                                 List *ancestors,
                                 ExplainState *es);

static void aqp_swr_index_only_scan_begin(CustomScanState *node,
                                          EState *estate,
                                          int eflags);
static TupleTableSlot* aqp_swr_index_only_scan_exec(PlanState *node);
static void aqp_swr_index_only_scan_end(CustomScanState *node);
static void aqp_swr_index_only_scan_rescan(CustomScanState *node);
static void aqp_swr_index_only_scan_explain(CustomScanState *node,
                                            List *ancestors,
                                            ExplainState *es);

static void aqp_progressive_swr_scan_begin(CustomScanState *node,
                               EState *estate,
                               int eflags);
static TupleTableSlot* aqp_progressive_swr_scan_exec(PlanState *node);
static void aqp_progressive_swr_scan_end(CustomScanState *node);
static void aqp_progressive_swr_scan_rescan(CustomScanState *node);
static void aqp_progressive_swr_scan_explain(CustomScanState *node,
                                             List *ancestors,
                                             ExplainState *es);


static void aqp_swr_scan_begin_new(CustomScanState *node,
                               EState *estate,
                               int eflags);
static TupleTableSlot* aqp_swr_scan_exec_new(PlanState *node);
static void aqp_swr_scan_end_new(CustomScanState *node);
static void aqp_swr_scan_rescan_new(CustomScanState *node);
/*static void aqp_swr_scan_explain(CustomScanState *node,
                                 List *ancestors,
                                 ExplainState *es); */

static void aqp_swr_index_only_scan_begin_new(CustomScanState *node,
                                          EState *estate,
                                          int eflags);
static TupleTableSlot* aqp_swr_index_only_scan_exec_new(PlanState *node);
static void aqp_swr_index_only_scan_end_new(CustomScanState *node);
static void aqp_swr_index_only_scan_rescan_new(CustomScanState *node);
/*static void aqp_swr_index_only_scan_explain(CustomScanState *node,
                                            List *ancestors,
                                            ExplainState *es); */

static void aqp_pswr_scan_begin(CustomScanState *node,
                               EState *estate,
                               int eflags);
static TupleTableSlot* aqp_pswr_scan_exec(PlanState *node);
static void aqp_pswr_scan_end(CustomScanState *node);
static void aqp_pswr_scan_rescan(CustomScanState *node);
/*static void aqp_pswr_scan_explain(CustomScanState *node,
                                             List *ancestors,
                                             ExplainState *es); */

static void aqp_pswr_continue_descent(IndexScanDesc scan, AQPPSWRCtlInfo pswrctl_info);
static void aqp_allocate_samples(ABTScanOpaque so, uint64 sample_size);
static void aqp_pswr_set_new_partitions(IndexScanDesc scan, AQPPSWRCtlInfo pswrctl_info);
static void aqp_pswr_batch_sampling_rescan(ABTScanOpaque so, AQPPSWRCtlInfo pswrctl_info);

static void aqp_pswr_partition_runtimekey(Relation index, 
                                          AQPProgressiveSampleScanSubplanState *subplan_state,
                                          ScanKey *scanKeys, int *numScanKeys,
					                      IndexRuntimeKeyInfo **runtimeKeys, int *numRuntimeKeys);
static TupleTableSlot* aqp_progressive_swr_scan_statis_collect_exec(PlanState *node);
static void aqp_statis_dpinfo(Relation index,
                              AQPProgressiveSampleScanSubplanState *subplan_state,
                              uint64 sample_budget);
static void aqp_statis_dp_calculate(Relation index,
                                    AQPProgressiveSampleScanSubplanState *subplan_state, 
                                    DPInfo *info, 
                                    int count, 
                                    uint64 sample_budget);

static void __attribute__((unused))
setup_dp_scankey(Relation index, 
                             AQPProgressiveSampleScanSubplanState *subplan_state,
                             ScanKey *scanKeys);
static double __attribute__((unused))
inv_prob_for_the_range(AQPProgressiveSampleScanSubplanState *subplan_state,
                                     Datum lower_key, Datum upper_key, 
                                     ScanKey scankeys, IndexScanDesc Scandesc);
static double** rate_calulate(double* group_p1, double* group_p2, int length);
static void minimizethecost(AQPProgressiveSampleScanSubplanState *subplan_state,
                            double* pp1, 
                            double* pp2, 
                            double* sum, 
                            double** p, 
                            int k, 
                            DPInfo *info,
                            uint64 sample_budget);
static void aqp_index_build_scankeys(PlanState *planstate,
                                     Relation index,
                                     List *quals,
                                     AQPPSWRCtlSubplanInfo subplan_info);
static void aqp_index_build_scankeys_impl(PlanState *planstate,
                                          Relation index,
                                          List *quals,
                                          AQPPSWRCtlSubplanInfo subplan_info,
                                          ScanKey lb,
                                          ScanKey ub,
                                          ScanKey scan_keys);
static inline bool aqp_is_lb_strategy(StrategyNumber strategy);
static inline bool aqp_is_ub_strategy(StrategyNumber strategy);
static inline bool aqp_is_strict_strategy(StrategyNumber strategy);
static bool aqp_compare_scankey_args(Relation index,
                                     AttrNumber attno,
                                     Oid optype,
                                     Oid opfuncid,
                                     Oid opcollid,
                                     StrategyNumber strat,
                                     Datum larg,
                                     Oid lefttype,
                                     Datum rarg,
                                     Oid righttype,
                                     bool *result);
static bool aqp_merge_current_qual_into_bounds(Relation index,
                                               ScanKey lb,
                                               int *p_nlb,
                                               ScanKey ub,
                                               int *p_nub,
                                               bool *emptyres,
                                               StrategyNumber op_strategy,
                                               Oid op_righttype,
                                               Oid opfuncid,
                                               Oid op_inputcollid,
                                               AttrNumber varattno,
                                               Datum scanvalue);
static bool aqp_merge_current_qual_into_bounds_impl(Relation index,
                                                    ScanKey lb,
                                                    int *p_nlb,
                                                    bool *emptyres,
                                                    StrategyNumber op_strategy,
                                                    Oid op_righttype,
                                                    Oid opfuncid,
                                                    Oid op_inputcollid,
                                                    AttrNumber varattno,
                                                    Datum scanvalue);
static void aqp_swrscan_fetch_pswrctl_info_param(
    AQPIndexSampleScanState *state,
    AQPIndexSampleScanPrivate *private,
    EState *estate);
static AQPPSWRCtlSubplanInfo aqp_swrscan_append_pswrctl_subplan_info(
        AQPPSWRCtlInfo pswrctl_info);

static int aqp_swrscan_append_sampler_ctl(AQPPSWRCtlInfo pswrctl_info);
static void aqp_swrscan_set_driver_sampler(AQPPSWRCtlInfo pswrctl_info,
                                           int sampler_id);
static AQPTableSamplerCtl aqp_swrscan_get_sampler_ctl(
    AQPIndexSampleScanState *state);

static ExtensibleNodeMethods aqp_swr_scan_private_methods = {
    AQPSWRScanPrivateName,
    sizeof(AQPIndexSampleScanPrivate),
    AQPIndexSampleScanPrivate_copy,
    AQPIndexSampleScanPrivate_equal,
    AQPIndexSampleScanPrivate_out,
    AQPIndexSampleScanPrivate_read
};

static ExtensibleNodeMethods aqp_swr_index_only_scan_private_methods = {
    AQPSWRIndexOnlyScanPrivateName,
    sizeof(AQPIndexSampleScanPrivate),
    AQPIndexSampleScanPrivate_copy,
    AQPIndexSampleScanPrivate_equal,
    AQPIndexSampleScanPrivate_out,
    AQPIndexSampleScanPrivate_read
};

static ExtensibleNodeMethods aqp_pswr_scan_private_methods = {
    AQPPSWRScanPrivateName,
    sizeof(AQPProgressiveSampleScanPrivate),
    AQPIndexSampleScanPrivate_copy,
    AQPIndexSampleScanPrivate_equal,
    AQPIndexSampleScanPrivate_out,
    AQPIndexSampleScanPrivate_read
};

static ExtensibleNodeMethods aqp_pswr_indexonly_scan_private_methods = {
    AQPPSWRIndexOnlyScanPrivateName,
    sizeof(AQPProgressiveSampleScanPrivate),
    AQPIndexSampleScanPrivate_copy,
    AQPIndexSampleScanPrivate_equal,
    AQPIndexSampleScanPrivate_out,
    AQPIndexSampleScanPrivate_read
};

/*
 * Node comparator.
 */
static int
aqp_irbt_cmp(const RBTNode *a, const RBTNode *b, void *arg)
{
	const DPInfoTreeNode *ea = (const DPInfoTreeNode *) a;
	const DPInfoTreeNode *eb = (const DPInfoTreeNode *) b;
    AQPProgressiveSampleScanSubplanState *subplan_state = 
        (AQPProgressiveSampleScanSubplanState *) arg;

	return DatumGetInt32(FunctionCall2Coll(&subplan_state->compareFn,
                                           subplan_state->supportCollation,
                                           ea->key, eb->key));
}

/*
 * Node combiner.
 */
static void
aqp_irbt_combine(RBTNode *existing, const RBTNode *newdata, void *arg)
{
	DPInfoTreeNode *eexist = (DPInfoTreeNode *) existing;
	/*const DPInfoTreeNode *enew = (const DPInfoTreeNode *) newdata;*/

    eexist->count_p1++;
}

/* Node allocator */
static RBTNode *
aqp_irbt_alloc(void *arg)
{
	return (RBTNode *) palloc(sizeof(DPInfoTreeNode));
}

/* Node freer */
static void
aqp_irbt_free(RBTNode *node, void *arg)
{
	pfree(node);
}

static void 
AQPIndexSampleScanPrivate_copy(ExtensibleNode *newnode_,
                       const ExtensibleNode *from_)
{
    AQPIndexSampleScanPrivate *newnode = (AQPIndexSampleScanPrivate *) newnode_;
    AQPIndexSampleScanPrivate *from = (AQPIndexSampleScanPrivate *) from_;
 
    newnode->flags = from->flags;
    newnode->indexid = from->indexid;
    newnode->indexqual = copyObject(from->indexqual);
    newnode->indexqualorig = copyObject(from->indexqualorig);
    newnode->sample_size = from->sample_size;
    newnode->repeatable_expr = copyObject(from->repeatable_expr);
    newnode->sample_size_expr = copyObject(from->sample_size_expr);
    newnode -> pswrctl_info_param = copyObject(from -> pswrctl_info_param);

    if (AQPISSIsPSWRSubPlan(from))
    {
        AQPProgressiveSampleScanPrivate *pssp_from =
            (AQPProgressiveSampleScanPrivate *) from;
        AQPProgressiveSampleScanPrivate *pssp_newnode =
            (AQPProgressiveSampleScanPrivate *) newnode;
        pssp_newnode->qptlist = copyObject(pssp_from->qptlist);
        pssp_newnode->qpqual = copyObject(pssp_from->qpqual);
        pssp_newnode->indextlist = copyObject(pssp_from->indextlist);
    }
}

static bool
AQPIndexSampleScanPrivate_equal(const ExtensibleNode *a,
                                const ExtensibleNode *b)
{
    elog(ERROR, "plan tree equal is not implemented");
    return false;
}

static void
AQPIndexSampleScanPrivate_out(StringInfo str,
                              const ExtensibleNode *node_)
{
    AQPIndexSampleScanPrivate *node = (AQPIndexSampleScanPrivate *) node_;
    bool is_indexonly = AQPISSIsIndexOnly(node);
    bool is_pswr_subplan = AQPISSIsPSWRSubPlan(node);
    
    appendStringInfo(str, " :is_pswr_subplan %s",
                     is_pswr_subplan ? "true" : "false");
    appendStringInfo(str, " :indexid %u", node->indexid);
    appendStringInfoString(str, " :indexqual ");
    outNode(str, node->indexqual);
    if (!is_indexonly)
    {
        appendStringInfoString(str, " :indexqualorig ");
        outNode(str, node->indexqualorig);
    }
    
    /* These are duplicated across different subplans. */
    appendStringInfo(str, " :sample_size " UINT64_FORMAT, node->sample_size);
    appendStringInfoString(str, " :repeatable_expr ");
    outNode(str, node->repeatable_expr);
    appendStringInfoString(str, " :sample_size_expr ");
    outNode(str, node->sample_size_expr);
    appendStringInfoString(str, " :pswrctl_info_param ");
    outNode(str, node -> pswrctl_info_param);

    if (is_pswr_subplan)
    {
        /* 
         * The subplan sample sizes are allocated by pswr so the original tsc
         * specifications are junk info here.
         */
        AQPProgressiveSampleScanPrivate *pssp =
            (AQPProgressiveSampleScanPrivate *) node;
        appendStringInfo(str, " :qptlist ");
        outNode(str, pssp->qptlist);
        appendStringInfo(str, " :qpqual ");
        outNode(str, pssp->qpqual);
        if (is_indexonly)
        {
            appendStringInfo(str, " :indextlist ");
            outNode(str, pssp->indextlist);
        }
    }
}

static void
AQPIndexSampleScanPrivate_read(ExtensibleNode *node_)
{
    AQPIndexSampleScanPrivate *local_node = (AQPIndexSampleScanPrivate *) node_;
    const char *token;
    int length;
    bool is_pswr_subplan;
    bool is_indexonly;

    local_node->flags = 0;

    /* :is_pswr_subplan */
    token = pg_strtok(&length);
    token = pg_strtok(&length);
    is_pswr_subplan = (*token == 't');

    if (is_pswr_subplan)
        local_node->flags |= AQP_ISSFLAG_PSWR_SUBPLAN;
    
    /* :indexid */
    token = pg_strtok(&length);
    token = pg_strtok(&length);
    local_node->indexid = atooid(token);

    /* :indexqual */
    token = pg_strtok(&length);
    local_node->indexqual = nodeRead(NULL, 0);

    token = pg_strtok(&length);
    if (length == 14 && token[1] == 'i')
    {
        /* :indexqualorig */
        local_node->indexqualorig = nodeRead(NULL, 0);
        is_indexonly = false;
    }
    else
    {
        /* :sample_size or :qptlist */
        local_node->indexqualorig = NIL;
        is_indexonly = true;
    }

    if (is_indexonly)
    {
        local_node->flags |= AQP_ISSFLAG_INDEXONLY;
        /* 
         * If this is not indexonly scan, we have consumed one extra token that
         * is :sample_size.
         *
         * If this is indexonly scan, consume the header token here so that we
         * can continue parsing the value of the field that immediately follows
         * the next field name.
         */
        token = pg_strtok(&length);
    }
    /* value in sample_size */
    token = pg_strtok(&length);
    local_node->sample_size = pg_strtouint64(token, NULL, 10);
    
    /* :repeatable_expr */
    token = pg_strtok(&length);
    local_node->repeatable_expr = nodeRead(NULL, 0);
    
    /* :sample_size_expr */
    token = pg_strtok(&length);
    local_node->sample_size_expr = nodeRead(NULL, 0);

    // pswrctl_info_param
    token = pg_strtok(&length);
    local_node -> pswrctl_info_param = nodeRead(NULL, 0);

    if (is_pswr_subplan)
    {
        AQPProgressiveSampleScanPrivate *pssp =
            (AQPProgressiveSampleScanPrivate *) local_node;

        /* value of qptlist */
        token = pg_strtok(&length);
        pssp->qptlist = nodeRead(NULL, 0);

        /* :qpqual */
        token = pg_strtok(&length);
        pssp->qpqual = nodeRead(NULL, 0);

        if (is_indexonly)
        {
            /* :indextlist */
            token = pg_strtok(&length);
            pssp->indextlist = nodeRead(NULL, 0);
        }
        else
        {
            pssp->indextlist = NIL;
        }
    }
}

CustomScanMethods aqp_swrscan_methods = {
    AQPSWRScanName,
    aqp_index_sample_scan_create_state
};

CustomScanMethods aqp_swrindexonlyscan_methods = {
    AQPSWRIndexOnlyScanName,
    aqp_index_sample_scan_create_state
};

CustomScanMethods aqp_progressive_swrscan_methods = {
    AQPPSWRScanName,
    aqp_index_sample_scan_create_state
};

CustomExecMethods aqp_swrscan_exec_methods = {
    /*CustomName=*/AQPSWRScanStateName,
    /*BeginCustomScan=*/aqp_swr_scan_begin,
    /*ExecCustomScan=*/
        (TupleTableSlot*(*)(CustomScanState*)) aqp_swr_scan_exec,
    /*EndCustomScan=*/aqp_swr_scan_end,
    /*ReScanCustomScan=*/aqp_swr_scan_rescan,
    /*MarkPosCustomScan=*/NULL,
    /*RestrPosCustomScan=*/NULL,
    /*EstimateDSMCustomScan=*/NULL,
    /*InitializeDSMCustomScan=*/NULL,
    /*ReInitializeDSMCustomScan=*/NULL,
    /*InitializeWorkerCustomScan=*/NULL,
    /*ShutdownCustomScan=*/NULL,
    /*ExplainCustomScan=*/aqp_swr_scan_explain
};

CustomExecMethods aqp_swrindexonlyscan_exec_methods = {
    /*CustomName=*/AQPSWRIndexOnlyScanStateName,
    /*BeginCustomScan=*/aqp_swr_index_only_scan_begin,
    /*ExecCustomScan=*/
        (TupleTableSlot*(*)(CustomScanState*)) aqp_swr_index_only_scan_exec,
    /*EndCustomScan=*/aqp_swr_index_only_scan_end,
    /*ReScanCustomScan=*/aqp_swr_index_only_scan_rescan,
    /*MarkPosCustomScan=*/NULL,
    /*RestrPosCustomScan=*/NULL,
    /*EstimateDSMCustomScan=*/NULL,
    /*InitializeDSMCustomScan=*/NULL,
    /*ReInitializeDSMCustomScan=*/NULL,
    /*InitializeWorkerCustomScan=*/NULL,
    /*ShutdownCustomScan=*/NULL,
    /*ExplainCustomScan=*/aqp_swr_index_only_scan_explain
};

CustomExecMethods aqp_progressive_swrscan_exec_methods = {
    /*CustomName=*/AQPPSWRScanStateName,
    /*BeginCustomScan=*/aqp_progressive_swr_scan_begin,
    /*ExecCustomScan=*/
        (TupleTableSlot*(*)(CustomScanState*)) aqp_progressive_swr_scan_exec,
    /*EndCustomScan=*/aqp_progressive_swr_scan_end,
    /*ReScanCustomScan=*/aqp_progressive_swr_scan_rescan,
    /*MarkPosCustomScan=*/NULL,
    /*RestrPosCustomScan=*/NULL,
    /*EstimateDSMCustomScan=*/NULL,
    /*InitializeDSMCustomScan=*/NULL,
    /*ReInitializeDSMCustomScan=*/NULL,
    /*InitializeWorkerCustomScan=*/NULL,
    /*ShutdownCustomScan=*/NULL,
    /*ExplainCustomScan=*/aqp_progressive_swr_scan_explain
};

CustomExecMethods aqp_swrscan_exec_methods_new = {
    /*CustomName=*/AQPSWRScanStateName,
    /*BeginCustomScan=*/aqp_swr_scan_begin_new,
    /*ExecCustomScan=*/NULL,
    /*EndCustomScan=*/aqp_swr_scan_end_new,
    /*ReScanCustomScan=*/aqp_swr_scan_rescan_new,
    /*MarkPosCustomScan=*/NULL,
    /*RestrPosCustomScan=*/NULL,
    /*EstimateDSMCustomScan=*/NULL,
    /*InitializeDSMCustomScan=*/NULL,
    /*ReInitializeDSMCustomScan=*/NULL,
    /*InitializeWorkerCustomScan=*/NULL,
    /*ShutdownCustomScan=*/NULL,
    /*ExplainCustomScan=*/aqp_swr_scan_explain
};

CustomExecMethods aqp_swrindexonlyscan_exec_methods_new = {
    /*CustomName=*/AQPSWRIndexOnlyScanStateName,
    /*BeginCustomScan=*/aqp_swr_index_only_scan_begin_new,
    /*ExecCustomScan=*/NULL,
    /*EndCustomScan=*/aqp_swr_index_only_scan_end_new,
    /*ReScanCustomScan=*/aqp_swr_index_only_scan_rescan_new,
    /*MarkPosCustomScan=*/NULL,
    /*RestrPosCustomScan=*/NULL,
    /*EstimateDSMCustomScan=*/NULL,
    /*InitializeDSMCustomScan=*/NULL,
    /*ReInitializeDSMCustomScan=*/NULL,
    /*InitializeWorkerCustomScan=*/NULL,
    /*ShutdownCustomScan=*/NULL,
    /*ExplainCustomScan=*/aqp_swr_index_only_scan_explain
};

CustomExecMethods aqp_pswrscan_exec_methods = {
    /*CustomName=*/AQPPSWRScanStateName,
    /*BeginCustomScan=*/aqp_pswr_scan_begin,
    /*ExecCustomScan=*/NULL,
    /*EndCustomScan=*/aqp_pswr_scan_end,
    /*ReScanCustomScan=*/aqp_pswr_scan_rescan,
    /*MarkPosCustomScan=*/NULL,
    /*RestrPosCustomScan=*/NULL,
    /*EstimateDSMCustomScan=*/NULL,
    /*InitializeDSMCustomScan=*/NULL,
    /*ReInitializeDSMCustomScan=*/NULL,
    /*InitializeWorkerCustomScan=*/NULL,
    /*ShutdownCustomScan=*/NULL,
    /*ExplainCustomScan=*/aqp_progressive_swr_scan_explain
};

Node *
aqp_index_sample_scan_create_state(CustomScan *cscan)
{
    AQPIndexSampleScanState *state;
    AQPIndexSampleScanPrivate *private;

    Assert(cscan->custom_private != NIL);
    private = (AQPIndexSampleScanPrivate *) linitial(cscan->custom_private);

    if (!AQPISSIsPSWRSubPlan(private))
    {
        /* non-pswr */
        state = (AQPIndexSampleScanState *)
            palloc0(sizeof(AQPIndexSampleScanState));
        if (AQPISSIsIndexOnly(private))
        {
            state->css.methods = aqp_use_new_agg_impl ?
                &aqp_swrindexonlyscan_exec_methods_new :
                &aqp_swrindexonlyscan_exec_methods; 
        }
        else
        {
            state->css.methods = aqp_use_new_agg_impl ?
                &aqp_swrscan_exec_methods_new :
                &aqp_swrscan_exec_methods;
        }

        aqp_pswr_tree = false;
    }
    else
    {
        /* pswr */
        int nsubplans = list_length(cscan->custom_private);
        AQPProgressiveSampleScanState *psss = (AQPProgressiveSampleScanState *)
            palloc0(offsetof(AQPProgressiveSampleScanState, subplan_states) +
                    sizeof(AQPProgressiveSampleScanSubplanState) * nsubplans);
        state = (AQPIndexSampleScanState *) psss;
        state->css.methods = aqp_use_new_agg_impl ? 
            &aqp_pswrscan_exec_methods :
            &aqp_progressive_swrscan_exec_methods;
    }

    
    /* common initialization */
    state->css.ss.ps.type = T_CustomScanState;
    /*
     * ps.plan, ps.state, ps.ExecProcNode, css.flags will be set up by
     * ExecInitCustomScan() 
     */
    state->css.custom_ps = NIL;
    state->css.pscan_len = 0;

    return (Node *) state;
}

static void
aqp_swr_scan_begin(CustomScanState *node,
                   EState *estate,
                   int eflags)
{
    LOCKMODE lockmode;
    AQPIndexSampleScanState *state = (AQPIndexSampleScanState *) node;
    CustomScan *scan = castNode(CustomScan, state->css.ss.ps.plan);
    AQPIndexSampleScanPrivate *private =
        (AQPIndexSampleScanPrivate *) linitial(scan->custom_private);
    Relation baserel;
    List *targetlist_with_no_prob = NULL;
    int nredundant_tupleslot;
    int i;
    bool is_pswr_subplan = AQPISSIsPSWRSubPlan(private);
    bool proj_info_set = false;
    bool want_prob;

    /* 
     * If this is a pswr subplan, our caller should have taken care of the
     * cleanups.
     */
    if (is_pswr_subplan)
    {
        Assert(state->css.ss.ps.qual == NULL);
        Assert(state->css.ss.ss_ScanTupleSlot == NULL);
    }
    else
    {
        /*
         * At this point, ExecInitCustomScan() has:
         * 1) assigned the expression context for this node;
         * 2) opened the base relation (state->css.ss.ss_CurrentRelation);
         * 3) initialized the scan tuple slot the type derived from the heap
         * relation and using the TTSOpsVirtual (which is not what we want!).
         * Because the ttsops is fixed at this point, we have to discard
         * everything starting from 3) until 5)....
         * 4) initialized the result tuple slot as a virtual tuple with the
         * heap relation relid as the expected varno in the tlist;
         * 5) initialized the plan qual (the additional plan qual not covered
         * by the index).
         */
        
        /* 
         * NOTE similar to swr_index_only_scan, we want to save a few
         * cycles of the indirect call from ExecCustomScan
         */
        state->css.ss.ps.ExecProcNode = aqp_swr_scan_exec;
        
        /* 
         * Unlike index only scan, where the scan tuple slot is ok, we need to
         * replace the scan slot with heap tuple TTSOps.
         */
        Assert(state->css.ss.ss_ScanTupleSlot);
        
        /* 
         * Unfortunately, there's not too much we can do with this ExprState
         * created in ExecInitCustomScan(). Hopefully it's not wasting too much
         * memory.
         */
        state->css.ss.ps.qual = NULL;

        /*
         * The same goes with the expression inside the projection info.
         * However, let's at least free the proj info.
         */
        if (state->css.ss.ps.ps_ProjInfo)
        {
            pfree(state->css.ss.ps.ps_ProjInfo);
            state->css.ss.ps.ps_ProjInfo = NULL;
        }

        /* 
         * We can also remove the extra tuple slot from the tuple slot table.
         * They should be at the end of estate->es_tupleTable.
         */
        if (state->css.ss.ps.ps_ResultTupleSlot)
            nredundant_tupleslot = 2;
        else
            nredundant_tupleslot = 1;
        Assert(list_length(estate->es_tupleTable) >= nredundant_tupleslot);
        for (i = 0; i < nredundant_tupleslot; ++i)
        {
            TupleTableSlot *slot =
                (TupleTableSlot *) llast(estate->es_tupleTable);
            if (slot == state->css.ss.ps.ps_ResultTupleSlot ||
                slot == state->css.ss.ss_ScanTupleSlot)
            {
                estate->es_tupleTable = list_delete_last(estate->es_tupleTable);
                if (slot->tts_tupleDescriptor)
                {
                   ReleaseTupleDesc(slot->tts_tupleDescriptor);
                   slot->tts_tupleDescriptor = NULL;
                }
                if (!TTS_FIXED(slot))
                {
                   if (slot->tts_values)
                       pfree(slot->tts_values);
                   if (slot->tts_isnull)
                       pfree(slot->tts_isnull);
                }
                pfree(slot);
            }
            else
            {
                ereport(ERROR,
                        errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("AQPSWRScan failed to find the redundant tuple "
                               "slots made by ExecInitCustomScan()"));
            }
        }
        state->css.ss.ps.ps_ResultTupleSlot = NULL;
        state->css.ss.ss_ScanTupleSlot = NULL;
    }

    /* 
     * OK, we're ready to do our own init. The following is similar to
     * ExecInitIndexScan().
     */
    baserel = state->css.ss.ss_currentRelation;
    state->css.ss.ss_currentScanDesc = NULL; /* no heap scan here */

    /* 
     * We can share the same scan slot and projection info across
     * all pswr subplans that are swr scans. This will reset the scan tuple
     * slot to the previously allocated scan tuple slot so that we'll skip
     * allocating a new one.
     */
    if (is_pswr_subplan)
    {
        AQPProgressiveSampleScanState *psss =
            (AQPProgressiveSampleScanState *) state;
        if (psss->swr_scanslot != NULL)
        {
            Assert(psss->swr_scan_descriptor != NULL);

            state->css.ss.ss_ScanTupleSlot = psss->swr_scanslot;
            state->css.ss.ps.scandesc = psss->swr_scan_descriptor;
            state->css.ss.ps.scanops = psss->swr_scanops;
            state->css.ss.ps.scanopsset = true;
            
            state->css.ss.ps.ps_ProjInfo = psss->swr_projInfo;
            state->css.ss.ps.resultopsset = psss->swr_resultopsset;
            state->css.ss.ps.resultopsfixed = psss->swr_resultopsfixed;
            state->css.ss.ps.resultops = psss->swr_resultops;
            proj_info_set = true;

            state->allnullslot = psss->swr_allnullslot;
        }
    }

    /*
     * Find out whether we want to include the probability column in the scan
     * tuple slot.
     *
     * NOTE We assume the plan rewriter always appends an extra entry in the
     * target list where varno == scan->scan.scanrelid and varattno == natts +
     * 1 where natts is the number of columns in the scan table, if it wants
     * to include the probability column in the scan tuple slot.
     */

    want_prob = false;
    if (list_length(scan->scan.plan.targetlist) != 0)
    {
        TargetEntry *tle = llast_node(TargetEntry, scan->scan.plan.targetlist);
        if (IsA(tle->expr, Var))
        {
            Var *var = (Var *) tle->expr;
            if (var->varno == scan->scan.scanrelid)
            {
                TupleDesc desc = RelationGetDescr(baserel);
                if (var->varattno == desc->natts + 1)
                {
                    /* ok, this is the probability column */
                    want_prob = true;

                    /* 
                     * A shorter targetlist without the probability column that
                     * will be used for initializing the projection info.
                     */
                    targetlist_with_no_prob =
                        list_copy(scan->scan.plan.targetlist);
                    targetlist_with_no_prob =
                        list_delete_last(targetlist_with_no_prob);
                }
            }
        }
    }

    /* 
     * Either all pswr subplans want the sample prob column or none wants that. 
     */
    Assert(!is_pswr_subplan ||
        ((AQPProgressiveSampleScanState *) state)->selected_subplan_idx == 0 ||
        state->want_prob == want_prob);
    state->want_prob = want_prob;
        
    /*
     * Initialize scan slot (and scan type).
     */
    if (state->css.ss.ss_ScanTupleSlot == NULL)
    {
        ExecInitScanTupleSlot(estate, &state->css.ss,
                              RelationGetDescr(baserel),
                              table_slot_callbacks(baserel));
        
        state->allnullslot = 
            ExecStoreAllNullTuple(ExecAllocTableSlot(&estate->es_tupleTable,
                                                     RelationGetDescr(baserel),
                                                     table_slot_callbacks(baserel)));
    }

    /*
     * Initialize result type and projection. Result type can be shared across
     * all pswr subplans (including swr scan and swr index only scans).
     */
    if (state->css.ss.ps.ps_ResultTupleDesc == NULL)
    {
        ExecInitResultTypeTL(&state->css.ss.ps);
    }
    
    /* Build project info. */
    if (!proj_info_set)
    {
        if (state->want_prob)
        {
            /* 
             * Has one extra prob column. We need to use the shorter targetlist
             * to initialize the projection info so that it won't try to fetch
             * an non-existent column.
             *
             * Here, we must assign a projection info because the true target
             * list never matches the scan rel tuple descriptor.
             */
            if (!state->css.ss.ps.ps_ResultTupleSlot)
            {
                /* 
                 * Some other PSWR subplan may have initialized the result tuple
                 * slot.
                 */
                ExecInitResultSlot(&state->css.ss.ps, &TTSOpsVirtual);
                state->css.ss.ps.resultops = &TTSOpsVirtual;
                state->css.ss.ps.resultopsfixed = true;
                state->css.ss.ps.resultopsset = true;
            }

            state->css.ss.ps.ps_ProjInfo =
                ExecBuildProjectionInfo(targetlist_with_no_prob,
                                        state->css.ss.ps.ps_ExprContext,
                                        state->css.ss.ps.ps_ResultTupleSlot,
                                        &state->css.ss.ps,
                                        state->css.ss.ss_ScanTupleSlot
                                            ->tts_tupleDescriptor);
        }
        else
        {
            /* 
             * No prob column. We can initialize the projection info as is. 
             * 
             * Note that this is safe to call for a PSWR subplan even if some
             * other PSWR subplan that is index-only and has initialized a
             * result tuple slot, which will be simply ignored (if 
             * the tlist matches the heap tuple descriptor), or reused (if they
             * do not match).
             */
            ExecAssignScanProjectionInfo(&state->css.ss);
        }
    }

    /*
     * Initialize child expressions.
     */
    state->css.ss.ps.qual =
        ExecInitQual(scan->scan.plan.qual, &state->css.ss.ps);
    state->indexqualorig =
        ExecInitQual(private->indexqualorig, &state->css.ss.ps);

    /* 
     * EXPLAIN stops here without opening the index.
     * See nodeIndexscan.c for rationale.
     */
    if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
    {
        /* 
         * The caller, aqp_progressive_swr_scan_begin(), will handle any
         * rewriting needed for EXPLAIN VERBOSE.
         */
        if (is_pswr_subplan)
            return;

        /* 
         * Explain only, we need to replace the Vars with invalid
         * references into the heap tuple with sample_prob() in the
         * query plan right now, before it is sent for printing.
         */
        if (aqp_is_in_explain_verbose)
            aqp_rewrite_sample_prob_dummy_var_for_explain(&state->css.ss.ps);
        return;
    }

    if (!IsMVCCSnapshot(estate->es_snapshot))
    {
        /*
         * It's not ok to use non-MVCC snapshot for index sample scan
         * because, otherwise, there might be multiple valid tuples in
         * one HOT chain matching one TID. That breaks the assumption that
         * one TID sampled TID from the index produces exactly one tuple.
         *
         * Do this check as early as possible to prevent wasting efforts
         * in all the initialization. However, we don't really care if this
         * is an EXPLAIN command.
         */
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("TABLESAMPLE SWR() must be used under MVCC "
                        "snapshot")));
    }

    /* open the index */
    lockmode = exec_rt_fetch(scan->scan.scanrelid, estate)->rellockmode;
    state->indexrel = index_open(private->indexid, lockmode);

    /* index-specific scan state */
    state->runtime_keys_ready = false;
    state->num_runtime_keys = 0;
    state->runtime_keys = NULL;

    /* 
     * index scan keys for the indexqual
     *
     * XXX We don't allow array keys currently. Can we? 
     *
     * TODO this is not right for pswr paths since we'd need to reset
     * the partition boundaries during runtime.
     */
    ExecIndexBuildScanKeys((PlanState *) state,
                           state->indexrel,
                           private->indexqual,
                           /*isorderby=*/false,
                           &state->scan_keys,
                           &state->num_scan_keys,
                           &state->runtime_keys,
                           &state->num_runtime_keys,
                           /*arrayKeys=*/NULL,
                           /*numArrayKeys=*/NULL);
    
    /* TODO ? */
    if (!is_pswr_subplan)
        state->runtime_context = NULL;
    if (state->num_runtime_keys > 0)
    {
        /* runtime context can also be shared across pswr */
        if (state->runtime_context == NULL)
            state->runtime_context = CreateExprContext(estate);
    }

    /* 
     * Initialize the sample scan states. But only do so when this is not a
     * subplan, in which case there is only one single shared sample scan
     * state.
     */
    if (!is_pswr_subplan)
    {
        aqp_sample_scan_init(&state->sss,
                             (PlanState*) state,
                             private->repeatable_expr,
                             private->sample_size,
                             private->sample_size_expr);
        aqp_sample_scan_start(&state->sss,
                              state->css.ss.ps.ps_ExprContext);
    }

    /* create the scan desc */
    if (is_pswr_subplan && state->num_scan_keys < 2)
    {    
        state->scandesc = index_beginscan(state->css.ss.ss_currentRelation,
                                            state->indexrel,
                                            state->css.ss.ps.state->es_snapshot,
                                            2,
                                            0);
        state->scandesc->numberOfKeys = state->num_scan_keys;
    }
    else
    {
        state->scandesc = index_beginscan(state->css.ss.ss_currentRelation,
                                state->indexrel,
                                state->css.ss.ps.state->es_snapshot,
                                state->num_scan_keys,
                                /*norderbys=*/0);
    }

    /* no runtime keys. We can pass the scan keys to AM at this time */
    if (state->num_runtime_keys == 0)
    {
        index_rescan(state->scandesc,
                     state->scan_keys,
                     state->num_scan_keys,
                     /*orderbys=*/NULL,
                     /*norderbys=*/0);
    }
}

static TupleTableSlot*
aqp_swr_scan_exec(PlanState *node)
{
    AQPIndexSampleScanState *state = (AQPIndexSampleScanState *) node;
    CustomScan *scan = castNode(CustomScan, state->css.ss.ps.plan);
    AQPIndexSampleScanPrivate *private =
        (AQPIndexSampleScanPrivate *) linitial(scan->custom_private);
    ExprState       *qual;
    ProjectionInfo  *projInfo;
    ExprContext     *econtext;
    IndexScanDesc   scandesc;
    TupleTableSlot  *slot;
    bool is_pswr_subplan = AQPISSIsPSWRSubPlan(private);

    if (aqp_sample_scan_no_more_samples(&state->sss))
    {
        /* we've got enough samples */
        if (is_pswr_subplan) {
            /* let pswr exec to decide on the next subplan to execute */
            return aqp_progressive_swr_scan_exec(node);
        }
        return NULL;
    }

    if (is_pswr_subplan)
    {
        
        state->running_sample_size->value ++;
    }


    qual = state->css.ss.ps.qual;
    projInfo = state->css.ss.ps.ps_ProjInfo;
    econtext = state->css.ss.ps.ps_ExprContext;
    scandesc = state->scandesc;
    slot = state->css.ss.ss_ScanTupleSlot;

    /* first call: set up runtime keys */
    if (state->num_runtime_keys > 0 && !state->runtime_keys_ready)
    {
        aqp_sample_scan_mark_rescan_for_runtime_keys_only(&state->sss);
        aqp_swr_scan_rescan(&state->css);
    }

#define REJECT_TUPLE() \
    if (state->want_prob) \
    { \
        aqp_sample_scan_got_new_sample(&state->sss); \
        if (projInfo) { \
            TupleTableSlot *result_slot; \
            econtext->ecxt_scantuple = state->allnullslot; \
            result_slot = ExecProject(projInfo); \
            result_slot->tts_isnull[result_slot->tts_nvalid - 1] = true; \
            return result_slot; \
        } \
        return state->allnullslot; \
    } \
    continue;

    for (;;)
    {
        double random_number;
        ItemPointer tid;

        ResetExprContext(econtext);
    
        /* 
         * We now do a separate call to index_samplenext_tid here
         * because we may have to fetch the heap tuple into a separate
         * tuple slot when the caller asks for the sampling probability.
         * In that case, we will have to deform the tuple into a virtual tuple
         * so that we can add the extra column to the scan tuple.
         */
        random_number = aqp_sample_scan_next_random_number(&state->sss);
        tid = index_samplenext_tid(scandesc, random_number);

        /* warn the user if the rejection rate is too high */
        aqp_sample_scan_check_for_high_rejection_rate(&state->sss);

        epoch_maybe_refresh();

        CHECK_FOR_INTERRUPTS();

        if (tid == NULL)
        {
            /* AB-tree rejection
                    no TID in range or probabilistic reject.
                     after warning once, (threshold promoted to inf), treat this as an effectively empty
                     range for the current outer key
                     and return NULL so nestloop can advance.
             */
            if(isinf(state -> sss.HighRejectionRateWarningThreshold)) {
                return NULL;
            }
            REJECT_TUPLE();
        }

        /* now fetch the heap tuple */
        if (!index_fetch_heap(scandesc, slot))
        {
            /* not visible */
            REJECT_TUPLE();
        }
        
        Assert(!scandesc->xs_recheck);

        /* now evaluate the plan qual and projection if any */
        econtext->ecxt_scantuple = slot;
        if (qual == NULL || ExecQual(qual, econtext))
        {
            /* passes qual or no qual */
            aqp_sample_scan_got_new_sample(&state->sss);

            if (state->want_prob)
            {
                TupleTableSlot *result_slot;
                double inv_prob = ((ABTScanOpaque) scandesc->opaque)->inv_prob;
                //elog(INFO, "inv_prob = %f", inv_prob);
                if (state->is_partition){
                    inv_prob = inv_prob*(1/state->percentage);
                    //elog(INFO, "inv_prob = %f, percent = %f", inv_prob, state->percentage);
                }
                econtext->ecxt_scantuple = slot;
                result_slot = ExecProject(projInfo);
                result_slot->tts_isnull[result_slot->tts_nvalid - 1] = false;
                result_slot->tts_values[result_slot->tts_nvalid - 1] =
                    Float8GetDatum(1.0 / inv_prob);
                return result_slot;
            }
            else
            {
                if (projInfo)
                {
                    return ExecProject(projInfo);
                }
                else
                {
                    return slot;
                }
            }
        }
        else
        {
            state->p2matter = true;
            REJECT_TUPLE();
        }
    }


#undef REJECT_TUPLE

    elog(ERROR, "unreachable");
    return NULL;
}


static void
aqp_swr_scan_end(CustomScanState *node)
{
    AQPIndexSampleScanState *state = (AQPIndexSampleScanState *) node;
    
    /* 
     * NOTE we have to set result tuple slot to something with
     * a valid ttsops, or ExecEndCustomScan() will blindly invoke
     * ExecClearTuple on a NULL pointer!
     *
     * We don't need to invoke clear either of the scan or the result tuple
     * though. It will be handled by ExecEndCustomScan.
     */
    if (!state->css.ss.ps.ps_ResultTupleSlot)
        state->css.ss.ps.ps_ResultTupleSlot =
            state->css.ss.ss_ScanTupleSlot;

    if (state->scandesc)
        index_endscan(state->scandesc);
    if (state->indexrel)
        index_close(state->indexrel, NoLock);
}

static void
aqp_swr_scan_rescan(CustomScanState *node)
{
    AQPIndexSampleScanState *state = (AQPIndexSampleScanState *) node;
    
    /* recompute the runtime keys on each rescan if there's any */
    if (state->num_runtime_keys > 0)
    {
        ExprContext *econtext = state->runtime_context;
        ResetExprContext(econtext);
        ExecIndexEvalRuntimeKeys(econtext,
                                 state->runtime_keys,
                                 state->num_runtime_keys);
    }
    state->runtime_keys_ready = true;

    if (state->scandesc)
    {
        /*
            reject any heap buffer pin from the previousscan iteration before restarting. abtreescan() resets the index page but does not release
            xs_heapfetch -> xs_cbuf, causing one extra heap-page pin per nestloop rescan (which accumulates into "buffer refcount leak on exit")
           */
        if(state -> scandesc -> xs_heapfetch != NULL) {
            table_index_fetch_reset(state -> scandesc -> xs_heapfetch);
        }

        index_rescan(state->scandesc,
                     state->scan_keys,
                     state->num_scan_keys,
                     /*orderbys=*/NULL,
                     /*norderbys=*/0);
    }

    aqp_sample_scan_start(&state->sss, state->css.ss.ps.ps_ExprContext);
    /*
        always clear the runtime keys only flag after a full rescan. the flag is set by the first exec path to skip re seeding on the very first outer tuple
        but if it is left true all subsequent nestloop rescans would skip resetting remsamplesize, causing the inner scan to return null for outer tuple after
        the first.
       */
    state -> sss.RescanForRuntimeKeysOnly = false;
}

static void
aqp_swr_scan_explain(CustomScanState *node,
                     List *ancestors,
                     ExplainState *es)
{
    CustomScan *cscan = (CustomScan*) node->ss.ps.plan;
    AQPIndexSampleScanPrivate *private =
        (AQPIndexSampleScanPrivate *) linitial(cscan->custom_private);
    const char *index_name;

    index_name = get_rel_name(private->indexid);
    if (!index_name)
        elog(ERROR, "cache lookup failed for index %u", private->indexid);
    ExplainPropertyText("Index Name", index_name, es);

    aqp_pgport_show_scan_qual(private->indexqualorig, "Index Cond",
                              &node->ss.ps, ancestors, es);
    aqp_show_index_sample_scan_details(private->sample_size,
                                       private->sample_size_expr,
                                       private->repeatable_expr,
                                       &node->ss.ps,
                                       ancestors,
                                       es);
}

static void
aqp_swr_index_only_scan_begin(CustomScanState *node,
                              EState *estate,
                              int eflags)
{
    LOCKMODE lockmode;
    AQPIndexSampleScanState *state = (AQPIndexSampleScanState *) node;
    CustomScan *scan = castNode(CustomScan, state->css.ss.ps.plan);
    AQPIndexSampleScanPrivate *private =
        (AQPIndexSampleScanPrivate *) linitial(scan->custom_private);
    int scan_tlist_len;
    int itupdesc_natts;
    bool is_pswr_subplan = AQPISSIsPSWRSubPlan(private);
    bool want_prob;

    /*
     * At this point, ExecInitCustomScan() has:
     * 1) assigned the expression context for this node;
     * 2) opened the base relation (state->css.ss.ss_CurrentRelation);
     * 3) initialized the scan tuple slot with type derived from
     * `scan->custom_scan_tlist`;
     * 4) initialized the result tuple slot as a virtual tuple with INDEX_VAR
     * as the expected varno in the tlist;
     * 5) initialized the plan qual (the additional plan qual not covered
     * by the index).
     */
    
    if (!is_pswr_subplan)
    {
        /* 
         * NOTE maybe save a few cycles of the indirect call from
         * ExecCustomScan
         */
        state->css.ss.ps.ExecProcNode = aqp_swr_index_only_scan_exec;
    }
    
    /* no heap scan; just to be extra cautious in case this is set */
    state->css.ss.ss_currentScanDesc = NULL;
    
    /* another table slot for visibility check */
    if (state->table_slot == NULL)
    {
        /* 
         * All PSWR subplans that are index-only can share one single table
         * slot for visibility check (unless we want to support parallelism).
         */
        state->table_slot = ExecAllocTableSlot(&estate->es_tupleTable,
            RelationGetDescr(state->css.ss.ss_currentRelation),
            table_slot_callbacks(state->css.ss.ss_currentRelation));
    }

    /* need to initialize indexqual */
    state->indexqual =
        ExecInitQual(private->indexqual, (PlanState *) state);

    /* 
     * NOTE we may have an extra result tuple slot if the projection info
     * is NULL because of matching indextlist and target list.
     */
    if (is_pswr_subplan)
    {
        /*
         * If this is a pswr subplan, ExecInitCustomScan() never had a chance
         * to initialize the scan & result tuple slot yet, though someone may
         * have initialized the result tuple slot for us if this is not the
         * first subplan.
         */
        TupleDesc   scan_descriptor;

        scan_descriptor = ExecTypeFromTL(scan->custom_scan_tlist);
        ExecInitScanTupleSlot(estate, &state->css.ss, scan_descriptor,
                              &TTSOpsVirtual);

        if (state->css.ss.ps.ps_ResultTupleDesc == NULL)
        {
            ExecInitResultTypeTL(&state->css.ss.ps);
        }
        if (state->css.ss.ps.ps_ResultTupleSlot == NULL)
        {
            ExecInitResultSlot(&state->css.ss.ps, &TTSOpsVirtual);
        }
        
        /* Initialize projection info. */
        ExecAssignScanProjectionInfoWithVarno(&state->css.ss, INDEX_VAR);

        /* Initialize qp qual. */
        state->css.ss.ps.qual =
            ExecInitQual(scan->scan.plan.qual, &state->css.ss.ps);
    }

    state->allnullslot = 
        ExecStoreAllNullTuple(ExecAllocTableSlot(&estate->es_tupleTable,
							                     ExecTypeFromTL(scan->custom_scan_tlist),
                                                 &TTSOpsVirtual));
    
    /* open the index */
    lockmode = (eflags & EXEC_FLAG_EXPLAIN_ONLY) ? NoLock
        : exec_rt_fetch(scan->scan.scanrelid, estate)->rellockmode;
    state->indexrel = index_open(private->indexid, lockmode);

    /* 
     * This needs to happen earlier than early return for EXPLAIN, as
     * aqp_swr_scan_begin() expects any prior subplans have consistently set
     * state->want_prob.
     */
    scan_tlist_len = list_length(scan->custom_scan_tlist);
    itupdesc_natts = IndexRelationGetNumberOfAttributes(state->indexrel);
    if (list_length(scan->custom_scan_tlist) > itupdesc_natts)
    {
        if (scan_tlist_len != itupdesc_natts + 1)
        {
            ereport(ERROR,
                    errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("AQPSWRIndexOnlyScan scan tlist length != : "
                           "itupdesc->natts + 1: %d %d",
                           scan_tlist_len,
                           itupdesc_natts));
        }
        want_prob = true;
    }
    else
    {
        if (scan_tlist_len != itupdesc_natts)
        {
            ereport(ERROR,
                    errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("AQPSWRIndexOnlyScan scan tlist length != : "
                           "itupdesc->natts: %d %d",
                           scan_tlist_len,
                           itupdesc_natts));
        }
        want_prob = false;
    }

    /* 
     * Either all pswr subplans want the sample prob column or none wants that. 
     */
    Assert(!is_pswr_subplan ||
        ((AQPProgressiveSampleScanState *) state)->selected_subplan_idx == 0 ||
        state->want_prob == want_prob);
    state->want_prob = want_prob;

    /* 
     * EXPLAIN stops here without opening the index.
     * See nodeIndexonlyscan.c for rationale.
     */
    if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
    {
        index_close(state->indexrel, NoLock);
        state->indexrel = NULL;

        /* 
         * The caller, aqp_progressive_swr_scan_begin(), will handle any
         * rewriting needed for EXPLAIN VERBOSE.
         */
        if (is_pswr_subplan)
            return;

        /* 
         * Explain only, we need to replace the Vars with invalid
         * references into the heap tuple with sample_prob() in the
         * query plan right now, before it is sent for printing.
         */
        if (aqp_is_in_explain_verbose)
            aqp_rewrite_sample_prob_dummy_var_for_explain((PlanState *) state);
        return;
    }

    if (!IsMVCCSnapshot(estate->es_snapshot))
    {
        /*
         * It's not ok to use non-MVCC snapshot for index sample scan
         * because, otherwise, there might be multiple valid tuples in
         * one HOT chain matching one TID. That breaks the assumption that
         * one TID sampled TID from the index produces exactly one tuple.
         *
         * Do this check as early as possible to prevent wasting efforts
         * in all the initialization. However, we don't really care if this
         * is an EXPLAIN command.
         */
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("TABLESAMPLE SWR() must be used under MVCC "
                        "snapshot")));
    }
    
    /* index-specific scan state */
    state->runtime_keys_ready = false;
    state->num_runtime_keys = 0;
    state->runtime_keys = NULL;
    
    /* 
     * index scan keys for the indexqual
     *
     * XXX We don't allow array keys currently. Can we? 
     */
    ExecIndexBuildScanKeys((PlanState *) state,
                           state->indexrel,
                           private->indexqual,
                           /*isorderby=*/false,
                           &state->scan_keys,
                           &state->num_scan_keys,
                           &state->runtime_keys,
                           &state->num_runtime_keys,
                           /*arrayKeys=*/NULL,
                           /*numArrayKeys=*/NULL);
    
    state->runtime_context = NULL;
    if (state->num_runtime_keys > 0)
    {
        state->runtime_context = CreateExprContext(estate);
    }

    state->vmbuffer = InvalidBuffer;
    
    /* initialize the sample scan states */
    if (!is_pswr_subplan)
    {
        aqp_sample_scan_init(&state->sss,
                             (PlanState*) state,
                             private->repeatable_expr,
                             private->sample_size,
                             private->sample_size_expr);
        aqp_sample_scan_start(&state->sss,
                              state->css.ss.ps.ps_ExprContext);
    }


    /* create the scan desc */ 
    if (is_pswr_subplan && state->num_scan_keys < 2)
    {    
        state->scandesc = index_beginscan(state->css.ss.ss_currentRelation,
                                            state->indexrel,
                                            state->css.ss.ps.state->es_snapshot,
                                            2,
                                            0);
        state->scandesc->numberOfKeys = state->num_scan_keys;
    }
    else
    {
        state->scandesc = index_beginscan(state->css.ss.ss_currentRelation,
                                state->indexrel,
                                state->css.ss.ps.state->es_snapshot,
                                state->num_scan_keys,
                                /*norderbys=*/0);
    }

    state->scandesc->xs_want_itup = true;
    
    /* no runtime keys. We can pass the scan keys to AM at this time */
    if (state->num_runtime_keys == 0 && !state->pswrctl_info)
    {
        index_rescan(state->scandesc,
                     state->scan_keys,
                     state->num_scan_keys,
                     /*orderbys=*/NULL,
                     /*norderbys=*/0);
    }
}

static TupleTableSlot*
aqp_swr_index_only_scan_exec(PlanState *node)
{
    AQPIndexSampleScanState *state = (AQPIndexSampleScanState *) node;
    CustomScan *cscan = castNode(CustomScan, node->plan);
    AQPIndexSampleScanPrivate *private = linitial(cscan->custom_private);
    ExprState       *qual;
    ProjectionInfo  *projInfo;
    ExprContext     *econtext;
    IndexScanDesc   scandesc;
    TupleTableSlot  *slot;
    bool is_pswr_subplan = AQPISSIsPSWRSubPlan(private);

    if (aqp_sample_scan_no_more_samples(&state->sss))
    {
        /* we've got enough samples */
        if (is_pswr_subplan) {
            /* let pswr exec to decide on the next subplan to execute */
            return aqp_progressive_swr_scan_exec(node);
        }
        return NULL;
    }

    if (is_pswr_subplan)
    {
        
        state->running_sample_size->value ++;
    }

    qual = state->css.ss.ps.qual;
    projInfo = state->css.ss.ps.ps_ProjInfo;
    econtext = state->css.ss.ps.ps_ExprContext;
    scandesc = state->scandesc;
    slot = state->css.ss.ss_ScanTupleSlot;
    
    /* first call: set up runtime keys */
    if (state->num_runtime_keys > 0 && !state->runtime_keys_ready)
    {
        aqp_sample_scan_mark_rescan_for_runtime_keys_only(&state->sss);
        aqp_swr_index_only_scan_rescan(&state->css);
    }

#define REJECT_TUPLE() \
    if (state->want_prob) \
    { \
        aqp_sample_scan_got_new_sample(&state->sss); \
        if (projInfo) { \
            econtext->ecxt_scantuple = state->allnullslot; \
            return ExecProject(projInfo); \
        } \
        return state->allnullslot; \
    } \
    continue;

    for (;;)
    {
        /*bool    tuple_from_heap; */
        double random_number;
        ItemPointer tid;

        ResetExprContext(econtext); 

        random_number = aqp_sample_scan_next_random_number(&state->sss);
        tid = index_samplenext_tid(scandesc, random_number);
    
        /* warn the user if the rejection rate is too high */
        aqp_sample_scan_check_for_high_rejection_rate(&state->sss);

        epoch_maybe_refresh();

        CHECK_FOR_INTERRUPTS();

        if (tid == NULL)
        {
            /* AB-tree rejection */
            if(isinf(state -> sss.HighRejectionRateWarningThreshold)) {
                return NULL;
            }
            REJECT_TUPLE();
        }

        /*tuple_from_heap = false; */
        
        /* 
         * See nodeIndexonlyscan.c for notes on the memory ordering effects
         * and why no lock or barrier is needed for checking visibility map.
         */
        if (!VM_ALL_VISIBLE(scandesc->heapRelation,
                            ItemPointerGetBlockNumber(tid),
                            &state->vmbuffer))
        {
            /* 
             * Not all heap tuples on this heap page is visible.
             * Go for the heap tuple to run visibility check.
             */
            if (!index_fetch_heap(scandesc, state->table_slot))
            {
                /* not visible */
                REJECT_TUPLE();
            }
            ExecClearTuple(state->table_slot);
            
            /* 
             * Same as nodeIndexonlyscan.c. We're not expecting non-MVCC
             * snapshots.
             */
            Assert(!scandesc->xs_heap_continue);
            if (scandesc->xs_heap_continue)
                elog(ERROR, "non-MVCC snapshots are not supported in index-only swr sample scan");

            /*tuple_from_heap = true; */
        }


        
        /* AB-tree never sets xs_hitup or ask us to recheck for index quals */
        Assert(scandesc->xs_hitup == NULL);
        Assert(!scandesc->xs_recheck);
        Assert(scandesc->xs_itup);

        /* fill data into the result slot */
        ExecClearTuple(slot);
        index_deform_tuple(scandesc->xs_itup,
                           scandesc->xs_itupdesc,
                           slot->tts_values,
                           slot->tts_isnull);
        if (state->want_prob)
        {
            double inv_prob = ((ABTScanOpaque) scandesc->opaque)->inv_prob;
            if (state->is_partition)
                inv_prob = inv_prob*(1/state->percentage);
            /* store the probability into the last column if we're asked to */
            slot->tts_values[scandesc->xs_itupdesc->natts] =
                Float8GetDatum(1.0 / inv_prob);
            slot->tts_isnull[scandesc->xs_itupdesc->natts] = false;
        }
        ExecStoreVirtualTuple(slot);
        
        /* 
         * XXX temporarily removed because I don't think page level
         * SI lock would make transactions with sampling serializable.
         *
         * Table level SI lock definitely works but maybe that's too
         * conservative.
         *
         * This requires further investigation.
         */
        /* take the SILock if we haven't read the tuple from heap */
        /*if (!tuple_from_heap)
            PredicateLockPage(scandesc->heapRelation,
                              ItemPointerGetBlockNumber(tid),
                              state->css.ss.ps.state->es_snapshot); */
        
        /* now evaluate the plan qual and projection if any */
        econtext->ecxt_scantuple = slot;
        if (qual == NULL || ExecQual(qual, econtext))
        {
            /* passes qual or no qual */
            aqp_sample_scan_got_new_sample(&state->sss);
            if (projInfo)
            {
                return ExecProject(projInfo);
            }
            else
            {
                return slot;
            }
        }
        else
        {
            /* oops, rejected by the qual */
            state->p2matter = true;
            REJECT_TUPLE();
        }
        
    }
#undef REJECT_TUPLE
    
    elog(ERROR, "unreachable");
    return NULL;
}

static void
aqp_swr_index_only_scan_end(CustomScanState *node)
{
    AQPIndexSampleScanState *state = (AQPIndexSampleScanState *) node;
    
    /* release visibility map buffer pin */
    if (state->vmbuffer != InvalidBuffer)
    {
        ReleaseBuffer(state->vmbuffer);
        state->vmbuffer = InvalidBuffer;
    }

    /* no need to call ExecFreeExprContext (see that for reason) */
    
    /* 
     * NOTE we have to set result tuple slot to something with
     * a valid ttsops, or ExecEndCustomScan() will blindly invoke
     * ExecClearTuple on a NULL pointer!
     *
     * Note that table_slot is cleared on every heap fetch, so we don't
     * need to clear it again here.
     *
     * We don't need to invoke clear either of the scan or the result tuple
     * though. It will be handled by ExecEndCustomScan.
     */
    if (!state->css.ss.ps.ps_ResultTupleSlot)
        state->css.ss.ps.ps_ResultTupleSlot =
            state->css.ss.ss_ScanTupleSlot;

    if (state->scandesc)
        index_endscan(state->scandesc);
    if (state->indexrel)
        index_close(state->indexrel, NoLock);
}

static void
aqp_swr_index_only_scan_rescan(CustomScanState *node)
{
    AQPIndexSampleScanState *state = (AQPIndexSampleScanState *) node;
    
    /* recompute the runtime keys on each rescan if there's any */
    if (state->num_runtime_keys > 0)
    {
        ExprContext *econtext = state->runtime_context;
        ResetExprContext(econtext);
        ExecIndexEvalRuntimeKeys(econtext,
                                 state->runtime_keys,
                                 state->num_runtime_keys);
    }
    state->runtime_keys_ready = true;

    if (state->scandesc)
    {
        // release any heap buffer pin from the prev scan iter
        if(state -> scandesc -> xs_heapfetch != NULL) {
            table_index_fetch_reset(state -> scandesc -> xs_heapfetch);
        }
        index_rescan(state->scandesc,
                     state->scan_keys,
                     state->num_scan_keys,
                     /*orderbys=*/NULL,
                     /*norderbys=*/0);
    }

    aqp_sample_scan_start(&state->sss, state->css.ss.ps.ps_ExprContext);
    // same rescanfor runtimekeys only fix as in aqp_swr_scan_rescan
    state -> sss.RescanForRuntimeKeysOnly = false;
}

static void
aqp_swr_index_only_scan_explain(CustomScanState *node,
                                List *ancestors,
                                ExplainState *es)
{
    CustomScan *cscan = (CustomScan*) node->ss.ps.plan;
    AQPIndexSampleScanPrivate *private =
        (AQPIndexSampleScanPrivate *) linitial(cscan->custom_private);
    const char *index_name;

    index_name = get_rel_name(private->indexid);
    if (!index_name)
        elog(ERROR, "cache lookup failed for index %u", private->indexid);
    ExplainPropertyText("Index Name", index_name, es);

    aqp_pgport_show_scan_qual(private->indexqual, "Index Cond",
                              &node->ss.ps, ancestors, es);
    aqp_show_index_sample_scan_details(private->sample_size,
                                       private->sample_size_expr,
                                       private->repeatable_expr,
                                       &node->ss.ps,
                                       ancestors,
                                       es);
}

static void
aqp_progressive_swr_scan_begin(CustomScanState *node,
                               EState *estate,
                               int eflags)
{
    AQPProgressiveSampleScanState *psss =
        (AQPProgressiveSampleScanState *) node; 
    CustomScan *cscan = castNode(CustomScan, node->ss.ps.plan);
    AQPIndexSampleScanPrivate *issp0 =
        (AQPIndexSampleScanPrivate *) linitial(cscan->custom_private);
    int nredundant_tupleslot;
    int i;

    /*
     * At this point, ExecInitCustomScan() has:
     * 1) assigned the expression context for this node;
     * 2) opened the base relation (psss->css.ss.ss_CurrentRelation);
     * 3) initialized the scan tuple slot the type derived from the heap
     * relation and using the TTSOpsVirtual (which is not what we want!).
     * Because the ttsops is fixed at this point, we have to discard everything
     * starting from 3) until 5)....
     * 4) initialized the result tuple slot as a virtual tuple with the heap
     * relation relid as the expected varno in the tlist;
     * 5) initialized the plan qual (the additional plan qual not covered
     * by the index).
     */

    /*
     * We might have multiple subplans, we will may need multiple scan slots
     * for storing heap tuples (for swr) or index tuples (for swr indexonly).
     * In any case, the scan slot we have right now must be discarded.
     */
    Assert(psss->isss.css.ss.ss_ScanTupleSlot);

    /*
     * We shouldn't have a qual here since we didn't set one during planning.
     * Each subplan will have to initialize their own quals based on the
     * corresponding scan slots.
     */
    Assert(cscan->scan.plan.qual == NIL);
    Assert(!psss->isss.css.ss.ps.qual);
    
    /*
     * Projection info is also invalid so let's reset it here. Note that we
     * can't do anything about the memory leaks we have here for the ExprState
     * inside the project info. Again, each subplan would have their own
     * projection info set depending on the corresponding scan slots.
     */
    if (psss->isss.css.ss.ps.ps_ProjInfo)
    {
        pfree(psss->isss.css.ss.ps.ps_ProjInfo);
        psss->isss.css.ss.ps.ps_ProjInfo = NULL;
    }

    /* 
     * We can also remove the extra tuple slot from the tuple slot table. They
     * should be at the end of estate->es_tupleTable.
     */
    if (psss->isss.css.ss.ps.ps_ResultTupleSlot)
        nredundant_tupleslot = 2;
    else
        nredundant_tupleslot = 1;
    Assert(list_length(estate->es_tupleTable) >= nredundant_tupleslot);
    for (i = 0; i < nredundant_tupleslot; ++i)
    {
        TupleTableSlot *slot = (TupleTableSlot *) llast(estate->es_tupleTable);
        if (slot == psss->isss.css.ss.ps.ps_ResultTupleSlot ||
            slot == psss->isss.css.ss.ss_ScanTupleSlot)
        {
            estate->es_tupleTable = list_delete_last(estate->es_tupleTable);
            if (slot->tts_tupleDescriptor)
            {
               ReleaseTupleDesc(slot->tts_tupleDescriptor);
               slot->tts_tupleDescriptor = NULL;
            }
            if (!TTS_FIXED(slot))
            {
               if (slot->tts_values)
                   pfree(slot->tts_values);
               if (slot->tts_isnull)
                   pfree(slot->tts_isnull);
            }
            pfree(slot);
        }
        else
        {
            ereport(ERROR,
                    errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("AQPSWRScan failed to find the redundant tuple "
                           "slots made by ExecInitCustomScan()"));
        }
    }
    psss->isss.css.ss.ps.ps_ResultTupleSlot = NULL;
    psss->isss.css.ss.ss_ScanTupleSlot = NULL;


    /*
     * We have cleaned up the mess from ExecInitCustomScan(). We can now
     * initialize the per-subplan states. Note that we initialize swr and
     * swrindexonly subplans a bit differently.
     *
     * All swr scans produce the same heap tuples and should have matching
     * tuple descriptors, so we can share all scan slots, qual expr, prjection
     * info across them.
     *
     * The swrindexonly scans produces different index tuples and won't have
     * matching tuple descriptors, so we must initialize separate states for
     * them.
     */
    psss->nsubplans = list_length(cscan->custom_private);
    psss->selected_subplan_idx = 0;
    if (psss->nsubplans == 0)
    {
        /* sanity check */
        elog(ERROR, "no valid index sample path found");
    }
    
    /* 
     * Save the original target list and restore it after we finish
     * initializing for better EXPLAIN output.
     */
    psss->orig_tlist = cscan->scan.plan.targetlist;
    psss->swr_scanslot = NULL;

    while (psss->selected_subplan_idx < psss->nsubplans)
    {
        Relation indexrel;
        ListCell *lc0 = list_nth_cell(cscan->custom_private, 0);
        ListCell *lci = list_nth_cell(cscan->custom_private,
                                      psss->selected_subplan_idx);
        AQPProgressiveSampleScanPrivate *pssp;
        AQPProgressiveSampleScanSubplanState *subplan_state;
        IndexScanDesc scan;

        swap_ptr(lfirst(lc0), lfirst(lci));
        
        pssp = (AQPProgressiveSampleScanPrivate *) lfirst(lc0);
        Assert(AQPISSIsPSWRSubPlan(pssp));
        
        /* restore the plan info */
        cscan->scan.plan.targetlist = pssp->qptlist;
        cscan->scan.plan.qual = pssp->qpqual;
        cscan->custom_scan_tlist = pssp->indextlist;
        if (AQPISSIsIndexOnly(pssp))
        {
            aqp_swr_index_only_scan_begin((CustomScanState *) psss,
                                          estate,
                                          eflags);
        }
        else
        {
            aqp_swr_scan_begin((CustomScanState *) psss,
                               estate,
                               eflags);
        }

        /* save the plan states for the current subplan */
        subplan_state = &psss->subplan_states[psss->selected_subplan_idx];
        Assert(psss->isss.css.ss.ps.scanopsset);
        Assert(psss->isss.css.ss.ps.scanopsfixed);
        subplan_state->scanslot = psss->isss.css.ss.ss_ScanTupleSlot;
        subplan_state->scan_descriptor = psss->isss.css.ss.ps.scandesc;
        subplan_state->scanops = psss->isss.css.ss.ps.scanops;
        /* Result tuple descriptor and result slots are shared. */
        subplan_state->projInfo = psss->isss.css.ss.ps.ps_ProjInfo;
        subplan_state->resultopsset = psss->isss.css.ss.ps.resultopsset;
        subplan_state->resultopsfixed = psss->isss.css.ss.ps.resultopsfixed;
        subplan_state->resultops = psss->isss.css.ss.ps.resultops;
        subplan_state->qual = psss->isss.css.ss.ps.qual;
        subplan_state->indexqual = psss->isss.indexqual;
        subplan_state->indexqualorig = psss->isss.indexqualorig;
        
        subplan_state->allnullslot = psss->isss.allnullslot;
        subplan_state->indexattr = pssp->indexattr;

        /* create rbtree for each subplan */
        indexrel = index_open(pssp->issp.indexid, NoLock);
        subplan_state->indexkeytype = indexrel->rd_opcintype[0];
        subplan_state->indexkeyopfamily = indexrel->rd_opfamily[0];

        subplan_state->typlen = pssp->typlen;
        subplan_state->typbyval = pssp->typbyval;
        
        subplan_state->compareFn = pssp->compareFn;
        subplan_state->supportCollation = indexrel->rd_indcollation[0];
        index_close(indexrel, NoLock);

        subplan_state->dpinfotree = rbt_create(sizeof(DPInfoTreeNode),
                                               aqp_irbt_cmp,
                                               aqp_irbt_combine,
                                               aqp_irbt_alloc,
                                               aqp_irbt_free,
                                               (void *) subplan_state);
    

        subplan_state->lower_paramid = pssp->lower_param->paramid;
        subplan_state->upper_paramid = pssp->upper_param->paramid;
        subplan_state->lower_opfuncid = pssp->lower_opfuncid;
        subplan_state->upper_opfuncid = pssp->upper_opfuncid;

        subplan_state->runtime_l_expr = 
            ExecInitExpr((Expr *)pssp->lower_param, (PlanState *)subplan_state);

        subplan_state->runtime_u_expr = 
            ExecInitExpr((Expr *)pssp->upper_param, (PlanState *)subplan_state);

        if (psss->isss.runtime_context == NULL)
            subplan_state->runtime_context = CreateExprContext(estate);
        else
            subplan_state->runtime_context = psss->isss.runtime_context;

        if (psss->isss.running_sample_size == NULL)
            psss->isss.running_sample_size =
                &psss->isss.css.ss.ps.ps_ExprContext->
                ecxt_param_exec_vals[pssp->running_sample_size_param->paramid];

        if (psss->isss.running_sample_budget == NULL)
            psss->isss.running_sample_budget =
                &psss->isss.css.ss.ps.ps_ExprContext->
                ecxt_param_exec_vals[pssp->running_sample_budget_param->paramid];

        if (psss->isss.running_state_id == NULL)
            psss->isss.running_state_id =
                &psss->isss.css.ss.ps.ps_ExprContext->
                ecxt_param_exec_vals[pssp->running_state_id_param->paramid];

        /*
         * The rest of the subplan states are only initialized for non-EXPLAIN
         * cases.
         */
        if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
        {
            subplan_state->indexrel = psss->isss.indexrel;
            subplan_state->scan_keys = psss->isss.scan_keys;
            subplan_state->num_scan_keys = psss->isss.num_scan_keys;
            subplan_state->runtime_keys = psss->isss.runtime_keys;
            subplan_state->num_runtime_keys = psss->isss.num_runtime_keys;
            subplan_state->scandesc = psss->isss.scandesc;
        }

        /* save the upper and lower of scandesc range */
        scan = subplan_state->scandesc;
        /* TODO:IF the range have the overlapping */
        //index_samplenext_tid(scan, 1);
        for (int i = 0; i < scan->numberOfKeys; i++)
        {
            //ABTScanOpaque so = (ABTScanOpaque) scan->opaque;

            if (scan->keyData[i].sk_strategy == BTLessStrategyNumber
                    || scan->keyData[i].sk_strategy == BTLessEqualStrategyNumber)
            {
                subplan_state->upper_scankey = (ScanKey) palloc(1 * sizeof(ScanKeyData));
                subplan_state->upper_scankey->sk_argument = scan->keyData[i].sk_argument;
                subplan_state->upper_scankey->sk_strategy = scan->keyData[i].sk_strategy;
                subplan_state->upper_scankey->sk_func = scan->keyData[i].sk_func;
            }
            if (scan->keyData[i].sk_strategy == BTGreaterStrategyNumber
                    || scan->keyData[i].sk_strategy == BTGreaterEqualStrategyNumber)
            {
                subplan_state->lower_scankey = (ScanKey) palloc(1 * sizeof(ScanKeyData));
                subplan_state->lower_scankey->sk_argument = scan->keyData[i].sk_argument;
                subplan_state->lower_scankey->sk_strategy = scan->keyData[i].sk_strategy;
                subplan_state->lower_scankey->sk_func = scan->keyData[i].sk_func;
            }
        }

        /* 
         * Save a copy of the first swr subplan's scan slot, projection info
         * and result slot ops. These are shared across all swr subplans.
         */
        if (!AQPISSIsIndexOnly(pssp) && psss->swr_scanslot == NULL)
        {
            psss->swr_scanslot = subplan_state->scanslot;
            psss->swr_scan_descriptor = subplan_state->scan_descriptor;
            psss->swr_scanops = subplan_state->scanops;
            psss->swr_projInfo = subplan_state->projInfo;
            psss->swr_resultopsset = subplan_state->resultopsset;
            psss->swr_resultopsfixed = subplan_state->resultopsfixed;
            psss->swr_resultops = subplan_state->resultops;
            psss->swr_allnullslot = subplan_state->allnullslot;
        }
        
        /* clean ups */
        psss->isss.css.ss.ss_ScanTupleSlot = NULL;
        psss->isss.css.ss.ps.scandesc = NULL;
        psss->isss.css.ss.ps.scanops = NULL;
        psss->isss.css.ss.ps.scanopsset = false;
        psss->isss.css.ss.ps.ps_ProjInfo = NULL;
        psss->isss.css.ss.ps.resultopsset = false;
        psss->isss.css.ss.ps.resultopsfixed = false;
        psss->isss.css.ss.ps.resultops = NULL;
        psss->isss.css.ss.ps.qual = NULL;
        psss->isss.indexqual = NULL;
        psss->isss.indexqualorig = NULL;
        psss->isss.indexrel = NULL;
        psss->isss.scan_keys = NULL;
        psss->isss.num_scan_keys = 0;
        psss->isss.runtime_keys = NULL;
        psss->isss.num_runtime_keys = 0;
        psss->isss.scandesc = NULL;

        psss->isss.p2matter = false;
        psss->isss.is_partition = false;
        psss->isss.allnullslot = NULL;

        swap_ptr(lfirst(lc0), lfirst(lci));
        ++psss->selected_subplan_idx;        
    }

    psss->selected_subplan_idx = -1;
    cscan->scan.plan.targetlist = psss->orig_tlist;
    cscan->scan.plan.qual = NIL;
    cscan->custom_scan_tlist = NIL;

    if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
    {
        /* 
         * Explain only, we need to replace the Vars with invalid
         * references into the heap tuple with sample_prob() in the
         * query plan right now, before it is sent for printing.
         */
        if (aqp_is_in_explain_verbose)
            aqp_rewrite_sample_prob_dummy_var_for_explain((PlanState *) psss);
        return;
    }

    /* 
     * Save us a few cycles of calling from ExecCustomScan. Must be done after
     * we begin each subplans as they also set ExecProcNode.
     */
    psss->isss.css.ss.ps.ExecProcNode = aqp_progressive_swr_scan_exec;

    /* 
     * Initialize the sample size budget. All sub plans have the same set of
     * repeatable_expr, sample_size and sample_size_expr so we can use any
     * one's.
     */
    aqp_sample_scan_init(&psss->isss.sss,
                         (PlanState *) psss,
                         issp0->repeatable_expr,
                         issp0->sample_size,
                         issp0->sample_size_expr);
    aqp_sample_scan_start(&psss->isss.sss,
                          psss->isss.css.ss.ps.ps_ExprContext);
    psss->sample_budget = psss->isss.sss.RemSampleSize;
    psss->rem_sample_budget = psss->sample_budget;
    psss->isss.sss.RemSampleSize = 0;
}

static TupleTableSlot*
aqp_progressive_swr_scan_exec(PlanState *node)
{
    AQPProgressiveSampleScanState *psss =
        (AQPProgressiveSampleScanState *) node;
    CustomScan *cscan = castNode(CustomScan, node->plan);
    AQPProgressiveSampleScanPrivate *pssp;
    AQPProgressiveSampleScanSubplanState *subplan_state;
    ListCell *lc0 = list_nth_cell(cscan->custom_private, 0);
    ListCell *lci;
    int previously_selected_subplan_idx = psss->selected_subplan_idx;
    uint64 next_batch_sample_size;
    uint64 phase_0_sample_size;

    /* We only end up here when we are ready to switch plans. */
    Assert(psss->isss.sss.RemSampleSize == 0); 
    
    /* 
     * Fast path for running out of sampling budget.
     */ 
    if (psss->rem_sample_budget == 0)
    {
        psss->selected_subplan_idx = -1;
        if (previously_selected_subplan_idx >= 0)
        {
            /* 
             * Recover the custom_private to its original state.
             */
            lci = list_nth_cell(cscan->custom_private,
                                previously_selected_subplan_idx);
            swap_ptr(lfirst(lc0), lfirst(lci));
        }

        /* The rest of cleanups will be done in end/rescan. */
        return NULL;
    }

    /* for debugging */
    if (aqp_pswr_chosen_plan_index >= 0)
    {
        psss->selected_subplan_idx = aqp_pswr_chosen_plan_index;
    }
    else
    {
        /* TODO plan selection and sample size allocation logic goes
         * here */
        psss->selected_subplan_idx = 0;
    }

    
    /* Sets the plan states to those of the selected subplan. */
    if (previously_selected_subplan_idx != psss->selected_subplan_idx)
    {
        
        if (previously_selected_subplan_idx >= 0)
        {
            lci = list_nth_cell(cscan->custom_private,
                                previously_selected_subplan_idx);
            swap_ptr(lfirst(lc0), lfirst(lci));
        }
            
        lci = list_nth_cell(cscan->custom_private, psss->selected_subplan_idx);
        swap_ptr(lfirst(lc0), lfirst(lci));
    }

    pssp = (AQPProgressiveSampleScanPrivate *) lfirst(lc0);
    subplan_state = &psss->subplan_states[psss->selected_subplan_idx];
    if (previously_selected_subplan_idx != psss->selected_subplan_idx)
    {
        cscan->scan.plan.targetlist = pssp->qptlist;
        cscan->scan.plan.qual = pssp->qpqual;
        cscan->custom_scan_tlist = pssp->indextlist;

        psss->isss.css.ss.ss_ScanTupleSlot = subplan_state->scanslot;
        psss->isss.css.ss.ps.scandesc = subplan_state->scan_descriptor;
        psss->isss.css.ss.ps.scanops = subplan_state->scanops;
        psss->isss.css.ss.ps.scanopsset = true;
        psss->isss.css.ss.ps.scanopsfixed = true;

        psss->isss.css.ss.ps.ps_ProjInfo = subplan_state->projInfo;
        psss->isss.css.ss.ps.resultopsset = subplan_state->resultopsset;
        psss->isss.css.ss.ps.resultopsfixed = subplan_state->resultopsfixed;
        psss->isss.css.ss.ps.resultops = subplan_state->resultops;
        /* result slot and result tuple descriptor are shared */

        psss->isss.css.ss.ps.qual = subplan_state->qual;
        psss->isss.indexqual = subplan_state->indexqual;
        psss->isss.indexqualorig = subplan_state->indexqualorig;
        psss->isss.indexrel = subplan_state->indexrel;
        psss->isss.scan_keys = subplan_state->scan_keys;
        psss->isss.num_scan_keys = subplan_state->num_scan_keys;
        psss->isss.runtime_keys = subplan_state->runtime_keys;
        psss->isss.num_runtime_keys = subplan_state->num_runtime_keys;
        psss->isss.scandesc = subplan_state->scandesc;

        psss->isss.allnullslot = subplan_state->allnullslot;
    }

    if (AQPISSIsIndexOnly(pssp))
    {
        psss->is_index_only_scan = true;
    }
    else
    {
        psss->is_index_only_scan = false;
    }

    /* TODO set new runtime keys */

    /* 
     * We probably have to mark the runtime keys as not ready since this might
     * be the first time the subplan is run after begin is called on the pswr
     * state.
     * 
     * TODO how about initializing the runtime keys here?
     */
    psss->isss.runtime_keys_ready = false;
    
    // running sample size to 0
    psss->isss.running_sample_size->value = (Datum) 0;
    psss->isss.running_sample_size->isnull = false;
    
    /* 
     * TODO compute next batch's sample size. Replace the following
     * with the computed sample size for the batch.
     */
    /*next_batch_sample_size = psss->rem_sample_budget;*/

    if (aqp_initial_phase_sample_size >= 0)
        phase_0_sample_size = aqp_initial_phase_sample_size;
    else
        phase_0_sample_size = psss->sample_budget / 2;

    if(psss->rem_sample_budget > psss->sample_budget - phase_0_sample_size)
    //if (psss->rem_sample_budget > (psss->sample_budget /2 + 1))
    {
        subplan_state->inputcnt = 0;

        psss->is_collect_finish = false;

        psss->isss.is_partition = false;

        next_batch_sample_size = phase_0_sample_size;
        //next_batch_sample_size = psss->sample_budget/2;
        elog(INFO, "%ld", next_batch_sample_size);

        psss->isss.running_sample_budget->value = next_batch_sample_size;
        psss->isss.running_sample_budget->isnull = false;

        psss->isss.running_state_id->value = next_batch_sample_size;
        psss->isss.running_state_id->isnull = false;

        psss->rem_sample_budget -= next_batch_sample_size;
        aqp_sample_scan_reset_rem_samplesize(&psss->isss.sss,
                                            next_batch_sample_size);
    
        /* 
        * The rest of sampling call will be directly invoked on the subplans exec
        * functions, until it runs out of the assigned sample size.
        *
        * TODO: replace this with your own exec function for collecting
        * stats.
        */
        psss->isss.css.ss.ps.ExecProcNode = aqp_progressive_swr_scan_statis_collect_exec;
    }
    else if (subplan_state->inputcnt == 0)
    {

        psss->is_collect_finish = false;

        psss->isss.is_partition = false;

        next_batch_sample_size = psss->rem_sample_budget;
        //next_batch_sample_size = psss->sample_budget/2;
        elog(INFO, "%ld", next_batch_sample_size);

        psss->isss.running_sample_budget->value = next_batch_sample_size;
        psss->isss.running_sample_budget->isnull = false;

        psss->isss.running_state_id->value = next_batch_sample_size;
        psss->isss.running_state_id->isnull = false;

        psss->rem_sample_budget -= next_batch_sample_size;
        aqp_sample_scan_reset_rem_samplesize(&psss->isss.sss,
                                            next_batch_sample_size);
        psss->isss.css.ss.ps.ExecProcNode =
            AQPISSIsIndexOnly(pssp) ? aqp_swr_index_only_scan_exec
                                    : aqp_swr_scan_exec;
    }
    else
    {
        Datum lower_key_str;
        Datum upper_key_str;

        /* If finish collecting statis, do dp */

        if (psss->is_collect_finish)
        {
            aqp_statis_dpinfo(psss->isss.indexrel, 
                              subplan_state, 
                              psss->rem_sample_budget);
            subplan_state->out_index = 0;
            psss->is_collect_finish = false;

            if (subplan_state->outputcnt == 1)
            {
                next_batch_sample_size = psss->rem_sample_budget;

                psss->isss.running_sample_budget->value = next_batch_sample_size;
                psss->isss.running_sample_budget->isnull = false;

                psss->isss.running_state_id->value = next_batch_sample_size;
                psss->isss.running_state_id->isnull = false;

                psss->rem_sample_budget -= next_batch_sample_size;
                aqp_sample_scan_reset_rem_samplesize(&psss->isss.sss,
                                            next_batch_sample_size);

                psss->isss.css.ss.ps.ExecProcNode =
                    AQPISSIsIndexOnly(pssp) ? aqp_swr_index_only_scan_exec
                                            : aqp_swr_scan_exec;

                return psss->isss.css.ss.ps.ExecProcNode(node);
            }

            subplan_state->scandesc->numberOfKeys = 2;
    
            psss->isss.running_sample_budget->value = psss->rem_sample_budget;
            psss->isss.running_sample_budget->isnull = false;
        }

        next_batch_sample_size = subplan_state->dpoutput[subplan_state->out_index].ni;
        if (next_batch_sample_size > psss->rem_sample_budget)
        {
            next_batch_sample_size = psss->rem_sample_budget;
            subplan_state->dpoutput[subplan_state->out_index].percentage
                = subplan_state->dpoutput[subplan_state->out_index].percentage / 
                subplan_state->dpoutput[subplan_state->out_index].ni * next_batch_sample_size;
        }

        psss->rem_sample_budget -= next_batch_sample_size;
      
        psss->isss.is_partition = true;
        psss->isss.percentage = subplan_state->dpoutput[subplan_state->out_index].percentage;
        
        psss->isss.running_state_id->value = subplan_state->dpoutput[subplan_state->out_index].ni;
        psss->isss.running_state_id->isnull = false;

        aqp_sample_scan_reset_rem_samplesize(&psss->isss.sss,
                                            next_batch_sample_size);

        ResetExprContext(subplan_state->runtime_context);

        if (subplan_state->out_index == 0)
        {
            /* upper boundary */
            //subplan_state->scan_keys[1] = *subplan_state->upper_scankey;
            aqp_pswr_partition_runtimekey(psss->isss.indexrel,
                                subplan_state,
                                &subplan_state->scan_keys,
                                &subplan_state->num_scan_keys,
                                &subplan_state->runtime_keys,
                                &subplan_state->num_runtime_keys);
            if (subplan_state->upper_scankey == NULL)
            {          
                //subplan_state->scan_keys[1] = subplan_state->upper_key;

                //subplan_state->runtime_context->
                //    ecxt_param_exec_vals[subplan_state->upper_paramid].value = NULL;
                subplan_state->runtime_context->
                    ecxt_param_exec_vals[subplan_state->upper_paramid].isnull = true;

            }
            else
            {
                subplan_state->scan_keys[1].sk_strategy = 
                    subplan_state->upper_scankey->sk_strategy;
                subplan_state->scan_keys[1].sk_func = 
                    subplan_state->upper_scankey->sk_func;

                subplan_state->runtime_context->
                    ecxt_param_exec_vals[subplan_state->upper_paramid].value = 
                    subplan_state->upper_scankey->sk_argument;
                subplan_state->runtime_context->
                    ecxt_param_exec_vals[subplan_state->upper_paramid].isnull = false;
            }

            subplan_state->runtime_context->
                ecxt_param_exec_vals[subplan_state->lower_paramid].value = 
                subplan_state->dpoutput[subplan_state->out_index].lower_key;
            subplan_state->runtime_context->
                ecxt_param_exec_vals[subplan_state->lower_paramid].isnull = false;
            
            lower_key_str = DirectFunctionCall1(date_out, subplan_state->runtime_context->ecxt_param_exec_vals[subplan_state->lower_paramid].value);
            upper_key_str = DirectFunctionCall1(date_out, subplan_state->runtime_context->ecxt_param_exec_vals[subplan_state->upper_paramid].value);

            elog(INFO, "lower_key: %s, upper_key %s", DatumGetCString(lower_key_str), DatumGetCString(upper_key_str));
        }
        else if (subplan_state->out_index == subplan_state->outputcnt - 1)
        {
            /* lower boundary */
            //subplan_state->scan_keys[0] = *subplan_state->lower_scankey;

            aqp_pswr_partition_runtimekey(psss->isss.indexrel,
                                subplan_state,
                                &subplan_state->scan_keys,
                                &subplan_state->num_scan_keys,
                                &subplan_state->runtime_keys,
                                &subplan_state->num_runtime_keys);
            
            if (subplan_state->lower_scankey == NULL)
            {          
                //subplan_state->scan_keys[0] = subplan_state->lower_key;

                //subplan_state->runtime_context->
                //    ecxt_param_exec_vals[subplan_state->lower_paramid].value = NULL;
                subplan_state->runtime_context->
                    ecxt_param_exec_vals[subplan_state->lower_paramid].isnull = true;
            }
            else
            {
                subplan_state->scan_keys[0].sk_strategy = 
                    subplan_state->lower_scankey->sk_strategy;
                subplan_state->scan_keys[0].sk_func = 
                    subplan_state->lower_scankey->sk_func;

                subplan_state->runtime_context->
                    ecxt_param_exec_vals[subplan_state->lower_paramid].value = 
                    subplan_state->lower_scankey->sk_argument;
                subplan_state->runtime_context->
                    ecxt_param_exec_vals[subplan_state->lower_paramid].isnull = false;
            }

            subplan_state->runtime_context->
                ecxt_param_exec_vals[subplan_state->upper_paramid].value = 
                subplan_state->dpoutput[subplan_state->out_index - 1].lower_key;
            subplan_state->runtime_context->
                ecxt_param_exec_vals[subplan_state->upper_paramid].isnull = false;

            lower_key_str = DirectFunctionCall1(date_out, subplan_state->runtime_context->ecxt_param_exec_vals[subplan_state->lower_paramid].value);
            upper_key_str = DirectFunctionCall1(date_out, subplan_state->runtime_context->ecxt_param_exec_vals[subplan_state->upper_paramid].value);

            elog(INFO, "lower_key: %s, upper_key %s", DatumGetCString(lower_key_str), DatumGetCString(upper_key_str));
        }
        else
        {
            aqp_pswr_partition_runtimekey(psss->isss.indexrel,
                                          subplan_state,
                                          &subplan_state->scan_keys,
                                          &subplan_state->num_scan_keys,
                                          &subplan_state->runtime_keys,
                                          &subplan_state->num_runtime_keys);
            
            subplan_state->runtime_context->
                ecxt_param_exec_vals[subplan_state->lower_paramid].value = 
                subplan_state->dpoutput[subplan_state->out_index].lower_key;
            subplan_state->runtime_context->
                ecxt_param_exec_vals[subplan_state->lower_paramid].isnull = false;

            subplan_state->runtime_context->
                ecxt_param_exec_vals[subplan_state->upper_paramid].value = 
                subplan_state->dpoutput[subplan_state->out_index - 1].lower_key;
            subplan_state->runtime_context->
                ecxt_param_exec_vals[subplan_state->upper_paramid].isnull = false;

            lower_key_str = DirectFunctionCall1(date_out, subplan_state->runtime_context->ecxt_param_exec_vals[subplan_state->lower_paramid].value);
            upper_key_str = DirectFunctionCall1(date_out, subplan_state->runtime_context->ecxt_param_exec_vals[subplan_state->upper_paramid].value);

            elog(INFO, "lower_key: %s, upper_key %s", DatumGetCString(lower_key_str), DatumGetCString(upper_key_str));
        }

        psss->isss.runtime_context = subplan_state->runtime_context;

        psss->isss.scan_keys = subplan_state->scan_keys;
        psss->isss.num_scan_keys = subplan_state->num_scan_keys;
        psss->isss.runtime_keys = subplan_state->runtime_keys;
        psss->isss.num_runtime_keys = subplan_state->num_runtime_keys;
        psss->isss.scandesc = subplan_state->scandesc;

        subplan_state->out_index++;
        
        psss->isss.css.ss.ps.ExecProcNode =
            AQPISSIsIndexOnly(pssp) ? aqp_swr_index_only_scan_exec
                                    : aqp_swr_scan_exec;
    }

    return psss->isss.css.ss.ps.ExecProcNode(node);
}

static void
aqp_progressive_swr_scan_end(CustomScanState *node)
{
    AQPProgressiveSampleScanState *psss =
        (AQPProgressiveSampleScanState *) node;
    int i;
    
    /* We should have reset the subplan selection at this point */
    Assert(psss->selected_subplan_idx == -1);

    if (psss->isss.vmbuffer != InvalidBuffer)
    {
        ReleaseBuffer(psss->isss.vmbuffer);
        psss->isss.vmbuffer = InvalidBuffer;
    }
    
    if (psss->swr_scanslot != NULL)
        ExecClearTuple(psss->swr_scanslot);
    
    for (i = 0; i < psss->nsubplans; ++i)
    {
        AQPProgressiveSampleScanSubplanState *subplan_state =
            &psss->subplan_states[i];
        
        /*if (subplan_state->dpoutput)
            pfree(subplan_state->dpoutput);
        if (subplan_state->dpinfotree)
        {
	        for (i = 0; i < subplan_state->inputcnt; i++)
            {
                DPInfoTreeNode *rbtnode;  

                rbtnode = (DPInfoTreeNode *) rbt_leftmost(subplan_state->dpinfotree);
                rbt_delete(subplan_state->dpinfotree, (RBTNode *) rbtnode);
            }

        }*/
        if (subplan_state->scanslot != psss->swr_scanslot)
            ExecClearTuple(subplan_state->scanslot);
        if (subplan_state->scandesc)
            index_endscan(subplan_state->scandesc);
        if (subplan_state->indexrel)
            index_close(subplan_state->indexrel, NoLock);
    }

    /* 
     * NOTE we have to set result tuple slot to something with
     * a valid ttsops, or ExecEndCustomScan() will blindly invoke
     * ExecClearTuple on a NULL pointer!
     *
     * (This means there is one slot that could be cleared for three times in
     * the worst case).
     *
     */
    if (psss->isss.css.ss.ps.ps_ResultTupleSlot == NULL)
    {
        psss->isss.css.ss.ps.ps_ResultTupleSlot =
            psss->subplan_states[0].scanslot;
    }
    if (psss->isss.css.ss.ss_ScanTupleSlot == NULL)
    {
        psss->isss.css.ss.ss_ScanTupleSlot = psss->subplan_states[0].scanslot;
    }
}

static void
aqp_progressive_swr_scan_rescan(CustomScanState *node)
{
    AQPProgressiveSampleScanState *psss =
        (AQPProgressiveSampleScanState *) node;
    
    /* Runtime keys will be initialized when each subplan is executed. */

    aqp_sample_scan_start(&psss->isss.sss, node->ss.ps.ps_ExprContext);
}

static void
aqp_progressive_swr_scan_explain(CustomScanState *node,
                                 List *ancestors,
                                 ExplainState *es)
{
    CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
    AQPIndexSampleScanPrivate *issp0 =
        (AQPIndexSampleScanPrivate *) linitial(cscan->custom_private);
    ListCell *lc;
    
    ExplainPropertyInteger("Number of subplans", NULL,
        list_length(cscan->custom_private), es);

    foreach (lc, cscan->custom_private)
    {
        AQPProgressiveSampleScanPrivate *pssp =
            (AQPProgressiveSampleScanPrivate *) lfirst(lc);
        const char *index_name;
        
        ExplainPropertyInteger("Subplan Id ", NULL,
            list_cell_number(cscan->custom_private, lc), es);
        ++es->indent;

        ExplainPropertyText("Subplan Type", pssp->issp.extnode.extnodename, es);

        index_name = get_rel_name(pssp->issp.indexid);
        if (!index_name)
            elog(ERROR, "cache lookup failed fo index %u",
                 pssp->issp.indexid);
        ExplainPropertyText("Index Name", index_name, es);
 
        /* 
         * Must set the indextlist here for explain to resolve variable names.
         */
        if (AQPISSIsIndexOnly(pssp))
        {
            cscan->custom_scan_tlist = pssp->indextlist;
            aqp_pgport_show_scan_qual(pssp->issp.indexqual, "Index Cond",
                                      &node->ss.ps, ancestors, es);
            aqp_pgport_show_scan_qual(pssp->qpqual, "Remaining Filter",
                                      &node->ss.ps, ancestors, es);
            cscan->custom_scan_tlist = NIL;
        }
        else
        {
            aqp_pgport_show_scan_qual(pssp->issp.indexqualorig, "Index Cond",
                                      &node->ss.ps, ancestors, es);
            aqp_pgport_show_scan_qual(pssp->qpqual, "Remaining Filter",
                                      &node->ss.ps, ancestors, es);

        }
        --es->indent;
    }
    
    aqp_show_index_sample_scan_details(issp0->sample_size,
                                       issp0->sample_size_expr,
                                       issp0->repeatable_expr,
                                       &node->ss.ps,
                                       ancestors,
                                       es);
}

static void 
aqp_pswr_partition_runtimekey(Relation index, 
                              AQPProgressiveSampleScanSubplanState *subplan_state,
                              ScanKey *scanKeys, int *numScanKeys,
					          IndexRuntimeKeyInfo **runtimeKeys, int *numRuntimeKeys)
{
    ScanKey		 scan_keys;
	IndexRuntimeKeyInfo *runtime_keys;
	int			 n_scan_keys;
	int			 n_runtime_keys;
    int			 op_strategy;	/* operator's strategy number */
    ScanKey		 this_scan_key;  
    RegProcedure lower_opfuncid; //= subplan_state->lower_opfuncid;	
                 /* operator proc id used in scan */
    RegProcedure upper_opfuncid; //= subplan_state->upper_opfuncid;
    Oid			 op_type = subplan_state->indexkeytype;
    AttrNumber	 varattno = 1;	/* att number used in scan */
    int			 flags = 0;
    Datum		 scanvalue;
    Oid          opno;

	/* Allocate array for ScanKey structs: one for lower_key, one for upper_key */
	/*scan_keys = (ScanKey) palloc(n_scan_keys * sizeof(ScanKeyData));*/

    scan_keys = *scanKeys;
    n_scan_keys = 0;
    runtime_keys = *runtimeKeys;
    n_runtime_keys = 0;


    if (*numScanKeys == 0)
        scan_keys = (ScanKey) palloc(2 * sizeof(ScanKeyData));
    else
        scan_keys = (ScanKey) repalloc(scan_keys, 2 * sizeof(ScanKeyData));


    if (*numRuntimeKeys == 0)
        runtime_keys = 
            (IndexRuntimeKeyInfo *) palloc(2 * sizeof(IndexRuntimeKeyInfo));
    else
        runtime_keys = (IndexRuntimeKeyInfo *)
            repalloc(runtime_keys, 2 * sizeof(IndexRuntimeKeyInfo));
						
    /*
	 * Convert the opclause of lower_key into a single scan key
	 */	
    this_scan_key = &scan_keys[n_scan_keys];
    op_strategy = BTGreaterEqualStrategyNumber; /*>=*/
    n_scan_keys++;

    /*get_op_opfamily_properties(opno, opfamily, isorderby,
                               &op_strategy,
                               &op_lefttype,
                               &op_righttype);*/

    runtime_keys[n_runtime_keys].scan_key = this_scan_key;
    runtime_keys[n_runtime_keys].key_expr = subplan_state->runtime_l_expr;
    runtime_keys[n_runtime_keys].key_toastable =
        TypeIsToastable(op_type);
    n_runtime_keys++;
    scanvalue = (Datum) 0;
 
    opno = get_opfamily_member(index->rd_opfamily[0],
                               op_type,
                               op_type,
                               op_strategy);
    lower_opfuncid = get_opcode(opno);
    
    /*
     * initialize the scan key's fields appropriately
     */
    ScanKeyEntryInitialize(this_scan_key,
                           flags,
                           varattno,	/* attribute number to scan */
                           op_strategy, /* op's strategy */
                           op_type,	/* strategy subtype */
                           index->rd_indcollation[0],	/* collation */
                           lower_opfuncid,	/* reg proc to use */
                           scanvalue);	/* constant */

	/*
	 * Convert the opclause of upper_key into a single scan key
	 */	
    this_scan_key = &scan_keys[n_scan_keys];
    op_strategy = BTLessStrategyNumber; /*<=*/
    n_scan_keys++;

    /*get_op_opfamily_properties(opno, opfamily, isorderby,
                               &op_strategy,
                               &op_lefttype,
                               &op_righttype);*/

    runtime_keys[n_runtime_keys].scan_key = this_scan_key;
    runtime_keys[n_runtime_keys].key_expr = subplan_state->runtime_u_expr;
    runtime_keys[n_runtime_keys].key_toastable =
        TypeIsToastable(op_type);
    n_runtime_keys++;
    scanvalue = (Datum) 0;

    opno = get_opfamily_member(index->rd_opfamily[0],
                               op_type,
                               op_type,
                               op_strategy);
    upper_opfuncid = get_opcode(opno);
    /*
     * initialize the scan key's fields appropriately
     */
    ScanKeyEntryInitialize(this_scan_key,
                           flags,
                           varattno,	/* attribute number to scan */
                           op_strategy, /* op's strategy */
                           op_type,	/* strategy subtype */
                           index->rd_indcollation[0],	/* collation */
                           upper_opfuncid,	/* reg proc to use */
                           scanvalue);	/* constant */

	/*
	 * Return info to our caller.
	 */
    *scanKeys = scan_keys;
	*numScanKeys = n_scan_keys;
	*runtimeKeys = runtime_keys;
	*numRuntimeKeys = n_runtime_keys;

}

static TupleTableSlot* 
aqp_progressive_swr_scan_statis_collect_exec(PlanState *node)
{
    AQPProgressiveSampleScanState *psss =
        (AQPProgressiveSampleScanState *) node;

    AQPProgressiveSampleScanSubplanState *subplan_state;
    TupleTableSlot  *slot;
    TupleTableSlot  *scanslot;
    /*ItemPointerData heaptid;*/
    RBTree          *dpinfotree;	
    DPInfoTreeNode  eatmp;
	DPInfoTreeNode  *ea;
    int             indexattr;
    Datum           key;
    bool            isnull;
    bool            isNew;

    scanslot = psss->isss.css.ss.ss_ScanTupleSlot;
    /*ItemPointerCopy(&psss->isss.scandesc->xs_heaptid, &heaptid);*/
    subplan_state = &psss->subplan_states[psss->selected_subplan_idx];
    indexattr = subplan_state->indexattr;
    dpinfotree = subplan_state->dpinfotree;

    if (aqp_sample_scan_no_more_samples(&psss->isss.sss)) 
    {
        psss->is_collect_finish = true;
        return aqp_progressive_swr_scan_exec(node);
    }

    if (psss->is_index_only_scan)
        slot = aqp_swr_index_only_scan_exec(node);
    else 
        slot = aqp_swr_scan_exec(node);

    if (slot->tts_isnull[slot->tts_nvalid-1])
    {
        /* prob column is NULL, the slot is rejected */
        /*if (&heaptid != &psss->isss.scandesc->xs_heaptid)*/
        if (psss->isss.p2matter)
        {
            /* rejected because of p2 */
            key = slot_getattr(scanslot, indexattr, &isnull);

            if (!isnull)
            {
                eatmp.key = key;
                ea = (DPInfoTreeNode *) rbt_insert(dpinfotree, (RBTNode *) &eatmp, &isNew);
                if (isNew)
                {
                    ea->key = datumCopy(key, subplan_state->typbyval, subplan_state->typlen);
                    ea->count_p1 = 1;
                    ea->count_p1_p2 = 0;
                    subplan_state->inputcnt++;
                }
                else
                {
                    /* Combiner did count_p1++ */
                }
            }
        }
    } 
    else
    {
        /* prob column is not NULL, the slot is accepted */
        key = slot_getattr(scanslot, indexattr, &isnull);

        if (!isnull)
        {
            eatmp.key = key;
            ea = (DPInfoTreeNode *) rbt_insert(dpinfotree, (RBTNode *) &eatmp, &isNew);
            if (isNew)
            {
                ea->key = datumCopy(key, subplan_state->typbyval, subplan_state->typlen);
                ea->count_p1 = 1;
                ea->count_p1_p2 = 0;
                subplan_state->inputcnt++;
            }
            else
            {
                /* Combiner did count_p1++ */
            }
            ea->count_p1_p2++;
        }
        else
        {
            /* TODO: Do we allow the situation when key is NULL? */
        }

    }

    psss->isss.p2matter = false;

    return slot;
}

/* DP info calculation */

static void
aqp_statis_dpinfo(Relation index, AQPProgressiveSampleScanSubplanState *subplan_state, uint64 sample_budget)
{
	RBTreeIterator iter;
	DPInfoTreeNode *node;
    RBTree         *tree;
    DPInfo         *info;
    DPInfo         *info_interval;
	int			   count = 0;

    info = palloc0(subplan_state->inputcnt * sizeof(DPInfo));
    info_interval = palloc0(100 * sizeof(DPInfo));

    tree = subplan_state->dpinfotree;

	rbt_begin_iterate(tree, LeftRightWalk, &iter);
    while ((node = (DPInfoTreeNode *) rbt_iterate(&iter)) != NULL)
    {
        info[count].upper_key = node->key;
        info[count].lower_key = node->key;
        info[count].count_p1 = node->count_p1;
        info[count].count_p1_p2 = node->count_p1_p2;

        count++;
    }

    elog(INFO, "%d", count);
    if(count < 100) 
        aqp_statis_dp_calculate(index, subplan_state, info, count, sample_budget);
    else
    {
        int per = count / 100 + 1;
        int count_interval = 0;
        for (int i = 0; i < count; i++)
        {
            int index = floor(i/per);
            if(i%per == 0)
                info_interval[index].lower_key = 
                    datumCopy(info[i].lower_key, subplan_state->typbyval,subplan_state->typlen);
            else if(i%per == (per - 1))
                info_interval[index].upper_key =
                    datumCopy(info[i].upper_key, subplan_state->typbyval,subplan_state->typlen);
            info_interval[index].count_p1 += info[i].count_p1;
            info_interval[index].count_p1_p2 += info[i].count_p1_p2;
            count_interval = index;

            //Datum lower_key_str = DirectFunctionCall1(date_out, info_interval[index].lower_key);
            //Datum upper_key_str = DirectFunctionCall1(date_out, info_interval[index].upper_key);
            //elog(INFO, "lower_key: %s, upper_key %s", DatumGetCString(lower_key_str), DatumGetCString(upper_key_str));
        }

        aqp_statis_dp_calculate(index, subplan_state, info_interval, count_interval, sample_budget);
    }

    pfree(info);
    pfree(info_interval);
}

static void
aqp_statis_dp_calculate(Relation index,
                        AQPProgressiveSampleScanSubplanState *subplan_state, 
                        DPInfo *info, int count, uint64 sample_budget)
{
    int k_cluster = count;
    double** rate;
    double* p1 = (double*) palloc(k_cluster * sizeof(double));
    double* p2 = (double*) palloc(k_cluster * sizeof(double));
    double* sum = (double*) palloc0((k_cluster + 1) * sizeof(double));
    
    for (int i = 0; i < k_cluster; i++) 
    {
        p1[i] = info[i].count_p1;
        p2[i] = info[i].count_p1_p2;
    }

    for (int i = 1; i < k_cluster + 1; i++) 
    {
        sum[i] = sum[i - 1] + p1[i - 1];
    }

    rate = rate_calulate (p1,p2,k_cluster);
    minimizethecost(subplan_state, p1, p2, sum, rate, k_cluster, info, sample_budget);

    for (int i = 0; i < k_cluster; i++)
        pfree(rate[i]);

    pfree(rate);
    pfree(sum);
    pfree(p1);
    pfree(p2);
}

static void 
setup_dp_scankey(Relation index, 
                 AQPProgressiveSampleScanSubplanState *subplan_state,
                 ScanKey *scanKeys)
{
    ScanKey		 scan_keys;
	int			 n_scan_keys = 0;
    int			 op_strategy;	/* operator's strategy number */
    ScanKey		 this_scan_key;  
    RegProcedure lower_opfuncid = subplan_state->lower_opfuncid;	
                 /* operator proc id used in scan */
    RegProcedure upper_opfuncid = subplan_state->upper_opfuncid;
    Oid			 op_type = subplan_state->indexkeytype;
    AttrNumber	 varattno = 1;	/* att number used in scan */
    int			 flags = 0;
    /* Datum		 scanvalue; */

	/* Allocate array for ScanKey structs: one for lower_key, one for upper_key */
	/*scan_keys = (ScanKey) palloc(n_scan_keys * sizeof(ScanKeyData));*/

    scan_keys = *scanKeys;

    scan_keys = (ScanKey) palloc(2 * sizeof(ScanKeyData));
					
    /*
	 * Convert the opclause of lower_key into a single scan key
	 */	
    this_scan_key = &scan_keys[n_scan_keys];
    op_strategy = BTGreaterEqualStrategyNumber; /*>=*/
    n_scan_keys++;
    
    /*
     * initialize the scan key's fields appropriately
     */
    ScanKeyEntryInitialize(this_scan_key,
                           flags,
                           varattno,	/* attribute number to scan */
                           op_strategy, /* op's strategy */
                           op_type,	/* strategy subtype */
                           index->rd_indcollation[0],	/* collation */
                           lower_opfuncid,	/* reg proc to use */
                           0);	/* constant */

	/*
	 * Convert the opclause of upper_key into a single scan key
	 */	
    this_scan_key = &scan_keys[n_scan_keys];
    op_strategy = BTLessEqualStrategyNumber; /*<=*/
    n_scan_keys++;

    /*
     * initialize the scan key's fields appropriately
     */
    ScanKeyEntryInitialize(this_scan_key,
                           flags,
                           varattno,	/* attribute number to scan */
                           op_strategy, /* op's strategy */
                           op_type,	/* strategy subtype */
                           index->rd_indcollation[0],	/* collation */
                           upper_opfuncid,	/* reg proc to use */
                           0);	/* constant */

	/*
	 * Return info to our caller.
	 */
    *scanKeys = scan_keys;
}


static double 
inv_prob_for_the_range(AQPProgressiveSampleScanSubplanState *subplan_state,
                       Datum lower_key, Datum upper_key, 
                       ScanKey scankeys, IndexScanDesc scandesc)
{
    double inv_prob;
    /* Change the value in scankeys */
    scankeys[0].sk_argument = 
        datumCopy(lower_key, subplan_state->typbyval, subplan_state->typlen);
    scankeys[1].sk_argument = 
        datumCopy(upper_key, subplan_state->typbyval, subplan_state->typlen);
    index_rescan(scandesc, scankeys, scandesc->numberOfKeys, NULL, 0);
    
    inv_prob = _abt_total_weight(scandesc);
    return inv_prob;
}

static double** 
rate_calulate(double* group_p1, double* group_p2, int length) 
{
    double** a = (double**)palloc0(length * sizeof(double*));
    for (int i = 0; i < length; i++) 
    {
        a[i] = (double*)palloc0(length * sizeof(double));
        for (int j = 0; j < length; j++) 
        {
            double p1_sum = 0, p2_sum = 0;
            for (int k = i; k <= j; k++) 
            {
                p1_sum += group_p1[k];
                p2_sum += group_p2[k];
            }
            if (p1_sum == 0) 
            {
                continue;
            }
            a[i][j] = p2_sum / p1_sum;
        }
    }
    return a;
}

static void
minimizethecost(AQPProgressiveSampleScanSubplanState *subplan_state,
                double* pp1, double* pp2, double* sum, double** p, 
                int k, DPInfo *info, uint64 sample_budget) 
{
    /*int custom_k = 1; */
    //int n0 = 30;

    DPOutput *result;
    int** id;
    int N = k;
    int kn = 0;
    int xx = N;
    double cal = 0;
    /*double Z = 1.959963985; */
    double N_sum = 0;
    double N_sum_p = 0;
    /* double e; */
    double* sq;
    double** dp = (double**)palloc0((N + 1) * sizeof(double*));
    int** index = (int**)palloc0((N + 1) * sizeof(int*));
    
    for (int i = 0; i <= N; i++) 
    {
        dp[i] = (double*)palloc0((k + 1) * sizeof(double));
        index[i] = (int*)palloc0((k + 1) * sizeof(int));
    }
    
    for (int i = 0; i < N; i++) 
    {
        N_sum = N_sum + pp1[i];
        N_sum_p = N_sum_p + pp2[i];
    }

    /*E = 0.05 * N_sum_p / N_sum;*/

    for (int j = 1; j <= k; j++) 
    {
        for (int i = 1; i <= N; i++) 
        {
            dp[i][j] = INFINITY;
        }
    }
    
    for (int i = 1; i <= N; i++) {
        dp[i][1] = (sum[i] - sum[0]) * sqrt(p[0][i - 1] * (1 - p[0][i - 1]));
        elog(INFO, "%f", dp[i][1]);
    }

    elog(INFO, "dp j = 1, dp[N][j] = %f", dp[N][1]);
    
    kn = k;
    for (int j = 2; j <= k; j++) 
    {
        for (int i = j; i <= N; i++) 
        {
            for (int x = j - 1; x < i; x++) 
            {
                if (dp[i][j] > dp[x][j - 1] + (sum[i] - sum[x]) * sqrt(p[x][i - 1] * (1 - p[x][i - 1]))) 
                {
                    index[i][j] = x;
                    /* elog(INFO, "i=%d, j=%d, x=%d", i, j, x);*/
                }
                dp[i][j] = Min(dp[i][j], 
                               dp[x][j - 1] + (sum[i] - sum[x]) * sqrt(p[x][i - 1] * (1 - p[x][i - 1])));
                elog(INFO, "i= %d, j=%d,x=%d, dp = %f", i,j,x,dp[i][j]);
            }
        }
        elog(INFO, "dp j = %d, dp[N][j] = %f", j, dp[N][j]);

        //if (Z * dp[N][j] / sqrt(sample_budget) + custom_k * j 
        //    > Z * dp[N][j - 1] / sqrt(sample_budget) + custom_k * (j - 1)) 
        //if (dp[N][j] > dp[N][j-1])        
        //if (dp[N][j] / sqrt(sample_budget - n0*j) 
        //    >  dp[N][j - 1] / sqrt(sample_budget - n0*(j-1))) 
        if (j == 4)
        {
            elog(INFO, "%f", dp[N][j-1]);
            elog(INFO, "percent = %f", (dp[N][1] - dp[N][j-1])/dp[N][1]);
            kn = j - 1;
            elog(INFO, "kn=%d", kn);
            break;
        }
    }


    //sample_budget = sample_budget - kn*30;

    sq = (double*)palloc0(kn * sizeof(double));
    result = palloc(kn *sizeof(DPOutput));

    id = (int**)palloc(kn * sizeof(int*));
    
    for (int i = 0; i < kn; i++) 
    {
        id[i] = (int*)palloc0(N * sizeof(int));
    }

    for (int j = kn; j >= 1; j--) 
    {
        int *idlist;
        double Ni_p = 0;
        double Ni = 0;
        int cnt = 0;
        Datum lower_key_str;
        Datum upper_key_str;
        
        idlist = (int*)palloc(N * sizeof(double));


        //elog(INFO, "zz: %d", index[xx][j]);
        for (int z = index[xx][j] + 1; z <= xx; z++) 
        {
            idlist[cnt] = z;
            elog(INFO, "z = %d", z);
            Ni_p = Ni_p + pp2[z - 1];
            Ni = Ni + pp1[z - 1];
            cnt++;
        }
        id[kn - j] = idlist;
        cal = sqrt(Ni_p * (Ni - Ni_p)) + cal;
        xx = index[xx][j];
        sq[kn - j] = sqrt(Ni_p * (Ni - Ni_p));
        
        result[kn - j].id = idlist;

        result[kn - j].lower_key = 
            datumCopy(info[id[kn - j][0] - 1].lower_key, subplan_state->typbyval, 
                      subplan_state->typlen);
        result[kn - j].upper_key = 
            datumCopy(info[id[kn - j][cnt - 1] - 1].upper_key, subplan_state->typbyval, 
                      subplan_state->typlen);

        lower_key_str = DirectFunctionCall1(date_out, result[kn - j].lower_key);
        upper_key_str = DirectFunctionCall1(date_out, result[kn - j].upper_key);

        elog(INFO, "lower: %d, upper: %d, Ni: %f, Ni_p: %f", id[kn-j][0]-1, id[kn-j][cnt-1]-1, Ni, Ni_p);
        elog(INFO, "lower_key: %s, upper_key %s", DatumGetCString(lower_key_str), DatumGetCString(upper_key_str));
        /*result[kn - j].lower_key = info[id[kn - j][0] - 1].key;
        result[kn - j].upper_key = info[id[kn - j][cnt-1] - 1].key;*/
    }

    /*E = Z * cal / sqrt(sample_budget); */
    //elog(INFO, "%f", E);
    /* e = E/(N_sum_p*sample_budget/N_sum); */

    /* e = E/N_sum_p;
    elog(INFO, "%.5f",e); */

    if (cal == 0)
    {
       subplan_state->outputcnt = 1; 
    }
    else
    {
        for (int i = 0; i < kn; i++) 
        {
            /*double n = Max(30, Z * Z * sq[i] * cal / (E * E));*/
            /*xxx Need to check*/
            double n;
            //if (cal == 0)
                //n = sample_budget;
            //else
            //    n = Z * Z * sq[i] *cal / (E * E);
            //else
            n = sample_budget * sq[i]/cal;

            result[i].ni = ceil(n);

            elog(INFO, "n%d = %f", i, ceil(n));

            result[i].percentage = (ceil(n))/sample_budget;
            elog(INFO, "p%d = %f", i, result[i].percentage);
        }
        subplan_state->outputcnt = kn;
    }

    for (int i = 0; i <= N; i++) 
    {
        pfree(dp[i]);
        pfree(index[i]);
    }

    for (int i = 0; i < kn; i++)
    {
        pfree(id[i]);
    }

    pfree(dp);
    pfree(index);
    pfree(sq);
    pfree(id);
    
    subplan_state->dpoutput = result;
}

static void
aqp_swr_scan_begin_new(CustomScanState *node,
                       EState *estate,
                       int eflags)
{
    LOCKMODE lockmode;
    AQPIndexSampleScanState *state = (AQPIndexSampleScanState *) node;
    CustomScan *scan = castNode(CustomScan, state->css.ss.ps.plan);
    AQPIndexSampleScanPrivate *private =
        (AQPIndexSampleScanPrivate *) linitial(scan->custom_private);
    Relation baserel;
    List *targetlist_with_no_prob = NULL;
    int nredundant_tupleslot;
    int i;
    bool is_pswr_subplan = AQPISSIsPSWRSubPlan(private);
    bool proj_info_set = false;
    bool want_prob;
    AQPPSWRCtlInfo pswrctl_info;
    AQPPSWRCtlSubplanInfo subplan_info;
    AQPPSWRCtlSubplanInfoData local_subplan_info_data;
    bool use_local_subplan_info = false;
    int nkeys_max;

    aqp_swrscan_fetch_pswrctl_info_param(state, private, estate);
    pswrctl_info = state->pswrctl_info;

    /* 
     * If this is a pswr subplan, our caller should have taken care of the
     * cleanups.
     */
    if (is_pswr_subplan)
    {
        Assert(state->css.ss.ps.qual == NULL);
        Assert(state->css.ss.ss_ScanTupleSlot == NULL);
    }
    else
    {
        /*
         * At this point, ExecInitCustomScan() has:
         * 1) assigned the expression context for this node;
         * 2) opened the base relation (state->css.ss.ss_CurrentRelation);
         * 3) initialized the scan tuple slot the type derived from the heap
         * relation and using the TTSOpsVirtual (which is not what we want!).
         * Because the ttsops is fixed at this point, we have to discard
         * everything starting from 3) until 5)....
         * 4) initialized the result tuple slot as a virtual tuple with the
         * heap relation relid as the expected varno in the tlist;
         * 5) initialized the plan qual (the additional plan qual not covered
         * by the index).
         */
        
        /* 
         * We must set this to aqp_pswr_scan_exec initially because we need it
         * to perform the key rescan for us.
         */
        state->css.ss.ps.ExecProcNode = aqp_pswr_scan_exec;
        
        /* 
         * Unlike index only scan, where the scan tuple slot is ok, we need to
         * replace the scan slot with heap tuple TTSOps.
         */
        Assert(state->css.ss.ss_ScanTupleSlot);
        
        /* 
         * Unfortunately, there's not too much we can do with this ExprState
         * created in ExecInitCustomScan(). Hopefully it's not wasting too much
         * memory.
         */
        state->css.ss.ps.qual = NULL;

        /*
         * The same goes with the expression inside the projection info.
         * However, let's at least free the proj info.
         */
        if (state->css.ss.ps.ps_ProjInfo)
        {
            pfree(state->css.ss.ps.ps_ProjInfo);
            state->css.ss.ps.ps_ProjInfo = NULL;
        }

        /* 
         * We can also remove the extra tuple slot from the tuple slot table.
         * They should be at the end of estate->es_tupleTable.
         */
        if (state->css.ss.ps.ps_ResultTupleSlot)
            nredundant_tupleslot = 2;
        else
            nredundant_tupleslot = 1;
        Assert(list_length(estate->es_tupleTable) >= nredundant_tupleslot);
        for (i = 0; i < nredundant_tupleslot; ++i)
        {
            TupleTableSlot *slot =
                (TupleTableSlot *) llast(estate->es_tupleTable);
            if (slot == state->css.ss.ps.ps_ResultTupleSlot ||
                slot == state->css.ss.ss_ScanTupleSlot)
            {
                estate->es_tupleTable = list_delete_last(estate->es_tupleTable);
                if (slot->tts_tupleDescriptor)
                {
                   ReleaseTupleDesc(slot->tts_tupleDescriptor);
                   slot->tts_tupleDescriptor = NULL;
                }
                if (!TTS_FIXED(slot))
                {
                   if (slot->tts_values)
                       pfree(slot->tts_values);
                   if (slot->tts_isnull)
                       pfree(slot->tts_isnull);
                }
                pfree(slot);
            }
            else
            {
                ereport(ERROR,
                        errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("AQPSWRScan failed to find the redundant tuple "
                               "slots made by ExecInitCustomScan()"));
            }
        }
        state->css.ss.ps.ps_ResultTupleSlot = NULL;
        state->css.ss.ss_ScanTupleSlot = NULL;
    }

    /* 
     * OK, we're ready to do our own init. The following is similar to
     * ExecInitIndexScan().
     */
    baserel = state->css.ss.ss_currentRelation;
    state->css.ss.ss_currentScanDesc = NULL; /* no heap scan here */

    /* 
     * We can share the same scan slot and projection info across
     * all pswr subplans that are swr scans. This will reset the scan tuple
     * slot to the previously allocated scan tuple slot so that we'll skip
     * allocating a new one.
     */
    if (is_pswr_subplan)
    {
        AQPProgressiveSampleScanState *psss =
            (AQPProgressiveSampleScanState *) state;
        if (psss->swr_scanslot != NULL)
        {
            Assert(psss->swr_scan_descriptor != NULL);

            state->css.ss.ss_ScanTupleSlot = psss->swr_scanslot;
            state->css.ss.ps.scandesc = psss->swr_scan_descriptor;
            state->css.ss.ps.scanops = psss->swr_scanops;
            state->css.ss.ps.scanopsset = true;
            
            state->css.ss.ps.ps_ProjInfo = psss->swr_projInfo;
            state->css.ss.ps.resultopsset = psss->swr_resultopsset;
            state->css.ss.ps.resultopsfixed = psss->swr_resultopsfixed;
            state->css.ss.ps.resultops = psss->swr_resultops;
            proj_info_set = true;

            state->allnullslot = psss->swr_allnullslot;
        }
    }

    /*
     * Find out whether we want to include the probability column in the scan
     * tuple slot.
     *
     * NOTE We assume the plan rewriter always appends an extra entry in the
     * target list where varno == scan->scan.scanrelid and varattno == natts +
     * 1 where natts is the number of columns in the scan table, if it wants
     * to include the probability column in the scan tuple slot.
     */

    want_prob = false;
    if (list_length(scan->scan.plan.targetlist) != 0)
    {
        TargetEntry *tle = llast_node(TargetEntry, scan->scan.plan.targetlist);
        if (IsA(tle->expr, Var))
        {
            Var *var = (Var *) tle->expr;
            if (var->varno == scan->scan.scanrelid)
            {
                TupleDesc desc = RelationGetDescr(baserel);
                if (var->varattno == desc->natts + 1)
                {
                    /* ok, this is the probability column */
                    want_prob = true;

                    /* 
                     * A shorter targetlist without the probability column that
                     * will be used for initializing the projection info.
                     */
                    targetlist_with_no_prob =
                        list_copy(scan->scan.plan.targetlist);
                    targetlist_with_no_prob =
                        list_delete_last(targetlist_with_no_prob);
                }
            }
        }
    }

    /* 
     * This is old-style probability passing logic, which we don't support
     * now.
     */
    if (want_prob)
    {
        elog(ERROR, "unexpected rewritten sample_prob() call");
    }
    state->want_prob = false;
        
    /*
     * Initialize scan slot (and scan type).
     */
    if (state->css.ss.ss_ScanTupleSlot == NULL)
    {
        ExecInitScanTupleSlot(estate, &state->css.ss,
                              RelationGetDescr(baserel),
                              table_slot_callbacks(baserel));

    }

    /*
     * Initialize result type and projection. Result type can be shared across
     * all pswr subplans (including swr scan and swr index only scans).
     */
    if (state->css.ss.ps.ps_ResultTupleDesc == NULL)
    {
        ExecInitResultTypeTL(&state->css.ss.ps);
    }
    if (state->allnullslot == NULL)
    {
        state->allnullslot =
            ExecStoreAllNullTuple(ExecAllocTableSlot(
                &estate->es_tupleTable,
                state->css.ss.ps.ps_ResultTupleDesc,
                &TTSOpsVirtual));
                                      
    }
    
    /* Build project info. */
    if (!proj_info_set)
    {
        if (state->want_prob)
        {
            /* 
             * Has one extra prob column. We need to use the shorter targetlist
             * to initialize the projection info so that it won't try to fetch
             * an non-existent column.
             *
             * Here, we must assign a projection info because the true target
             * list never matches the scan rel tuple descriptor.
             */
            if (!state->css.ss.ps.ps_ResultTupleSlot)
            {
                /* 
                 * Some other PSWR subplan may have initialized the result tuple
                 * slot.
                 */
                ExecInitResultSlot(&state->css.ss.ps, &TTSOpsVirtual);
                state->css.ss.ps.resultops = &TTSOpsVirtual;
                state->css.ss.ps.resultopsfixed = true;
                state->css.ss.ps.resultopsset = true;
            }

            state->css.ss.ps.ps_ProjInfo =
                ExecBuildProjectionInfo(targetlist_with_no_prob,
                                        state->css.ss.ps.ps_ExprContext,
                                        state->css.ss.ps.ps_ResultTupleSlot,
                                        &state->css.ss.ps,
                                        state->css.ss.ss_ScanTupleSlot
                                            ->tts_tupleDescriptor);
        }
        else
        {
            /* 
             * No prob column. We can initialize the projection info as is. 
             * 
             * Note that this is safe to call for a PSWR subplan even if some
             * other PSWR subplan that is index-only and has initialized a
             * result tuple slot, which will be simply ignored (if 
             * the tlist matches the heap tuple descriptor), or reused (if they
             * do not match).
             */
            ExecAssignScanProjectionInfo(&state->css.ss);
        }
    }

    /*
     * Initialize child expressions.
     */
    state->css.ss.ps.qual =
        ExecInitQual(scan->scan.plan.qual, &state->css.ss.ps);
    state->indexqualorig =
        ExecInitQual(private->indexqualorig, &state->css.ss.ps);

    if (!is_pswr_subplan)
    {
    //    Assert(!pswrctl_info->subplan_info);
    //    pswrctl_info->nsubplans = 1;
    //    pswrctl_info->subplan_info =
    //        palloc0(sizeof(AQPPSWRCtlSubplanInfoData));
    //    subplan_info = pswrctl_info->subplan_info;
        state->pswrctl_sampler_id =
            aqp_swrscan_append_sampler_ctl(pswrctl_info);

        if (pswrctl_info->pswr_driver_sampler_id >= 0)
        {
            memset(&local_subplan_info_data, 0,
                   sizeof(AQPPSWRCtlSubplanInfoData));
            subplan_info = &local_subplan_info_data;
            state->pswrctl_subplan_id = -1;
            use_local_subplan_info = true;
        }
        else
        {
            subplan_info =
                aqp_swrscan_append_pswrctl_subplan_info(pswrctl_info);
            state->pswrctl_subplan_id =
                (int) (subplan_info - pswrctl_info->subplan_info);
        }
    }
    else
    {
        /*
         * aqp_pswr_scan_begin should set pswrctl_info->cur_plan_id before
         * calling us.
         */
        Assert(pswrctl_info->subplan_info);
        subplan_info = &pswrctl_info->subplan_info[pswrctl_info->cur_plan_id];
        state->pswrctl_subplan_id = pswrctl_info->cur_plan_id;
    }

    /*
     * Store the per-table sample size so the exec function can enforce a
     * per-table stopping condition for inner (non-driver) scans in a join.
     * This fixes the bug where the inner scan shared the global counter and
     * could return multiple tuples per outer tuple (corrupting WanderJoin).
     */
    if (!use_local_subplan_info)
        Assert(state->pswrctl_subplan_id >= 0 &&
               state->pswrctl_subplan_id < pswrctl_info->nsubplans);

    if (!is_pswr_subplan &&
        !private->sample_size_expr &&
        private->sample_size > 0)
    {
        pswrctl_info->sampler_ctl[state->pswrctl_sampler_id].sample_size =
            private->sample_size;
    }

    /* 
     * EXPLAIN stops here without opening the index.
     * See nodeIndexscan.c for rationale.
     */
    if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
    {
        /* 
         * The caller, aqp_progressive_swr_scan_begin(), will handle any
         * rewriting needed for EXPLAIN VERBOSE.
         */
        if (is_pswr_subplan)
            return;

        /* 
         * Explain only, we need to replace the Vars with invalid
         * references into the heap tuple with sample_prob() in the
         * query plan right now, before it is sent for printing.
         */
        if (aqp_is_in_explain_verbose)
            aqp_rewrite_sample_prob_dummy_var_for_explain(&state->css.ss.ps);
        return;
    }

    if (!IsMVCCSnapshot(estate->es_snapshot))
    {
        /*
         * It's not ok to use non-MVCC snapshot for index sample scan
         * because, otherwise, there might be multiple valid tuples in
         * one HOT chain matching one TID. That breaks the assumption that
         * one TID sampled TID from the index produces exactly one tuple.
         *
         * Do this check as early as possible to prevent wasting efforts
         * in all the initialization. However, we don't really care if this
         * is an EXPLAIN command.
         */
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("TABLESAMPLE SWR() must be used under MVCC "
                        "snapshot")));
    }

    /* open the index */
    lockmode = exec_rt_fetch(scan->scan.scanrelid, estate)->rellockmode;
    state->indexrel = index_open(private->indexid, lockmode);

    /* 
     * This version builds not only the scan key as indicated by the original
     * index quals, it also sets up three additional partition keys (lmost,
     * middle, rmost) in case we want to supply logical partition keys
     * from pswrctl.
     */
    aqp_index_build_scankeys((PlanState *) state,
                             state->indexrel,
                             private->indexqual,
                             subplan_info);
    /* elog(NOTICE, "aqp_swr_scan_begin_new: sid=%d valid=%d indexqual_len=%d",
         state->pswrctl_subplan_id,
         (int) subplan_info->valid,
         list_length(private->indexqual)); */

     /*
     * Fallback for parameterized index quals (e.g., inner side of WanderJoin):
     * aqp_index_build_scankeys currently marks non-Const quals as invalid.
     * For non-PSWR SWR queries, we can safely use PostgreSQL's standard
     * runtime-key builder and still execute through the new agg path.
     * (Mirrors the same fallback in aqp_swr_index_only_scan_begin_new().)
     */
    if (!subplan_info->valid)
    {
        state->runtime_keys_ready = false;
        state->num_runtime_keys = 0;
        state->runtime_keys = NULL;

        ExecIndexBuildScanKeys((PlanState *) state,
                               state->indexrel,
                               private->indexqual,
                               /*isorderby=*/false,
                               &state->scan_keys,
                               &state->num_scan_keys,
                               &state->runtime_keys,
                               &state->num_runtime_keys,
                               /*arrayKeys=*/NULL,
                               /*numArrayKeys=*/NULL);

        if (state->num_runtime_keys > 0 && state->runtime_context == NULL)
            state->runtime_context = CreateExprContext(estate);

        subplan_info->valid = true;
        subplan_info->emptyres = false;
        subplan_info->scan_keys = state->scan_keys;
        subplan_info->n_scan_keys = state->num_scan_keys;
        subplan_info->allequi = true;
        subplan_info->partition_attno = InvalidAttrNumber;
        subplan_info->partition_atttypid = InvalidOid;
        subplan_info->partition_keys = NULL;
        subplan_info->lmost_partition_keys = NULL;
        subplan_info->rmost_partition_keys = NULL;
        subplan_info->n_partition_keys = 0;
        subplan_info->n_lmost_partition_keys = 0;
        subplan_info->n_rmost_partition_keys = 0;
    }

    /* elog(NOTICE, "aqp_swr_scan_begin_new: sid=%d fallback ran, num_scan_keys=%d num_runtime_keys=%d",
             state->pswrctl_subplan_id,
             state->num_scan_keys,
             state->num_runtime_keys); */

    if (subplan_info->valid && !subplan_info->emptyres &&
        state->num_runtime_keys == 0)
    {
        state->scan_keys = subplan_info->scan_keys;
        state->num_scan_keys = subplan_info->n_scan_keys;
    }

    if (!subplan_info->valid || subplan_info->emptyres){
        return; 
    }

    /* 
     * Initialize the sample scan states. But only do so when this is not a
     * subplan, in which case there is only one single shared sample scan
     * state.
     */
    if (!is_pswr_subplan)
    {
        if (private->repeatable_expr)
        {
            /* 
             * XXX There's no way for us to support this right now as online
             * insertion could change how tuples are mapped to random numbers,
             * even if we can reinitialize the random number generator using
             * the same seed. We'll need something that could freeze/lock
             * the index for the entire duration of the query, but that
             * would be too disruptive.
             */
            elog(ERROR, "pswr/swr sampler does not support REPEATABLE");
        }

        sampler_random_init_state(/*seed=*/(long) random(), state->rand_state);
    }
    
    /*
     * The nkeys arugment to index_beginscan only determines the size of
     * internal scankey array that AB-tree creates, which we want it to set it
     * to the max. If we supply a shorter one to index_rescan, we are free to
     * set scandesc->numberOfKeys to the actual number.
     */
    nkeys_max = subplan_info->n_scan_keys;
    if (!subplan_info->allequi)
    {
        nkeys_max = Max(nkeys_max, subplan_info->n_partition_keys);
        nkeys_max = Max(nkeys_max, subplan_info->n_lmost_partition_keys);
        nkeys_max = Max(nkeys_max, subplan_info->n_rmost_partition_keys);
    }
    state->scandesc = index_beginscan(baserel,
                                      state->indexrel,
                                      state->css.ss.ps.state->es_snapshot,
                                      /*nkeys=*/nkeys_max,
                                      /*norderby=*/0);
    /* 
     * Don't rescan the index right now as we need the pswrctl to tell us which
     * scankey to use.
     */
}

static TupleTableSlot*
aqp_swr_scan_exec_new(PlanState *node)
{
    AQPIndexSampleScanState *state = (AQPIndexSampleScanState *) node;
    /*CustomScan *scan = castNode(CustomScan, state->css.ss.ps.plan); */
    AQPPSWRCtlInfo  pswrctl_info;
    ExprState       *qual;
    ProjectionInfo  *projInfo;
    ExprContext     *econtext;
    IndexScanDesc   scandesc;
    TupleTableSlot  *slot;    
    bool is_driver;
    AQPTableSamplerCtl sampler_ctl;

    qual = state->css.ss.ps.qual;
    projInfo = state->css.ss.ps.ps_ProjInfo;
    econtext = state->css.ss.ps.ps_ExprContext;
    scandesc = state->scandesc;
    slot = state->css.ss.ss_ScanTupleSlot;
    pswrctl_info = state->pswrctl_info;

    {
        sampler_ctl = aqp_swrscan_get_sampler_ctl(state);
        is_driver = sampler_ctl->is_driver;
    
    /*
     * Stopping condition:
     * - Inner (non-driver) scans in a WanderJoin must stop after their own
     *   per-table sample_size (typically 1), not the global total.
     *   Sharing the global counter allows the inner scan to return multiple
     *   tuples per outer tuple, which corrupts the WanderJoin estimator.
     * - The driver (outer) scan stops when the global counter is reached.
     */
        if (!is_driver && sampler_ctl->sample_size > 0)
        {
            if (sampler_ctl->num_samples_fetched >= sampler_ctl->sample_size)
                return NULL;
        }
        else if (is_driver &&
                pswrctl_info->num_samples_fetched >= pswrctl_info->sample_size)
        {
            return NULL;
        }
    }
      

#define REJECT_TUPLE() \
    pswrctl_info->rejected = true; \
    return state->allnullslot; \
    
    /* 
     * The loop logic is temporarily disabled until we want to
     * bring back the sample-until-first-accepted semantics .
     */
    /* for (;;) */
    {
        double random_number;
        ItemPointer tid;

        ResetExprContext(econtext);
    
        /* 
         * We now do a separate call to index_samplenext_tid here
         * because we may have to fetch the heap tuple into a separate
         * tuple slot when the caller asks for the sampling probability.
         * In that case, we will have to deform the tuple into a virtual tuple
         * so that we can add the extra column to the scan tuple.
         */
        random_number = sampler_random_fract(state->rand_state);
        
        if (state->use_batch_sampling)
            tid = index_samplenextbatch_tid(scandesc, random_number);
        else
            tid = index_samplenext_tid(scandesc, random_number);

      //  pswrctl_info->inv_prob *= ((ABTScanOpaque) scandesc->opaque)->inv_prob;

        {
            float8 inv_prob_cur = ((ABTScanOpaque) scandesc->opaque)->inv_prob;

            sampler_ctl->inv_prob = inv_prob_cur;
            ++sampler_ctl->num_samples_fetched;

            /* Legacy field for single-sampler paths and fallback code. */
            pswrctl_info->inv_prob = inv_prob_cur;
        }

        /*
         * Only the driver (outer) scan advances the global sample counter.
         * Inner scans track their own per-table counter, and sharing the global
         * counter would cause the inner scan to exhaust the total budget across
         * all tables rather than stopping after its own per-table sample_size.
         */
        {
            if (is_driver)
                ++pswrctl_info->num_samples_fetched;
        }


        /* warn the user if the rejection rate is too high */
        //aqp_sample_scan_check_for_high_rejection_rate(&state->sss);

        epoch_maybe_refresh();

        CHECK_FOR_INTERRUPTS();

        if (tid == NULL)
        {
            /* AB-tree rejection */
            if(isinf(state -> sss.HighRejectionRateWarningThreshold)) {
                return NULL; // might remove later, check with prof during meeting
            }
            REJECT_TUPLE();
        }

        if (is_driver && pswrctl_info->want_partition_key)
        {
            /* TODO record the current partition key here */
            /* NOTE Only the AB-tree rejection must be excluded because
             * for the rest of the cases, we have fetched an index tuple,
             * and thus they will count towards the rejection rate in some
             * partition.
             *
             * For instance, let's say we fetched x = 100 and we have a
             * partition [99, 110). If it is rejected below, it will count
             * towards a rejected sample in that partition. If it is accepted
             * below, we will be able to get some valid value in the
             * aqp_approx_sum_internal_accum function so this will count as
             * accepted.
             *
             * To get the value, we should read it from the index tuple,
             * not the heap tuple.
             */
            bool isnull;
            uint64 i = pswrctl_info->next_kv_idx++;
            AttrNumber attno =
                pswrctl_info->subplan_info[pswrctl_info->cur_plan_id].partition_attno;
            
            if (scandesc->xs_itup == NULL)
                elog(ERROR,
                    "PSWR driver requested partition key, but index tuple was not returned");

            pswrctl_info->recorded_kv_pairs[i << 1] =
                index_getattr(scandesc->xs_itup,
                              attno,
                              scandesc->xs_itupdesc,
                              &isnull);
            if (aqp_optimization_strategy == AQP_OPTIMIZATION_STRATEGY_DP ||
                    aqp_optimization_strategy == AQP_OPTIMIZATION_STRATEGY_OPT_STRAT)
            {
                pswrctl_info->recorded_kv_pairs[(i << 1) + 1] = 0;
            }
            if (aqp_pswr_tree)
            {
                pswrctl_info->pswrctl->tree_level = 
                    ((ABTScanOpaque) scandesc->opaque)->locked_path->alp_level + 1;
        
                pswrctl_info->samples_height[i] = 
                    ((ABTScanOpaque) scandesc->opaque)->sample_height + 1;
            }
            if (isnull)
            {
                elog(ERROR, "unhandled null partition key");
            }
        }
        else if (is_driver)
        {
            pswrctl_info->pswrctl->tree_level =
                ((ABTScanOpaque) scandesc->opaque)->sample_height + 1;
        }

        /* now fetch the heap tuple */
        if (!index_fetch_heap(scandesc, slot))
        {
            /* not visible */
            REJECT_TUPLE();
        }
        
        Assert(!scandesc->xs_recheck);

        /* now evaluate the plan qual and projection if any */
        econtext->ecxt_scantuple = slot;
        if (qual == NULL || ExecQual(qual, econtext))
        {
            pswrctl_info->rejected = false;
            if (projInfo)
            {
                return ExecProject(projInfo);
            }
            else
            {
                return slot;
            }
        }
        else
        {
            /* regular rejection */
            REJECT_TUPLE();
        }
    }

#undef REJECT_TUPLE

    elog(ERROR, "unreachable");
    return NULL;
}


static void
aqp_swr_scan_end_new(CustomScanState *node)
{
    AQPIndexSampleScanState *state = (AQPIndexSampleScanState *) node;
    
    /* 
     * NOTE we have to set result tuple slot to something with
     * a valid ttsops, or ExecEndCustomScan() will blindly invoke
     * ExecClearTuple on a NULL pointer!
     *
     * We don't need to invoke clear either of the scan or the result tuple
     * though. It will be handled by ExecEndCustomScan.
     */
    if (!state->css.ss.ps.ps_ResultTupleSlot)
        state->css.ss.ps.ps_ResultTupleSlot =
            state->css.ss.ss_ScanTupleSlot;

    if (state->scandesc)
        index_endscan(state->scandesc);
    if (state->indexrel)
        index_close(state->indexrel, NoLock);
}

static void
aqp_swr_scan_rescan_new(CustomScanState *node)
{
    AQPIndexSampleScanState *state = (AQPIndexSampleScanState *) node;

    /* Let the PSWR dispatcher choose keys and perform index_rescan again. */
    state->css.ss.ps.ExecProcNode = aqp_pswr_scan_exec;

    if (state->scandesc && state->scandesc->xs_heapfetch != NULL)
        table_index_fetch_reset(state->scandesc->xs_heapfetch);

    if (state->css.ss.ss_ScanTupleSlot)
        ExecClearTuple(state->css.ss.ss_ScanTupleSlot);
    if (state->allnullslot)
        ExecClearTuple(state->allnullslot);
    /*
     * Reset the per-table sample counter so this inner scan can return its
     * allotted sample_size tuples again for the next outer tuple in a join.
     */
    if (state->pswrctl_info &&
        state->pswrctl_info->sampler_ctl &&
        state->pswrctl_sampler_id >= 0 &&
        state->pswrctl_sampler_id < state->pswrctl_info->nsamplers)
    {
        state->pswrctl_info->sampler_ctl[state->pswrctl_sampler_id]
            .num_samples_fetched = 0;
    }

    state->use_batch_sampling = false;
}

static void
aqp_swr_index_only_scan_begin_new(CustomScanState *node,
                                  EState *estate,
                                  int eflags)
{
    LOCKMODE lockmode;
    AQPIndexSampleScanState *state = (AQPIndexSampleScanState *) node;
    CustomScan *scan = castNode(CustomScan, state->css.ss.ps.plan);
    AQPIndexSampleScanPrivate *private =
        (AQPIndexSampleScanPrivate *) linitial(scan->custom_private);
    int scan_tlist_len;
    int itupdesc_natts;
    bool is_pswr_subplan = AQPISSIsPSWRSubPlan(private);
    bool want_prob;
    AQPPSWRCtlInfo pswrctl_info;
    AQPPSWRCtlSubplanInfo subplan_info;
    AQPPSWRCtlSubplanInfoData local_subplan_info_data;
    bool use_local_subplan_info = false;
    int nkeys_max;

    aqp_swrscan_fetch_pswrctl_info_param(state, private, estate);
    pswrctl_info = state->pswrctl_info;

    /*
     * At this point, ExecInitCustomScan() has:
     * 1) assigned the expression context for this node;
     * 2) opened the base relation (state->css.ss.ss_CurrentRelation);
     * 3) initialized the scan tuple slot with type derived from
     * `scan->custom_scan_tlist`;
     * 4) initialized the result tuple slot as a virtual tuple with INDEX_VAR
     * as the expected varno in the tlist;
     * 5) initialized the plan qual (the additional plan qual not covered
     * by the index).
     */
    
    if (!is_pswr_subplan)
    {
        /* 
         * We must set this to aqp_pswr_scan_exec initially because we need it
         * to perform the key rescan for us.
         */
        state->css.ss.ps.ExecProcNode = aqp_pswr_scan_exec;
    }
    
    /* no heap scan; just to be extra cautious in case this is set */
    state->css.ss.ss_currentScanDesc = NULL;
    
    /* another table slot for visibility check */
    if (state->table_slot == NULL)
    {
        /* 
         * All PSWR subplans that are index-only can share one single table
         * slot for visibility check (unless we want to support parallelism).
         */
        state->table_slot = ExecAllocTableSlot(&estate->es_tupleTable,
            RelationGetDescr(state->css.ss.ss_currentRelation),
            table_slot_callbacks(state->css.ss.ss_currentRelation));
    }

    /* need to initialize indexqual */
    state->indexqual =
        ExecInitQual(private->indexqual, (PlanState *) state);

    /* 
     * NOTE we may have an extra result tuple slot if the projection info
     * is NULL because of matching indextlist and target list.
     */
    if (is_pswr_subplan)
    {
        /*
         * If this is a pswr subplan, ExecInitCustomScan() never had a chance
         * to initialize the scan & result tuple slot yet, though someone may
         * have initialized the result tuple slot for us if this is not the
         * first subplan.
         */
        TupleDesc   scan_descriptor;

        scan_descriptor = ExecTypeFromTL(scan->custom_scan_tlist);
        ExecInitScanTupleSlot(estate, &state->css.ss, scan_descriptor,
                              &TTSOpsVirtual);

        if (state->css.ss.ps.ps_ResultTupleDesc == NULL)
        {
            ExecInitResultTypeTL(&state->css.ss.ps);
        }
        if (state->css.ss.ps.ps_ResultTupleSlot == NULL)
        {
            ExecInitResultSlot(&state->css.ss.ps, &TTSOpsVirtual);
        }
        
        /* Initialize projection info. */
        ExecAssignScanProjectionInfoWithVarno(&state->css.ss, INDEX_VAR);

        /* Initialize qp qual. */
        state->css.ss.ps.qual =
            ExecInitQual(scan->scan.plan.qual, &state->css.ss.ps);
    }

    //if (state->allnullslot == NULL)
    //{
    state->allnullslot =
        ExecStoreAllNullTuple(ExecAllocTableSlot(
            &estate->es_tupleTable,
            state->css.ss.ps.ps_ResultTupleDesc,
            &TTSOpsVirtual));
                                      
    //}
    
    /* open the index */
    lockmode = (eflags & EXEC_FLAG_EXPLAIN_ONLY) ? NoLock
        : exec_rt_fetch(scan->scan.scanrelid, estate)->rellockmode;
    state->indexrel = index_open(private->indexid, lockmode);

    /* 
     * This needs to happen earlier than early return for EXPLAIN, as
     * aqp_swr_scan_begin() expects any prior subplans have consistently set
     * state->want_prob.
     */
    scan_tlist_len = list_length(scan->custom_scan_tlist);
    itupdesc_natts = IndexRelationGetNumberOfAttributes(state->indexrel);
    if (list_length(scan->custom_scan_tlist) > itupdesc_natts)
    {
        if (scan_tlist_len != itupdesc_natts + 1)
        {
            ereport(ERROR,
                    errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("AQPSWRIndexOnlyScan scan tlist length != : "
                           "itupdesc->natts + 1: %d %d",
                           scan_tlist_len,
                           itupdesc_natts));
        }
        want_prob = true;
    }
    else
    {
        if (scan_tlist_len != itupdesc_natts)
        {
            ereport(ERROR,
                    errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("AQPSWRIndexOnlyScan scan tlist length != : "
                           "itupdesc->natts: %d %d",
                           scan_tlist_len,
                           itupdesc_natts));
        }
        want_prob = false;
    }

    /* 
     * This is old-style probability passing logic, which we don't support
     * now.
     */
    if (want_prob)
    {
        elog(ERROR, "unexpected rewritten sample_prob() call");
    }

    /* 
     * Either all pswr subplans want the sample prob column or none wants that. 
     */
    Assert(!is_pswr_subplan ||
        ((AQPProgressiveSampleScanState *) state)->selected_subplan_idx == 0 ||
        state->want_prob == want_prob);
    state->want_prob = want_prob;

    if (!is_pswr_subplan)
    {
      //  Assert(!pswrctl_info->subplan_info);
      //  pswrctl_info->nsubplans = 1;
      //  pswrctl_info->subplan_info =
      //      palloc0(sizeof(AQPPSWRCtlSubplanInfoData));
      //  subplan_info = pswrctl_info->subplan_info;
        state->pswrctl_sampler_id =
            aqp_swrscan_append_sampler_ctl(pswrctl_info);

        if (pswrctl_info->pswr_driver_sampler_id >= 0)
        {
            memset(&local_subplan_info_data, 0,
                   sizeof(AQPPSWRCtlSubplanInfoData));
            subplan_info = &local_subplan_info_data;
            state->pswrctl_subplan_id = -1;
            use_local_subplan_info = true;
        }
        else
        {
            subplan_info =
                aqp_swrscan_append_pswrctl_subplan_info(pswrctl_info);
            state->pswrctl_subplan_id =
                (int) (subplan_info - pswrctl_info->subplan_info);
        }
    }
    else
    {
        /*
         * aqp_pswr_scan_begin should set pswrctl_info->cur_plan_id before
         * calling us.
         */
        Assert(pswrctl_info->subplan_info);
        subplan_info = &pswrctl_info->subplan_info[pswrctl_info->cur_plan_id];
        state->pswrctl_subplan_id = pswrctl_info->cur_plan_id;
    }
    if (!use_local_subplan_info)
        Assert(state->pswrctl_subplan_id >= 0 &&
               state->pswrctl_subplan_id < pswrctl_info->nsubplans);

    /*
     * Store the per-table sample size so the exec function can enforce a
     * per-table stopping condition for inner (non-driver) scans in a join.
     * This fixes the bug where the inner scan shared the global counter and
     * could return multiple tuples per outer tuple (corrupting WanderJoin).
     */
    if (!is_pswr_subplan &&
        !private->sample_size_expr &&
        private->sample_size > 0)
    {
        pswrctl_info->sampler_ctl[state->pswrctl_sampler_id].sample_size =
            private->sample_size;
    }
    
    /* 
     * This version builds not only the scan key as indicated by the original
     * index quals, it also sets up three additional partition keys (lmost,
     * middle, rmost) in case we want to supply logical partition keys
     * from pswrctl.
     */
    aqp_index_build_scankeys((PlanState *) state,
                             state->indexrel,
                             private->indexqual,
                             subplan_info);

    /*
     * Fallback for parameterized index quals (e.g., inner side of WanderJoin):
     * aqp_index_build_scankeys currently marks non-Const quals as invalid.
     * For non-PSWR SWR queries, we can safely use PostgreSQL's standard
     * runtime-key builder and still execute through the new agg path.
     */
    if (!subplan_info->valid)
    {
        state->runtime_keys_ready = false;
        state->num_runtime_keys = 0;
        state->runtime_keys = NULL;

        ExecIndexBuildScanKeys((PlanState *) state,
                               state->indexrel,
                               private->indexqual,
                               /*isorderby=*/false,
                               &state->scan_keys,
                               &state->num_scan_keys,
                               &state->runtime_keys,
                               &state->num_runtime_keys,
                               /*arrayKeys=*/NULL,
                               /*numArrayKeys=*/NULL);

        if (state->num_runtime_keys > 0 && state->runtime_context == NULL)
            state->runtime_context = CreateExprContext(estate);

        subplan_info->valid = true;
        subplan_info->emptyres = false;
        subplan_info->scan_keys = state->scan_keys;
        subplan_info->n_scan_keys = state->num_scan_keys;
        subplan_info->allequi = true;
        subplan_info->partition_attno = InvalidAttrNumber;
        subplan_info->partition_atttypid = InvalidOid;
        subplan_info->partition_keys = NULL;
        subplan_info->lmost_partition_keys = NULL;
        subplan_info->rmost_partition_keys = NULL;
        subplan_info->n_partition_keys = 0;
        subplan_info->n_lmost_partition_keys = 0;
        subplan_info->n_rmost_partition_keys = 0;
    }

    if (subplan_info->valid && !subplan_info->emptyres &&
        state->num_runtime_keys == 0)
    {
        state->scan_keys = subplan_info->scan_keys;
        state->num_scan_keys = subplan_info->n_scan_keys;
    }

    if (!subplan_info->valid || subplan_info->emptyres)
        return; 


    /* 
     * EXPLAIN stops here without opening the index.
     * See nodeIndexonlyscan.c for rationale.
     */
    if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
    {
        index_close(state->indexrel, NoLock);
        state->indexrel = NULL;

        /* 
         * The caller, aqp_progressive_swr_scan_begin(), will handle any
         * rewriting needed for EXPLAIN VERBOSE.
         */
        if (is_pswr_subplan)
            return;

        /* 
         * Explain only, we need to replace the Vars with invalid
         * references into the heap tuple with sample_prob() in the
         * query plan right now, before it is sent for printing.
         */
        if (aqp_is_in_explain_verbose)
            aqp_rewrite_sample_prob_dummy_var_for_explain((PlanState *) state);
        return;
    }

    if (!IsMVCCSnapshot(estate->es_snapshot))
    {
        /*
         * It's not ok to use non-MVCC snapshot for index sample scan
         * because, otherwise, there might be multiple valid tuples in
         * one HOT chain matching one TID. That breaks the assumption that
         * one TID sampled TID from the index produces exactly one tuple.
         *
         * Do this check as early as possible to prevent wasting efforts
         * in all the initialization. However, we don't really care if this
         * is an EXPLAIN command.
         */
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("TABLESAMPLE SWR() must be used under MVCC "
                        "snapshot")));
    }

    state->vmbuffer = InvalidBuffer;
    
    /* initialize the sample scan states */
    if (!is_pswr_subplan)
    {
        if (private->repeatable_expr)
        {
            /* 
             * XXX There's no way for us to support this right now as online
             * insertion could change how tuples are mapped to random numbers,
             * even if we can reinitialize the random number generator using
             * the same seed. We'll need something that could freeze/lock
             * the index for the entire duration of the query, but that
             * would be too disruptive.
             */
            elog(ERROR, "pswr/swr sampler does not support REPEATABLE");
        }

        sampler_random_init_state(/*seed=*/(long) random(), state->rand_state);
    }

    /*
     * The nkeys arugment to index_beginscan only determines the size of
     * internal scankey array that AB-tree creates, which we want it to set it
     * to the max. If we supply a shorter one to index_rescan, we are free to
     * set scandesc->numberOfKeys to the actual number.
     */
    nkeys_max = subplan_info->n_scan_keys;
    if (!subplan_info->allequi)
    {
        nkeys_max = Max(nkeys_max, subplan_info->n_partition_keys);
        nkeys_max = Max(nkeys_max, subplan_info->n_lmost_partition_keys);
        nkeys_max = Max(nkeys_max, subplan_info->n_rmost_partition_keys);
    }
    state->scandesc = index_beginscan(state->css.ss.ss_currentRelation,
                                      state->indexrel,
                                      state->css.ss.ps.state->es_snapshot,
                                      /*nkeys=*/nkeys_max,
                                      /*norderby=*/0);
    state->scandesc->xs_want_itup = true;

    /* 
     * Don't rescan the index right now as we need the pswrctl to tell us which
     * scankey to use.
     */
}

static TupleTableSlot*
aqp_swr_index_only_scan_exec_new(PlanState *node)
{
    AQPIndexSampleScanState *state = (AQPIndexSampleScanState *) node;
    AQPPSWRCtlInfo  pswrctl_info;
    /*CustomScan *cscan = castNode(CustomScan, node->plan); */
    ExprState       *qual;
    ProjectionInfo  *projInfo;
    ExprContext     *econtext;
    IndexScanDesc   scandesc;
    TupleTableSlot  *slot;
    bool is_driver;
    AQPTableSamplerCtl sampler_ctl;

    qual = state->css.ss.ps.qual;
    projInfo = state->css.ss.ps.ps_ProjInfo;
    econtext = state->css.ss.ps.ps_ExprContext;
    scandesc = state->scandesc;
    slot = state->css.ss.ss_ScanTupleSlot;
    pswrctl_info = state->pswrctl_info;

    {
        sampler_ctl = aqp_swrscan_get_sampler_ctl(state);
        is_driver = sampler_ctl->is_driver;

    /*
     * Stopping condition:
     * - Inner (non-driver) scans in a WanderJoin must stop after their own
     *   per-table sample_size (typically 1), not the global total.
     *   Sharing the global counter allows the inner scan to return multiple
     *   tuples per outer tuple, which corrupts the WanderJoin estimator.
     * - The driver (outer) scan stops when the global counter is reached.
     */

        if (!is_driver && sampler_ctl->sample_size > 0)
        {
            if (sampler_ctl->num_samples_fetched >= sampler_ctl->sample_size)
                return NULL;
        }
        else if (is_driver &&
                pswrctl_info->num_samples_fetched >= pswrctl_info->sample_size)
        {
            return NULL;
        }
    }

#define REJECT_TUPLE() \
    pswrctl_info->rejected = true; \
    return state->allnullslot; \

    /* 
     * The loop logic is temporarily disabled until we want to
     * bring back the sample-until-first-accepted semantics .
     */
    /* for (;;) */
    {
        double random_number;
        ItemPointer tid;

        ResetExprContext(econtext); 

        random_number = sampler_random_fract(state->rand_state);

        if (state->use_batch_sampling)
            tid = index_samplenextbatch_tid(scandesc, random_number);
        else
            tid = index_samplenext_tid(scandesc, random_number);

    //    pswrctl_info->inv_prob *= ((ABTScanOpaque) scandesc->opaque)->inv_prob;
        {
            float8 inv_prob_cur = ((ABTScanOpaque) scandesc->opaque)->inv_prob;

            sampler_ctl->inv_prob = inv_prob_cur;
            ++sampler_ctl->num_samples_fetched;

            /* Legacy field for single-sampler paths and fallback code. */
            pswrctl_info->inv_prob = inv_prob_cur;
        }

        /*
         * Only the driver (outer) scan advances the global sample counter.
         * Inner scans track their own per-table counter, and sharing the global
         * counter would cause the inner scan to exhaust the total budget across
         * all tables rather than stopping after its own per-table sample_size.
         */
        {
            if (is_driver)
                ++pswrctl_info->num_samples_fetched;
        }
    
        /* warn the user if the rejection rate is too high */
        /*aqp_sample_scan_check_for_high_rejection_rate(&state->sss); */

        epoch_maybe_refresh();

        CHECK_FOR_INTERRUPTS();

        if (tid == NULL)
        {
            /* AB-tree rejection */
            REJECT_TUPLE();
        }

        /* fill data into the result slot */
        ExecClearTuple(slot);

        if (scandesc->xs_itup == NULL)
            elog(ERROR,
                 "swr_index-only scan did not return an index tuple");

        index_deform_tuple(scandesc->xs_itup,
                           scandesc->xs_itupdesc,
                           slot->tts_values,
                           slot->tts_isnull);
        ExecStoreVirtualTuple(slot);

        if (is_driver && pswrctl_info->want_partition_key)
        {
            /* TODO record the current partition key here */

            /* NOTE Only the AB-tree rejection must be excluded because
             * for the rest of the cases, we have fetched an index tuple,
             * and thus they will count towards the rejection rate in some
             * partition.
             *
             * For instance, let's say we fetched x = 100 and we have a
             * partition [99, 110). If it is rejected below, it will count
             * towards a rejected sample in that partition. If it is accepted
             * below, we will be able to get some valid value in the
             * aqp_approx_sum_internal_accum function so this will count as
             * accepted.
             *
             * To get the value, we should read it from the index tuple,
             * not the heap tuple.
             *
             * Also, we do deform the tuple below after the visibility
             * check, so maybe do that first over here. The visibility check
             * might be more expensive than deforming the index tuple.
             */

            uint64 i = pswrctl_info->next_kv_idx++;
            AttrNumber attno = pswrctl_info
                ->subplan_info[pswrctl_info->cur_plan_id].partition_attno;
            
            if (slot->tts_isnull[attno - 1])
            {
                elog(ERROR, "unhandled null partition key");
            }
            
            pswrctl_info->recorded_kv_pairs[i << 1] = slot->tts_values[attno - 1];

            if (aqp_optimization_strategy == AQP_OPTIMIZATION_STRATEGY_DP || 
                    aqp_optimization_strategy == AQP_OPTIMIZATION_STRATEGY_OPT_STRAT)
            {
                pswrctl_info->recorded_kv_pairs[(i << 1) + 1] = 0;
            }

            if (aqp_pswr_tree)
            {
                pswrctl_info->pswrctl->tree_level = 
                    ((ABTScanOpaque) scandesc->opaque)->locked_path->alp_level + 1;
        
                pswrctl_info->samples_height[i] = 
                    ((ABTScanOpaque) scandesc->opaque)->sample_height + 1;
            }
        }
        else if (is_driver)
        {
            pswrctl_info->pswrctl->tree_level =
                ((ABTScanOpaque) scandesc->opaque)->sample_height + 1;
        }
        
        /* 
         * See nodeIndexonlyscan.c for notes on the memory ordering effects
         * and why no lock or barrier is needed for checking visibility map.
         */
        if (!VM_ALL_VISIBLE(scandesc->heapRelation,
                            ItemPointerGetBlockNumber(tid),
                            &state->vmbuffer))
        {
            /* 
             * Not all heap tuples on this heap page is visible.
             * Go for the heap tuple to run visibility check.
             */
            if (!index_fetch_heap(scandesc, state->table_slot))
            {
                /* not visible */
                REJECT_TUPLE();
            }
            ExecClearTuple(state->table_slot);
            
            /* 
             * Same as nodeIndexonlyscan.c. We're not expecting non-MVCC
             * snapshots.
             */
            Assert(!scandesc->xs_heap_continue);
            if (scandesc->xs_heap_continue)
                elog(ERROR, "non-MVCC snapshots are not supported in index-only swr sample scan");
        }

        /* AB-tree never sets xs_hitup or ask us to recheck for index quals */
        Assert(scandesc->xs_hitup == NULL);
        Assert(!scandesc->xs_recheck);
        Assert(scandesc->xs_itup);
        
        /* 
         * XXX temporarily removed because I don't think page level
         * SI lock would make transactions with sampling serializable.
         *
         * Table level SI lock definitely works but maybe that's too
         * conservative.
         *
         * This requires further investigation.
         */
        /* take the SILock if we haven't read the tuple from heap */
        /*if (!tuple_from_heap)
            PredicateLockPage(scandesc->heapRelation,
                              ItemPointerGetBlockNumber(tid),
                              state->css.ss.ps.state->es_snapshot); */
        
        /* now evaluate the plan qual and projection if any */
        econtext->ecxt_scantuple = slot;
        if (qual == NULL || ExecQual(qual, econtext))
        {
            /* passes qual or no qual */
            pswrctl_info->rejected = false;
            if (projInfo)
            {
                return ExecProject(projInfo);
            }
            else
            {
                return slot;
            }
        }
        else
        {
            /* oops, rejected by the qual */
            REJECT_TUPLE();
        }
        
    }
#undef REJECT_TUPLE
    
    elog(ERROR, "unreachable");
    return NULL;
}

static void
aqp_swr_index_only_scan_end_new(CustomScanState *node)
{
    AQPIndexSampleScanState *state = (AQPIndexSampleScanState *) node;
    
    /* release visibility map buffer pin */
    if (state->vmbuffer != InvalidBuffer)
    {
        ReleaseBuffer(state->vmbuffer);
        state->vmbuffer = InvalidBuffer;
    }

    /* no need to call ExecFreeExprContext (see that for reason) */
    
    /* 
     * NOTE we have to set result tuple slot to something with
     * a valid ttsops, or ExecEndCustomScan() will blindly invoke
     * ExecClearTuple on a NULL pointer!
     *
     * Note that table_slot is cleared on every heap fetch, so we don't
     * need to clear it again here.
     *
     * We don't need to invoke clear either of the scan or the result tuple
     * though. It will be handled by ExecEndCustomScan.
     */
    if (!state->css.ss.ps.ps_ResultTupleSlot)
        state->css.ss.ps.ps_ResultTupleSlot =
            state->css.ss.ss_ScanTupleSlot;

    if (state->scandesc)
        index_endscan(state->scandesc);
    if (state->indexrel)
        index_close(state->indexrel, NoLock);
}

static void
aqp_swr_index_only_scan_rescan_new(CustomScanState *node)
{
    AQPIndexSampleScanState *state = (AQPIndexSampleScanState *) node;

    /* Let the PSWR dispatcher choose keys and perform index_rescan again. */
    state->css.ss.ps.ExecProcNode = aqp_pswr_scan_exec;

    if (state->vmbuffer != InvalidBuffer)
    {
        ReleaseBuffer(state->vmbuffer);
        state->vmbuffer = InvalidBuffer;
    }

    if (state->scandesc && state->scandesc->xs_heapfetch != NULL)
        table_index_fetch_reset(state->scandesc->xs_heapfetch);

    if (state->css.ss.ss_ScanTupleSlot)
        ExecClearTuple(state->css.ss.ss_ScanTupleSlot);
    if (state->table_slot)
        ExecClearTuple(state->table_slot);
    if (state->allnullslot)
        ExecClearTuple(state->allnullslot);
    /*
     * Reset the per-table sample counter so this inner scan can return its
     * allotted sample_size tuples again for the next outer tuple in a join.
     */
    if (state->pswrctl_info &&
        state->pswrctl_info->sampler_ctl &&
        state->pswrctl_sampler_id >= 0 &&
        state->pswrctl_sampler_id < state->pswrctl_info->nsamplers)
    {
        state->pswrctl_info->sampler_ctl[state->pswrctl_sampler_id]
            .num_samples_fetched = 0;
    }

    state->use_batch_sampling = false;
}
/*static void aqp_swr_index_only_scan_explain(CustomScanState *node,
                                            List *ancestors,
                                            ExplainState *es); */

static void
aqp_pswr_scan_begin(CustomScanState *node,
                    EState *estate,
                    int eflags)
{
    AQPProgressiveSampleScanState *psss =
        (AQPProgressiveSampleScanState *) node; 
    CustomScan *cscan = castNode(CustomScan, node->ss.ps.plan);
    AQPIndexSampleScanPrivate *issp0 =
        (AQPIndexSampleScanPrivate *) linitial(cscan->custom_private);
    AQPPSWRCtlInfo pswrctl_info;
    int nredundant_tupleslot;
    int i;

    aqp_swrscan_fetch_pswrctl_info_param(&psss->isss, issp0, estate);
    pswrctl_info = psss->isss.pswrctl_info;

    psss->isss.pswrctl_sampler_id =
        aqp_swrscan_append_sampler_ctl(pswrctl_info);
    pswrctl_info->pswr_driver_sampler_id = psss->isss.pswrctl_sampler_id;

    /*
     * At this point, ExecInitCustomScan() has:
     * 1) assigned the expression context for this node;
     * 2) opened the base relation (psss->css.ss.ss_CurrentRelation);
     * 3) initialized the scan tuple slot the type derived from the heap
     * relation and using the TTSOpsVirtual (which is not what we want!).
     * Because the ttsops is fixed at this point, we have to discard everything
     * starting from 3) until 5)....
     * 4) initialized the result tuple slot as a virtual tuple with the heap
     * relation relid as the expected varno in the tlist;
     * 5) initialized the plan qual (the additional plan qual not covered
     * by the index).
     */

    /*
     * We might have multiple subplans, we will may need multiple scan slots
     * for storing heap tuples (for swr) or index tuples (for swr indexonly).
     * In any case, the scan slot we have right now must be discarded.
     */
    Assert(psss->isss.css.ss.ss_ScanTupleSlot);

    /*
     * We shouldn't have a qual here since we didn't set one during planning.
     * Each subplan will have to initialize their own quals based on the
     * corresponding scan slots.
     */
    Assert(cscan->scan.plan.qual == NIL);
    Assert(!psss->isss.css.ss.ps.qual);
    
    /*
     * Projection info is also invalid so let's reset it here. Note that we
     * can't do anything about the memory leaks we have here for the ExprState
     * inside the project info. Again, each subplan would have their own
     * projection info set depending on the corresponding scan slots.
     */
    if (psss->isss.css.ss.ps.ps_ProjInfo)
    {
        pfree(psss->isss.css.ss.ps.ps_ProjInfo);
        psss->isss.css.ss.ps.ps_ProjInfo = NULL;
    }

    /* 
     * We can also remove the extra tuple slot from the tuple slot table. They
     * should be at the end of estate->es_tupleTable.
     */
    if (psss->isss.css.ss.ps.ps_ResultTupleSlot)
        nredundant_tupleslot = 2;
    else
        nredundant_tupleslot = 1;
    Assert(list_length(estate->es_tupleTable) >= nredundant_tupleslot);
    for (i = 0; i < nredundant_tupleslot; ++i)
    {
        TupleTableSlot *slot = (TupleTableSlot *) llast(estate->es_tupleTable);
        if (slot == psss->isss.css.ss.ps.ps_ResultTupleSlot ||
            slot == psss->isss.css.ss.ss_ScanTupleSlot)
        {
            estate->es_tupleTable = list_delete_last(estate->es_tupleTable);
            if (slot->tts_tupleDescriptor)
            {
               ReleaseTupleDesc(slot->tts_tupleDescriptor);
               slot->tts_tupleDescriptor = NULL;
            }
            if (!TTS_FIXED(slot))
            {
               if (slot->tts_values)
                   pfree(slot->tts_values);
               if (slot->tts_isnull)
                   pfree(slot->tts_isnull);
            }
            pfree(slot);
        }
        else
        {
            ereport(ERROR,
                    errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("AQPSWRScan failed to find the redundant tuple "
                           "slots made by ExecInitCustomScan()"));
        }
    }
    psss->isss.css.ss.ps.ps_ResultTupleSlot = NULL;
    psss->isss.css.ss.ss_ScanTupleSlot = NULL;


    /*
     * We have cleaned up the mess from ExecInitCustomScan(). We can now
     * initialize the per-subplan states. Note that we initialize swr and
     * swrindexonly subplans a bit differently.
     *
     * All swr scans produce the same heap tuples and should have matching
     * tuple descriptors, so we can share all scan slots, qual expr, prjection
     * info across them.
     *
     * The swrindexonly scans produces different index tuples and won't have
     * matching tuple descriptors, so we must initialize separate states for
     * them.
     */
    pswrctl_info->nsubplans = psss->nsubplans =
        list_length(cscan->custom_private);
    pswrctl_info->cur_plan_id = 0;
    if (psss->nsubplans == 0)
    {
        /* sanity check */
        elog(ERROR, "no valid index sample path found");
    }

    /* also set up the subplan_info for pswrctl here */
    pswrctl_info->subplan_info = palloc0(pswrctl_info->nsubplans *
        sizeof(AQPPSWRCtlSubplanInfoData));
    
    /* 
     * Save the original target list and restore it after we finish
     * initializing for better EXPLAIN output.
     */
    psss->orig_tlist = cscan->scan.plan.targetlist;
    psss->swr_scanslot = NULL;

    while (pswrctl_info->cur_plan_id < pswrctl_info->nsubplans)
    {
        ListCell *lc0;
        ListCell *lci;
        AQPProgressiveSampleScanPrivate *pssp;
        AQPProgressiveSampleScanSubplanState *subplan_state;

        lc0 = list_nth_cell(cscan->custom_private, 0);
        lci = list_nth_cell(cscan->custom_private, pswrctl_info->cur_plan_id);
        swap_ptr(lfirst(lc0), lfirst(lci));
        
        pssp = (AQPProgressiveSampleScanPrivate *) lfirst(lc0);
        Assert(AQPISSIsPSWRSubPlan(pssp));
        
        /* restore the plan info */
        cscan->scan.plan.targetlist = pssp->qptlist;
        cscan->scan.plan.qual = pssp->qpqual;
        cscan->custom_scan_tlist = pssp->indextlist;
        if (AQPISSIsIndexOnly(pssp))
        {
            aqp_swr_index_only_scan_begin_new((CustomScanState *) psss,
                                              estate,
                                              eflags);
        }
        else
        {
            aqp_swr_scan_begin_new((CustomScanState *) psss,
                                   estate,
                                   eflags);
        }

        /* save the plan states for the current subplan */
        subplan_state = &psss->subplan_states[pswrctl_info->cur_plan_id];
        Assert(psss->isss.css.ss.ps.scanopsset);
        Assert(psss->isss.css.ss.ps.scanopsfixed);
        subplan_state->scanslot = psss->isss.css.ss.ss_ScanTupleSlot;
        subplan_state->scan_descriptor = psss->isss.css.ss.ps.scandesc;
        subplan_state->scanops = psss->isss.css.ss.ps.scanops;
        /* Result tuple descriptor and result slots are shared. */
        subplan_state->projInfo = psss->isss.css.ss.ps.ps_ProjInfo;
        subplan_state->resultopsset = psss->isss.css.ss.ps.resultopsset;
        subplan_state->resultopsfixed = psss->isss.css.ss.ps.resultopsfixed;
        subplan_state->resultops = psss->isss.css.ss.ps.resultops;
        subplan_state->qual = psss->isss.css.ss.ps.qual;
        subplan_state->indexqual = psss->isss.indexqual;
        subplan_state->indexqualorig = psss->isss.indexqualorig;
        
        subplan_state->allnullslot = psss->isss.allnullslot;
        subplan_state->indexattr = pssp->indexattr;

        /*
         * The rest of the subplan states are only initialized for non-EXPLAIN
         * cases.
         */
        if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
        {
            subplan_state->indexrel = psss->isss.indexrel;
            subplan_state->scan_keys = psss->isss.scan_keys;
            subplan_state->num_scan_keys = psss->isss.num_scan_keys;
            subplan_state->runtime_keys = psss->isss.runtime_keys;
            subplan_state->num_runtime_keys = psss->isss.num_runtime_keys;
            subplan_state->scandesc = psss->isss.scandesc;
        }

        /* 
         * Save a copy of the first swr subplan's scan slot, projection info
         * and result slot ops. These are shared across all swr subplans.
         */
        if (!AQPISSIsIndexOnly(pssp) && psss->swr_scanslot == NULL)
        {
            psss->swr_scanslot = subplan_state->scanslot;
            psss->swr_scan_descriptor = subplan_state->scan_descriptor;
            psss->swr_scanops = subplan_state->scanops;
            psss->swr_projInfo = subplan_state->projInfo;
            psss->swr_resultopsset = subplan_state->resultopsset;
            psss->swr_resultopsfixed = subplan_state->resultopsfixed;
            psss->swr_resultops = subplan_state->resultops;
            psss->swr_allnullslot = subplan_state->allnullslot;
        }
        
        /* clean ups */
        psss->isss.css.ss.ss_ScanTupleSlot = NULL;
        psss->isss.css.ss.ps.scandesc = NULL;
        psss->isss.css.ss.ps.scanops = NULL;
        psss->isss.css.ss.ps.scanopsset = false;
        psss->isss.css.ss.ps.ps_ProjInfo = NULL;
        psss->isss.css.ss.ps.resultopsset = false;
        psss->isss.css.ss.ps.resultopsfixed = false;
        psss->isss.css.ss.ps.resultops = NULL;
        psss->isss.css.ss.ps.qual = NULL;
        psss->isss.indexqual = NULL;
        psss->isss.indexqualorig = NULL;
        psss->isss.indexrel = NULL;
        psss->isss.scan_keys = NULL;
        psss->isss.num_scan_keys = 0;
        psss->isss.runtime_keys = NULL;
        psss->isss.num_runtime_keys = 0;
        psss->isss.scandesc = NULL;

        /*psss->isss.p2matter = false;
        psss->isss.is_partition = false;
        psss->isss.allnullslot = NULL;
        */

        swap_ptr(lfirst(lc0), lfirst(lci));
        ++pswrctl_info->cur_plan_id;        
    }

    psss->selected_subplan_idx = -1;
    pswrctl_info->cur_plan_id = -1;
    cscan->scan.plan.targetlist = psss->orig_tlist;
    cscan->scan.plan.qual = NIL;
    cscan->custom_scan_tlist = NIL;

    if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
    {
        /* 
         * Explain only, we need to replace the Vars with invalid
         * references into the heap tuple with sample_prob() in the
         * query plan right now, before it is sent for printing.
         */
        if (aqp_is_in_explain_verbose)
            aqp_rewrite_sample_prob_dummy_var_for_explain((PlanState *) psss);
        return;
    }

    /* 
     * Save us a few cycles of calling from ExecCustomScan. Must be done after
     * we begin each subplans as they also set ExecProcNode.
     */
    psss->isss.css.ss.ps.ExecProcNode = aqp_pswr_scan_exec;

    if (issp0->repeatable_expr)
    {
        /* 
         * XXX There's no way for us to support this right now as online
         * insertion could change how tuples are mapped to random numbers,
         * even if we can reinitialize the random number generator using
         * the same seed. We'll need something that could freeze/lock
         * the index for the entire duration of the query, but that
         * would be too disruptive.
         */
        elog(ERROR, "pswr/swr sampler does not support REPEATABLE");
    }
    sampler_random_init_state(/*seed=*/(long) random(), psss->isss.rand_state);
}

static TupleTableSlot*
aqp_pswr_scan_exec(PlanState *node)
{
    AQPIndexSampleScanState *isss = (AQPIndexSampleScanState *) node;
    CustomScan *cscan = castNode(CustomScan, node->plan);
    AQPIndexSampleScanPrivate *issp0 =
        (AQPIndexSampleScanPrivate *) linitial(cscan->custom_private);
    AQPPSWRCtlInfo pswrctl_info;
    bool is_pswr = AQPISSIsPSWRSubPlan(issp0);
    bool use_batch_sampling = false;

    /*
    TimestampTz tz1, tz2;
    long s;
    int us;
    */

    pswrctl_info = isss->pswrctl_info;

    if (pswrctl_info->cur_plan_id == -1)
    {
        /* 
         * -1 means the pswrctl wants us to simply return a NULL slot without
         *  doing anything. This currently happens when we found at least one
         *  subplan saying the quals are not satisfiable during init calls, but
         *  TODO we may also use this to signal other cases where we find the
         *  result set to be empty.
         */
        return NULL;
    }

    
    /*
     * If this is pswr, check if we need to perform plan switching.
     */
    if (is_pswr)
    {
        AQPProgressiveSampleScanState *psss =
            (AQPProgressiveSampleScanState *) isss;
        int previously_selected_subplan_idx = psss->selected_subplan_idx;
        AQPProgressiveSampleScanPrivate *pssp;
        AQPProgressiveSampleScanSubplanState *subplan_state;
        
        psss->selected_subplan_idx = pswrctl_info->cur_plan_id;
        Assert(psss->selected_subplan_idx >= 0 &&
            psss->selected_subplan_idx < psss->nsubplans);
        isss->pswrctl_subplan_id = psss->selected_subplan_idx;

        if (previously_selected_subplan_idx != psss->selected_subplan_idx)
        {
            ListCell *lc0;
            ListCell *lci;

            lc0 = list_nth_cell(cscan->custom_private, 0);
            if (previously_selected_subplan_idx >= 0)
            {
                lci = list_nth_cell(cscan->custom_private,
                                    previously_selected_subplan_idx);
                swap_ptr(lfirst(lc0), lfirst(lci));
            }
                
            lci = list_nth_cell(cscan->custom_private,
                                psss->selected_subplan_idx);
            swap_ptr(lfirst(lc0), lfirst(lci));

            /* also update issp0 here */
            issp0 = (AQPIndexSampleScanPrivate *) lfirst(lc0);
            pssp = (AQPProgressiveSampleScanPrivate *) issp0;
            subplan_state = &psss->subplan_states[psss->selected_subplan_idx];

            cscan->scan.plan.targetlist = pssp->qptlist;
            cscan->scan.plan.qual = pssp->qpqual;
            cscan->custom_scan_tlist = pssp->indextlist;

            psss->isss.css.ss.ss_ScanTupleSlot = subplan_state->scanslot;
            psss->isss.css.ss.ps.scandesc = subplan_state->scan_descriptor;
            psss->isss.css.ss.ps.scanops = subplan_state->scanops;
            psss->isss.css.ss.ps.scanopsset = true;
            psss->isss.css.ss.ps.scanopsfixed = true;

            psss->isss.css.ss.ps.ps_ProjInfo = subplan_state->projInfo;
            psss->isss.css.ss.ps.resultopsset = subplan_state->resultopsset;
            psss->isss.css.ss.ps.resultopsfixed = subplan_state->resultopsfixed;
            psss->isss.css.ss.ps.resultops = subplan_state->resultops;
            /* result slot and result tuple descriptor are shared */

            psss->isss.css.ss.ps.qual = subplan_state->qual;
            psss->isss.indexqual = subplan_state->indexqual;
            psss->isss.indexqualorig = subplan_state->indexqualorig;
            psss->isss.indexrel = subplan_state->indexrel;
            /* TODO we'll get rid of these */
            psss->isss.scan_keys = subplan_state->scan_keys;
            psss->isss.num_scan_keys = subplan_state->num_scan_keys;
            psss->isss.runtime_keys = subplan_state->runtime_keys;
            psss->isss.num_runtime_keys = subplan_state->num_runtime_keys;
            psss->isss.scandesc = subplan_state->scandesc;

            psss->isss.allnullslot = subplan_state->allnullslot;
        }
    }

    /* 
     * At this point, issp0 should point to the selected plan in pswr,
     * or the only plan itself if this is swr/swrindexonly.
     *
     * isss should have all the state objects swapped into the isss portion.
     *
     * This is the place to perform rescan and set up the correct execution
     * functions.
     */
    //Assert(pswrctl_info->num_samples_fetched == 0);
    if (is_pswr)
        aqp_swrscan_set_driver_sampler(pswrctl_info,
                                    isss->pswrctl_sampler_id);

    if (aqp_batch_sampling && is_pswr &&
        !pswrctl_info->pswrctl->do_switch_to_uniform)
    {
        /*
         * Batch sampling is only correct for one PSWR scan node.  Multiple
         * PSWR samplers in a join currently share pswrctl/subplan_info state
         * in a way that confuses table samplers with same-table index
         * candidates, so fail fast instead of returning a wrong estimate.
         * After fallback to uniform, batch sampling uses the normal SWR 
         * path below.
         */
        if (pswrctl_info->batch_sampling_pswr_node == NULL)
            pswrctl_info->batch_sampling_pswr_node = (void *) node;
        else if (pswrctl_info->batch_sampling_pswr_node != (void *) node)
            elog(ERROR,
                 "batch_sampling does not support joins with multiple PSWR samplers");

        {
            int sid = isss->pswrctl_subplan_id;

            if (pswrctl_info->subplan_info &&
                sid >= 0 &&
                sid < pswrctl_info->nsubplans &&
                sid == pswrctl_info->cur_plan_id &&
                pswrctl_info->sampler_ctl[isss->pswrctl_sampler_id].is_driver)
                use_batch_sampling = true;
        }
    }
    isss->use_batch_sampling = use_batch_sampling;

    if (!use_batch_sampling)
    {
        if (!is_pswr)
        {
            /*
             * Multi-sampler SWR (join) in new agg mode: drive each sampler
             * with its own scan keys/runtime keys instead of one global keyset.
             */
            if (isss->num_runtime_keys > 0)
            {
                ExprContext *econtext = isss->runtime_context;
                ResetExprContext(econtext);
                ExecIndexEvalRuntimeKeys(econtext,
                                         isss->runtime_keys,
                                         isss->num_runtime_keys);
                isss->runtime_keys_ready = true;
            }

            isss->scandesc->numberOfKeys = isss->num_scan_keys;
            if (!AQPISSIsIndexOnly(issp0))
                isss->scandesc->xs_want_itup = false;

            index_rescan(isss->scandesc,
                         isss->scan_keys,
                         isss->num_scan_keys,
                         /*orderbys=*/NULL,
                         /*norderbys=*/0);
        }
        else
        {
        isss->scandesc->numberOfKeys = pswrctl_info->n_current_scan_keys;
        if (!AQPISSIsIndexOnly(issp0))
        {
            /* 
             * If this is not index only, we normally do not need to request
             * the index tuple on the leaf level when abtsampletuple returns.
             * However, this will be needed if we want to record all the partition
             * keys during sampling during optimization phases.
             *
             * Make sure to set it back if we don't want that since keeping the
             * itup will require the tree to keep a pin on the corresponding leaf
             * page.
             */
            if (pswrctl_info->want_partition_key)
                isss->scandesc->xs_want_itup = true;
            else
                isss->scandesc->xs_want_itup = false;
        }       
        /*
        if (pswrctl_info->pswrctl->cur_phase > 0 
            && pswrctl_info->pswrctl->cur_partition != 0)
        {
            int height = ((ABTScanOpaque) isss->scandesc->opaque)->locked_path->alp_level;
          
            if (height <= pswrctl_info->height_min)
                pswrctl_info->height_min = height;
            if (height >= pswrctl_info->height_max)
                pswrctl_info->height_max = height;
            pswrctl_info->height_avg += height;
        }
            
        tz1 = GetCurrentTimestamp();
        */

        index_rescan(isss->scandesc,
                     pswrctl_info->current_scan_keys,
                     pswrctl_info->n_current_scan_keys,
                     /*orderbys=*/NULL,
                     /*norderbys=*/0);
    
        /*
        tz2 = GetCurrentTimestamp();
        TimestampDifference(tz1, tz2, &s, &us);
        
        if (pswrctl_info->pswrctl->cur_phase > 0)
        {
            float8 time = s * 1e3 + us * 1e-3;
            if (time <= pswrctl_info->time_min)
                pswrctl_info->time_min = time;
            if (time >= pswrctl_info->time_max)
                pswrctl_info->time_max = time;
            pswrctl_info->time_avg += time;
        }
        */
        }
    }
    else
    {
        ABTScanOpaque so = (ABTScanOpaque) isss->scandesc->opaque;

        /* if (pswrctl_info->pswrctl->cur_phase == 0 
            && pswrctl_info->pswrctl->cur_partition == 0) */
        if (pswrctl_info->pswrctl->transstate[0].flags == 
            AQP_LEAF_PAGE_STATISTICS)
        {

            isss->scandesc->numberOfKeys = pswrctl_info->n_current_scan_keys;

            if (!AQPISSIsIndexOnly(issp0))
            {
                /* 
                * If this is not index only, we normally do not need to request
                * the index tuple on the leaf level when abtsampletuple returns.
                * However, this will be needed if we want to record all the partition
                * keys during sampling during optimization phases.
                *
                * Make sure to set it back if we don't want that since keeping the
                * itup will require the tree to keep a pin on the corresponding leaf
                * page.
                */
                if (pswrctl_info->want_partition_key)
                    isss->scandesc->xs_want_itup = true;
                else
                    isss->scandesc->xs_want_itup = false;
            }
        
            index_rescan(isss->scandesc,
                        pswrctl_info->current_scan_keys,
                        pswrctl_info->n_current_scan_keys,
                        /*orderbys=*/NULL,
                        /*norderbys=*/0);

            _abt_build_lpath_batch(isss->scandesc);
            aqp_allocate_samples(so, pswrctl_info->sample_size);
            pswrctl_info->pswrctl->initial_phase_sample_size = so->initial_sample_size;
            pswrctl_info->pswrctl->cur_phase_sample_size = so->initial_sample_size;
        }

        if (pswrctl_info->pswrctl->continue_descent_dp == 1)
            aqp_pswr_set_new_partitions(isss->scandesc, pswrctl_info);
        if (pswrctl_info->pswrctl->continue_descent == 1)
            aqp_pswr_continue_descent(isss->scandesc, pswrctl_info);
        if (pswrctl_info->pswrctl->continue_descent != -1)
            aqp_pswr_batch_sampling_rescan(so, pswrctl_info);
    }
   
    isss->css.ss.ps.ExecProcNode = AQPISSIsIndexOnly(issp0) ?
        aqp_swr_index_only_scan_exec_new :
        aqp_swr_scan_exec_new;
    return isss->css.ss.ps.ExecProcNode(node);
}

static void
aqp_pswr_scan_end(CustomScanState *node)
{
    AQPProgressiveSampleScanState *psss =
        (AQPProgressiveSampleScanState *) node;
    CustomScan *cscan = castNode(CustomScan, node->ss.ps.plan);
    int i;
    
    /*
     * It's not entirely clear whether we actually need to swap the subplan
     * back in the original order, but let's do that just in case.
     */

    if (psss->selected_subplan_idx > 0)
    {
        ListCell *lc0;
        ListCell *lci;

        lc0 = list_nth_cell(cscan->custom_private, 0);
        lci = list_nth_cell(cscan->custom_private, psss->selected_subplan_idx);
        swap_ptr(lfirst(lc0), lfirst(lci));
    }
    psss->selected_subplan_idx = -1;

    if (psss->isss.vmbuffer != InvalidBuffer)
    {
        ReleaseBuffer(psss->isss.vmbuffer);
        psss->isss.vmbuffer = InvalidBuffer;
    }
    
    if (psss->swr_scanslot != NULL)
        ExecClearTuple(psss->swr_scanslot);
    
    for (i = 0; i < psss->nsubplans; ++i)
    {
        AQPProgressiveSampleScanSubplanState *subplan_state =
            &psss->subplan_states[i];
        
        if (subplan_state->scanslot != psss->swr_scanslot)
            ExecClearTuple(subplan_state->scanslot);
        if (subplan_state->scandesc)
            index_endscan(subplan_state->scandesc);
        if (subplan_state->indexrel)
            index_close(subplan_state->indexrel, NoLock);
    }

    /* 
     * NOTE we have to set result tuple slot to something with
     * a valid ttsops, or ExecEndCustomScan() will blindly invoke
     * ExecClearTuple on a NULL pointer!
     *
     * (This means there is one slot that could be cleared for three times in
     * the worst case).
     *
     */
    if (psss->isss.css.ss.ps.ps_ResultTupleSlot == NULL)
    {
        psss->isss.css.ss.ps.ps_ResultTupleSlot =
            psss->subplan_states[0].scanslot;
    }
    if (psss->isss.css.ss.ss_ScanTupleSlot == NULL)
    {
        psss->isss.css.ss.ss_ScanTupleSlot = psss->subplan_states[0].scanslot;
    }

}

static void
aqp_pswr_scan_rescan(CustomScanState *node)
{
    AQPIndexSampleScanState *state = (AQPIndexSampleScanState *) node;

    state->css.ss.ps.ExecProcNode = aqp_pswr_scan_exec;

    state->use_batch_sampling = false;

    //elog(ERROR, "aqp_pswr_scan_rescan not implemented yet");
}

static void 
aqp_pswr_continue_descent(IndexScanDesc scan,
                          AQPPSWRCtlInfo pswrctl_info)
{
    AQPPSWRControlState *pswrctl = pswrctl_info->pswrctl;
    AQPApproxAggTransState *transstate = pswrctl->transstate;
    ABTScanOpaque	so = (ABTScanOpaque) scan->opaque;
    bool found = false;
    uint32 index;
    AttrNumber partition_attno;

    Assert(pswrctl_info->cur_plan_id >= 0);
    Assert(pswrctl_info->cur_plan_id < pswrctl_info->nsubplans);

    partition_attno =
        pswrctl_info->subplan_info[pswrctl_info->cur_plan_id].partition_attno;

    if (partition_attno == InvalidAttrNumber)
        elog(ERROR, "cannot continue PSWR descent without a partition attribute");

    for (int i = 0; i < pswrctl->cur_phase_n_partitions; i++)
    {
        index = transstate->sort_idx[i];
        if (index == 0 || transstate->done[index] == 1)
            continue;

        transstate->done[index] = 1;
        found = _abt_continue_descent(scan, transstate->sort_idx[i],
                                    partition_attno,
                                    aqp_batch_sampling_dp);
        if (!found)
            continue;
        else
            break;
    }

    if (!found)
    {
        pswrctl->continue_descent = -1;
        pswrctl_info->sample_size = 0;
        pswrctl->cur_partition--;
    }
    else
    {
        transstate->dp_start_idx = pswrctl->cur_phase_n_partitions;
        transstate->split_idx = index;
        transstate->used[index] = 1;
        pswrctl->cur_descent_partitions = so->curr_partitions;
        transstate->var_part[index] = transstate->var_part[index] 
            * (1 / (float8) (so->curr_partitions + 1)) 
            * (1 / (float8) (so->curr_partitions + 1)) 
            - transstate->var_part[index];
        transstate->weights[index] = (float8) so->curr_partitions 
            / (so->curr_partitions + 1) * transstate->weights[index];

        if (aqp_batch_sampling_dp)
            pswrctl->tree_level = so->curr_level;

        pswrctl->transstate->var_0 = transstate->var_total;
        pswrctl->descent_try_sample_size = 1;

        /* elog(INFO, "start_id: %d, split_id: %d, nintvls: %lu", 
             transstate->dp_start_idx, transstate->split_idx, 
             pswrctl->cur_descent_partitions); */
    }
    pswrctl->cur_phase_n_partitions = so->total_partitions;
}


static void 
aqp_allocate_samples(ABTScanOpaque so, 
                     uint64 sample_size)
{
    /*
    uint64 initial_sample_size;
    
    initial_sample_size = so->statis[0].sample_size;

    for (int i = 1; i < so->total_partitions; ++i)
    {
        double weight = so->statis[i].weight;
        uint64 per_sample = weight * 
            (sample_size - so->statis[0].sample_size);

        so->statis[i].sample_size = Max(30, per_sample); 

        initial_sample_size += so->statis[i].sample_size;
    }
    so->initial_sample_size = initial_sample_size;
    */
    so->initial_sample_size = so->statis[0].sample_size + 
                            aqp_subtree_sample_size * (so->total_partitions - 1);

    elog(INFO, "inital_partitions:%d", so->total_partitions);
    /* for (int i = 0; i < so->locked_path->alp_nsubtrees; i++)
    {
        elog(INFO, "idx = %u, size = %lu", so->locked_path->alp_subtreerefs[i].idx,
                so->locked_path->alp_subtree_aggs[i]);
    } */
}

static void
aqp_pswr_set_new_partitions(IndexScanDesc scan,
                            AQPPSWRCtlInfo pswrctl_info)
{   
    
    ABTScanOpaque	so = (ABTScanOpaque) scan->opaque;
    AQPPSWRControlState *pswrctl = pswrctl_info->pswrctl;
    ABTLockedPath lpath = so->locked_path;
    ABTLockedPathSubtreeRefDes *subtreerefdes_f;
    ABTLockedPathSubtreeRefDes *subtreerefdes_b;
    ABTLockedPathSubtreeRefDes subtreerefdes_new;

    int *prev_idx = pswrctl->dp_prev_idx;
    uint32 start_id = pswrctl->transstate->dp_start_idx;
    uint64 k = 0;
    uint64 i = 1;

    while(k < pswrctl->dp_k_partitions)
    {
        uint64 j;
        uint64 agg = 0;

        j = prev_idx[k];

        subtreerefdes_f = &lpath->alp_subtreerefdess[start_id - 2 + i];
        subtreerefdes_b = &lpath->alp_subtreerefdess[start_id - 2 + j];

        if (subtreerefdes_f->buf != InvalidBuffer)
        {
            subtreerefdes_new.buf = subtreerefdes_f->buf;
            for (int m = start_id - 2 + i; m <= start_id - 2 + j; m++)
                agg += lpath->alp_subtreerefdess[m].agg;
            subtreerefdes_new.agg = agg;
        }
        else
            subtreerefdes_new.buf = InvalidBuffer;
        subtreerefdes_new.blkno = subtreerefdes_f->blkno;
        subtreerefdes_new.minoff = subtreerefdes_f->minoff;
        subtreerefdes_new.maxoff = subtreerefdes_b->maxoff;
        subtreerefdes_new.level = subtreerefdes_f->level;
        subtreerefdes_new.is_leaf = false;
        subtreerefdes_new.in_use = true;
        subtreerefdes_new.is_splited = true;

        /*
        elog(INFO, "min_blkno: %u, max_blkno: %u", subtreerefdes_f->blkno, subtreerefdes_b->blkno);
        elog(INFO, "i: %lu, j: %lu, min: %d, max: %d", i, j, subtreerefdes_new.minoff, subtreerefdes_b->maxoff); 
        */

        lpath->alp_subtreerefdess[so->total_partitions - 1 + k] 
            = subtreerefdes_new;

        i = j + 1;
        ++k;
    }
    
    so->total_partitions += pswrctl->dp_k_partitions;
    pswrctl->cur_phase_n_partitions = so->total_partitions;

    if (pswrctl->continue_descent == 1)
    {
        pswrctl->cur_partition = pswrctl->cur_phase_n_partitions;
        aqp_pswr_continue_descent(scan, pswrctl_info);
        pswrctl->continue_descent = 2;
    }
    else
        pswrctl->continue_descent = -1;

    pswrctl->continue_descent_dp = 0;
    pswrctl_info->sample_size = 0;
}

static void
aqp_pswr_batch_sampling_rescan(ABTScanOpaque so,
                               AQPPSWRCtlInfo pswrctl_info)
{
    uint64  index = pswrctl_info->pswrctl->cur_partition;

    if (so->locked_path == NULL)
        elog(ERROR, "PSWR batch sampling requires an initialized AB-tree locked path");
    if (so->statis == NULL || index >= so->total_partitions)
        elog(ERROR, "PSWR batch sampling partition is out of range");

    so->split_partition = true;
    
    pswrctl_info->pswrctl->cur_phase_n_partitions = so->total_partitions;

    /* if (!subtreerefdes->in_use)
        pswrctl_info->pswrctl->transstate->used[index] = 1; */
    /*
    if (pswrctl_info->pswrctl->cur_phase == 0)
    {
        pswrctl_info->sample_size = so->statis[index].sample_size;
        so->sample_size = so->statis[index].sample_size;
    }
    else
    {
        pswrctl_info->sample_size = 
            pswrctl_info->pswrctl->gpsa_opt_sample_size[index];
        so->sample_size = 
            pswrctl_info->pswrctl->gpsa_opt_sample_size[index];
    }
    so->idx = so->statis[index].subtree_idx;
    */

    if (pswrctl_info->pswrctl->cur_phase == 0)
    {
        if (index == 0)
        {
            pswrctl_info->pswrctl->leaf_page_sample_size = 
                so->statis[index].sample_size;
            pswrctl_info->sample_size = so->statis[index].sample_size;
            so->sample_size = so->statis[index].sample_size;
        }
        else
        {
            pswrctl_info->sample_size = aqp_subtree_sample_size;
            so->sample_size = aqp_subtree_sample_size;
        }
    }
    else
    {
        if (pswrctl_info->pswrctl->descent_try_sample_size == 1)
        {
            pswrctl_info->sample_size = aqp_subtree_sample_size;
            so->sample_size = aqp_subtree_sample_size;
        }
        else
        {
            uint64 sample_size;

            if (pswrctl_info->pswrctl->progressive_current_phase_enabled &&
                !pswrctl_info->pswrctl->do_switch_to_uniform)
                sample_size =
                    pswrctl_info->pswrctl->partition_round_budget[index];
            else
                sample_size =
                    pswrctl_info->pswrctl->gpsa_opt_sample_size[index];

            pswrctl_info->sample_size = sample_size;
            so->sample_size = sample_size;
        }

        /*elog(INFO, "%lu", so->sample_size);*/
    }


    so->idx = index;

    if (so->idx == 0)
    {
        so->idx_leaf = 0;
        so->currPos.itemIndex = 0;
    }
}

static void
aqp_swrscan_fetch_pswrctl_info_param(AQPIndexSampleScanState *state,
                                     AQPIndexSampleScanPrivate *private,
                                     EState *estate)
{
    /* 
     * We may also grab the pswrctl parameter if any. This is common
     * for all plan/subplan nodes. 
     */
    if (private->pswrctl_info_param && !state->pswrctl_info)
    {
        int paramid = private->pswrctl_info_param->paramid;
        EState *estate = state->css.ss.ps.state;
        Assert(!estate->es_param_exec_vals[paramid].isnull);
        state->pswrctl_info = (AQPPSWRCtlInfo) DatumGetPointer(
            estate->es_param_exec_vals[paramid].value);
    }
    Assert(state->pswrctl_info != NULL);
}

static AQPPSWRCtlSubplanInfo
aqp_swrscan_append_pswrctl_subplan_info(AQPPSWRCtlInfo pswrctl_info)
{
    int old_nsubplans = pswrctl_info->nsubplans;

    if (pswrctl_info->subplan_info == NULL)
    {
        pswrctl_info->subplan_info =
            palloc0(sizeof(AQPPSWRCtlSubplanInfoData));
    }
    else
    {
        pswrctl_info->subplan_info = repalloc(
            pswrctl_info->subplan_info,
            sizeof(AQPPSWRCtlSubplanInfoData) * (old_nsubplans + 1));
        memset(&pswrctl_info->subplan_info[old_nsubplans],
               0,
               sizeof(AQPPSWRCtlSubplanInfoData));
    }

    pswrctl_info->nsubplans = old_nsubplans + 1;
    return &pswrctl_info->subplan_info[old_nsubplans];
}

static int
aqp_swrscan_append_sampler_ctl(AQPPSWRCtlInfo pswrctl_info)
{
    int old_nsamplers = pswrctl_info->nsamplers;
    AQPTableSamplerCtl ctl;

    if (pswrctl_info->sampler_ctl == NULL)
        pswrctl_info->sampler_ctl =
            palloc0(sizeof(AQPTableSamplerCtlData));
    else
        pswrctl_info->sampler_ctl =
            repalloc(pswrctl_info->sampler_ctl,
                     sizeof(AQPTableSamplerCtlData) * (old_nsamplers + 1));

    ctl = &pswrctl_info->sampler_ctl[old_nsamplers];
    memset(ctl, 0, sizeof(AQPTableSamplerCtlData));
    ctl->inv_prob = 1.0;

    pswrctl_info->nsamplers = old_nsamplers + 1;
    return old_nsamplers;
}

static void
aqp_swrscan_set_driver_sampler(AQPPSWRCtlInfo pswrctl_info, int sampler_id)
{
    int i;

    pswrctl_info->driver_sampler_id = sampler_id;

    if (!pswrctl_info->sampler_ctl)
        return;

    for (i = 0; i < pswrctl_info->nsamplers; ++i)
        pswrctl_info->sampler_ctl[i].is_driver = (i == sampler_id);
}

static AQPTableSamplerCtl
aqp_swrscan_get_sampler_ctl(AQPIndexSampleScanState *state)
{
    AQPPSWRCtlInfo pswrctl_info = state->pswrctl_info;

    if (!pswrctl_info ||
        !pswrctl_info->sampler_ctl ||
        state->pswrctl_sampler_id < 0 ||
        state->pswrctl_sampler_id >= pswrctl_info->nsamplers)
        elog(ERROR, "invalid PSWR sampler id %d",
             state->pswrctl_sampler_id);

    return &pswrctl_info->sampler_ctl[state->pswrctl_sampler_id];
}

static void
aqp_index_build_scankeys(PlanState *planstate, Relation index,
                         List *quals, AQPPSWRCtlSubplanInfo subplan_info)
{
    int nquals;
    int indnkeyatts;
    ScanKey scan_keys;
    ScanKey lb;
    ScanKey ub;

    /* This is the max size we'll ever need. */
    indnkeyatts = IndexRelationGetNumberOfKeyAttributes(index);
	nquals = list_length(quals);
    /* 
     * scan_keys is a bad choice of and confusing name, which actually stores
     * all the additional index quals that the index is supposed to check.
     * However, it is named in this way because the original function
     * ExecIndexBuildScanKeys which we base this function on uses the following
     * array to store the entire scan key and it is very convenient for us to
     * reuse that name in the following impl.
     */
	scan_keys =
        (ScanKey) palloc((nquals + 2 + 2 * indnkeyatts) * sizeof(ScanKeyData));
    lb = scan_keys + nquals;
    ub = lb + 1 + indnkeyatts;

    aqp_index_build_scankeys_impl(planstate, index, quals, subplan_info,
                                  lb, ub, scan_keys);

    pfree(scan_keys);
}

/*
 * aqp_index_build_scankeys_impl is adapted from ExecIndexBuildScanKeys.
 *
 * For now, we only support case 1, with the rest temporarily disabled.
 *
 * Suppose the index keys are (x1, x2, ... xn)
 * We expect the index qual to be in the following form:
 *
 * x1 = c1 and x2 = c2 and ... xl = cl and xl+1 >/>= lb, xl+1 </<= ub.
 *
 * XXX There isn't a reason why we couldn't make xl+1 to be a row comparison
 * with row comparison lb and ub, but it can be a bit clumbsy to write so I'll
 * defer this until a later time (when we find a valid use case).
 *
 * When we perform partitioning, we only partition on l.
 *
 * TODO we could potentially support group-by by changing the values of x1, ...
 * xl.
 *
 * We additionally perform some of the work done in the
 * index preprocess so that we can always produce an array of two ScanKeys, [0]
 * representing the lower bound and [1] representing the upper bound (both
 * of which may be null, indicating no bound specified in the query).
 *
 * It also happens to be a convenient place for us to look up the comparison
 * operators which will be needed by pswrctl.
 *
 *
 * The following is the original description of ExecIndexBuildScanKeys
 *		Build the index scan keys from the index qualification expressions
 *
 * The index quals are passed to the index AM in the form of a ScanKey array.
 * This routine sets up the ScanKeys, fills in all constant fields of the
 * ScanKeys, and prepares information about the keys that have non-constant
 * comparison values.  We divide index qual expressions into five types:
 *
 * 1. Simple operator with constant comparison value ("indexkey op constant").
 * For these, we just fill in a ScanKey containing the constant value.
 *
 * 2. Simple operator with non-constant value ("indexkey op expression").
 * For these, we create a ScanKey with everything filled in except the
 * expression value, and set up an IndexRuntimeKeyInfo struct to drive
 * evaluation of the expression at the right times.
 *
 * 3. RowCompareExpr ("(indexkey, indexkey, ...) op (expr, expr, ...)").
 * For these, we create a header ScanKey plus a subsidiary ScanKey array,
 * as specified in access/skey.h.  The elements of the row comparison
 * can have either constant or non-constant comparison values.
 *
 * 4. ScalarArrayOpExpr ("indexkey op ANY (array-expression)").  If the index
 * supports amsearcharray, we handle these the same as simple operators,
 * setting the SK_SEARCHARRAY flag to tell the AM to handle them.  Otherwise,
 * we create a ScanKey with everything filled in except the comparison value,
 * and set up an IndexArrayKeyInfo struct to drive processing of the qual.
 * (Note that if we use an IndexArrayKeyInfo struct, the array expression is
 * always treated as requiring runtime evaluation, even if it's a constant.)
 *
 * 5. NullTest ("indexkey IS NULL/IS NOT NULL").  We just fill in the
 * ScanKey properly.
 *
 * This code is also used to prepare ORDER BY expressions for amcanorderbyop
 * indexes.  The behavior is exactly the same, except that we have to look up
 * the operator differently.  Note that only cases 1 and 2 are currently
 * possible for ORDER BY.
 *
 * Input params are:
 *
 * planstate: executor state node we are working for
 * index: the index we are building scan keys for
 * quals: indexquals (or indexorderbys) expressions
 * *runtimeKeys: ptr to pre-existing IndexRuntimeKeyInfos, or NULL if none
 * *numRuntimeKeys: number of pre-existing runtime keys
 *
 * Output params are:
 *
 * *scanKeys: receives ptr to array of ScanKeys
 * *numScanKeys: receives number of scankeys
 * *runtimeKeys: receives ptr to array of IndexRuntimeKeyInfos, or NULL if none
 * *numRuntimeKeys: receives number of runtime keys
 * *arrayKeys: receives ptr to array of IndexArrayKeyInfos, or NULL if none
 * *numArrayKeys: receives number of array keys
 *
 * Caller may pass NULL for arrayKeys and numArrayKeys to indicate that
 * IndexArrayKeyInfos are not supported.
 */
static void
aqp_index_build_scankeys_impl(PlanState *planstate, Relation index,
                              List *quals, AQPPSWRCtlSubplanInfo subplan_info,
                              ScanKey lb,
                              ScanKey ub,
                              ScanKey scan_keys)
{
	ListCell   *qual_cell;
	int			nquals;
	int			indnkeyatts;
    int         nlb;
    int         nub;
    int         partition_attno;
    int         i,
                ilmost,
                irmost,
                iscankeys,
                j,
                nremquals,
                attno;
    
    indnkeyatts = IndexRelationGetNumberOfKeyAttributes(index);
    
    /* Default is to assume the underlying sample space is not empty. */
    subplan_info->emptyres = false;
    
    nquals = 0;
    nlb = 1;
    nub = 1;
    /* 
     * Slot 0 in both lb and ub are set to these dummy values so that we can
     * treat it as if we have matched a prefix of equality conditions on the
     * index keys with length 0.
     */
    lb[0].sk_attno = 0;
    lb[0].sk_strategy = BTEqualStrategyNumber;
    ub[0].sk_attno = 0;
    ub[0].sk_strategy = BTEqualStrategyNumber;

	/*
	 * for each opclause in the given qual, convert the opclause into a single
	 * scan key
	 */
	foreach(qual_cell, quals)
	{
		Expr	   *clause = (Expr *) lfirst(qual_cell);
		Oid			opno;		/* operator's OID */
		RegProcedure opfuncid;	/* operator proc id used in scan */
		Oid			opfamily;	/* opfamily of index column */
		int			op_strategy;	/* operator's strategy number */
		Oid			op_lefttype;	/* operator's declared input types */
		Oid			op_righttype;
		Expr	   *leftop;		/* expr on lhs of operator */
		Expr	   *rightop;	/* expr on rhs ... */
		AttrNumber	varattno;	/* att number used in scan */
		Datum		scanvalue;  /* the rhs of the value */
        bool        used_as_bound;

		if (IsA(clause, OpExpr))
		{
			/* indexkey op const or indexkey op expression */
			opno = ((OpExpr *) clause)->opno;
			opfuncid = ((OpExpr *) clause)->opfuncid;

			/*
			 * leftop should be the index key Var, possibly relabeled
			 */
			leftop = (Expr *) get_leftop(clause);

			if (leftop && IsA(leftop, RelabelType))
				leftop = ((RelabelType *) leftop)->arg;

			Assert(leftop != NULL);

			if (!(IsA(leftop, Var) &&
				  ((Var *) leftop)->varno == INDEX_VAR))
				elog(ERROR, "indexqual doesn't have key on left side");

			varattno = ((Var *) leftop)->varattno;
			if (varattno < 1 || varattno > indnkeyatts)
				elog(ERROR, "bogus index qualification");

			/*
			 * We have to look up the operator's strategy number.  This
			 * provides a cross-check that the operator does match the index.
			 */
			opfamily = index->rd_opfamily[varattno - 1];

			get_op_opfamily_properties(opno, opfamily, /*isorderby =*/false,
									   &op_strategy,
									   &op_lefttype,
									   &op_righttype);

			/*
			 * rightop is the constant or variable comparison value
			 */
			rightop = (Expr *) get_rightop(clause);

			if (rightop && IsA(rightop, RelabelType))
				rightop = ((RelabelType *) rightop)->arg;

			Assert(rightop != NULL);

			if (IsA(rightop, Const))
			{
				/* OK, simple constant comparison value */
				scanvalue = ((Const *) rightop)->constvalue;
                if (((Const *) rightop)->constisnull)
                {
                    subplan_info->emptyres = true;
                    return ;
                }
			}
			else
			{
				/* Need to treat this one as a runtime key */

                /* disallowed */
                subplan_info->valid = false; 
                return;
                /*
				if (n_runtime_keys >= max_runtime_keys)
				{
					if (max_runtime_keys == 0)
					{
						max_runtime_keys = 8;
						runtime_keys = (IndexRuntimeKeyInfo *)
							palloc(max_runtime_keys * sizeof(IndexRuntimeKeyInfo));
					}
					else
					{
						max_runtime_keys *= 2;
						runtime_keys = (IndexRuntimeKeyInfo *)
							repalloc(runtime_keys, max_runtime_keys * sizeof(IndexRuntimeKeyInfo));
					}
				}
				runtime_keys[n_runtime_keys].scan_key = this_scan_key;
				runtime_keys[n_runtime_keys].key_expr =
					ExecInitExpr(rightop, planstate);
				runtime_keys[n_runtime_keys].key_toastable =
					TypeIsToastable(op_righttype);
				n_runtime_keys++;
				scanvalue = (Datum) 0;
                */
			}
            
            used_as_bound = aqp_merge_current_qual_into_bounds(index,
                lb, &nlb, ub, &nub, &subplan_info->emptyres,
                op_strategy, op_righttype, opfuncid,
                ((OpExpr *) clause)->inputcollid, varattno, scanvalue);

            /* 
             * Return early if we can already determine the sample space is
             * empty. 
             */
            if (subplan_info->emptyres)
                return;
            
            if (!used_as_bound) {
                /* 
                 * An index qual that can't be considered as part of either
                 * upper bound or lower bound, maybe because the last scankey
                 * on the prefix has a non-equality comparison; we are skipping
                 * the next key on the prefix; or there's missing suitable
                 * cross-type comparators between the two scankey arguments.
                 */
                ScanKeyEntryInitialize(&scan_keys[nquals++],
                                       0,
                                       varattno,
                                       op_strategy,
                                       op_righttype,
                                       ((OpExpr *) clause)->inputcollid,
                                       opfuncid,
                                       scanvalue);
            }
		}
#if 0 /* disabled */
		else if (IsA(clause, RowCompareExpr))
		{
			/* (indexkey, indexkey, ...) op (expression, expression, ...) */
			RowCompareExpr *rc = (RowCompareExpr *) clause;
			ScanKey		first_sub_key;
			int			n_sub_key;
			ListCell   *largs_cell;
			ListCell   *rargs_cell;
			ListCell   *opnos_cell;
			ListCell   *collids_cell;
            bool        used_as_bound;

			Assert(!isorderby);

			first_sub_key = (ScanKey)
				palloc(list_length(rc->opnos) * sizeof(ScanKeyData));
			n_sub_key = 0;

			/* Scan RowCompare columns and generate subsidiary ScanKey items */
			forfour(largs_cell, rc->largs, rargs_cell, rc->rargs,
					opnos_cell, rc->opnos, collids_cell, rc->inputcollids)
			{
				ScanKey		this_sub_key = &first_sub_key[n_sub_key];
				int			flags = SK_ROW_MEMBER;
				Datum		scanvalue;
				Oid			inputcollation;

				leftop = (Expr *) lfirst(largs_cell);
				rightop = (Expr *) lfirst(rargs_cell);
				opno = lfirst_oid(opnos_cell);
				inputcollation = lfirst_oid(collids_cell);

				/*
				 * leftop should be the index key Var, possibly relabeled
				 */
				if (leftop && IsA(leftop, RelabelType))
					leftop = ((RelabelType *) leftop)->arg;

				Assert(leftop != NULL);

				if (!(IsA(leftop, Var) &&
					  ((Var *) leftop)->varno == INDEX_VAR))
					elog(ERROR, "indexqual doesn't have key on left side");

				varattno = ((Var *) leftop)->varattno;

				/*
				 * We have to look up the operator's associated btree support
				 * function
				 */
				if ((index->rd_rel->relam != BTREE_AM_OID &&
                     index->rd_rel->relam != ABTREE_AM_OID) ||
					varattno < 1 || varattno > indnkeyatts)
					elog(ERROR, "bogus RowCompare index qualification");
				opfamily = index->rd_opfamily[varattno - 1];

				get_op_opfamily_properties(opno, opfamily, isorderby,
										   &op_strategy,
										   &op_lefttype,
										   &op_righttype);

				if (op_strategy != rc->rctype)
					elog(ERROR, "RowCompare index qualification contains wrong operator");

				opfuncid = get_opfamily_proc(opfamily,
											 op_lefttype,
											 op_righttype,
											 BTORDER_PROC);
				if (!RegProcedureIsValid(opfuncid))
					elog(ERROR, "missing support function %d(%u,%u) in opfamily %u",
						 BTORDER_PROC, op_lefttype, op_righttype, opfamily);

				/*
				 * rightop is the constant or variable comparison value
				 */
				if (rightop && IsA(rightop, RelabelType))
					rightop = ((RelabelType *) rightop)->arg;

				Assert(rightop != NULL);

				if (IsA(rightop, Const))
				{
					/* OK, simple constant comparison value */
					scanvalue = ((Const *) rightop)->constvalue;
					if (((Const *) rightop)->constisnull)
                    {
                        subplan_info->emptyres = true;
                        return;
                    }
				}
				else
				{
					/* 
                     * Need to treat this one as a runtime key;
                     * XXX disabled 
                     */
                    subplan_info->valid = false;
                    return;
					/*if (n_runtime_keys >= max_runtime_keys)
					{
						if (max_runtime_keys == 0)
						{
							max_runtime_keys = 8;
							runtime_keys = (IndexRuntimeKeyInfo *)
								palloc(max_runtime_keys * sizeof(IndexRuntimeKeyInfo));
						}
						else
						{
							max_runtime_keys *= 2;
							runtime_keys = (IndexRuntimeKeyInfo *)
								repalloc(runtime_keys, max_runtime_keys * sizeof(IndexRuntimeKeyInfo));
						}
					}
					runtime_keys[n_runtime_keys].scan_key = this_sub_key;
					runtime_keys[n_runtime_keys].key_expr =
						ExecInitExpr(rightop, planstate);
					runtime_keys[n_runtime_keys].key_toastable =
						TypeIsToastable(op_righttype);
					n_runtime_keys++;
					scanvalue = (Datum) 0;*/
				}

				/*
				 * initialize the subsidiary scan key's fields appropriately
				 */
				ScanKeyEntryInitialize(this_sub_key,
									   flags,
									   varattno,	/* attribute number */
									   op_strategy, /* op's strategy */
									   op_righttype,	/* strategy subtype */
									   inputcollation,	/* collation */
									   opfuncid,	/* reg proc to use */
									   scanvalue);	/* constant */
				n_sub_key++;
			}

			/* Mark the last subsidiary scankey correctly */
			first_sub_key[n_sub_key - 1].sk_flags |= SK_ROW_END;

            used_as_bound = aqp_merge_current_qual_into_bounds(index,
                lb, &nlb, ub, &nub, &subplan_info->emptyres,
                rc->rctype, /*righttype=*/InvalidOid,
                /*opfuncid=*/InvalidOid,
                /*varattno*/first_sub_key[0]->sk_attno,
                PointerGetDatum(first_sub_key));

            if (subplan_info->emptyres)
                return;

            if (!used_as_bound)
            {
                ScanKey this_scan_key = &scan_keys[nquals++];
                /*
                 * We don't use ScanKeyEntryInitialize for the header because
                 * it isn't going to contain a valid sk_func pointer.
                 */
                MemSet(this_scan_key, 0, sizeof(ScanKeyData));
                this_scan_key->sk_flags = SK_ROW_HEADER;
                this_scan_key->sk_attno = first_sub_key->sk_attno;
                this_scan_key->sk_strategy = rc->rctype;
                /* sk_subtype, sk_collation, sk_func not used in a header */
                this_scan_key->sk_argument = PointerGetDatum(first_sub_key);
            }
		}
		else if (IsA(clause, ScalarArrayOpExpr))
		{
			/* indexkey op ANY (array-expression) */
			ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
			int			flags = 0;
			Datum		scanvalue;

			Assert(!isorderby);

			Assert(saop->useOr);
			opno = saop->opno;
			opfuncid = saop->opfuncid;

			/*
			 * leftop should be the index key Var, possibly relabeled
			 */
			leftop = (Expr *) linitial(saop->args);

			if (leftop && IsA(leftop, RelabelType))
				leftop = ((RelabelType *) leftop)->arg;

			Assert(leftop != NULL);

			if (!(IsA(leftop, Var) &&
				  ((Var *) leftop)->varno == INDEX_VAR))
				elog(ERROR, "indexqual doesn't have key on left side");

			varattno = ((Var *) leftop)->varattno;
			if (varattno < 1 || varattno > indnkeyatts)
				elog(ERROR, "bogus index qualification");

			/*
			 * We have to look up the operator's strategy number.  This
			 * provides a cross-check that the operator does match the index.
			 */
			opfamily = index->rd_opfamily[varattno - 1];

			get_op_opfamily_properties(opno, opfamily, isorderby,
									   &op_strategy,
									   &op_lefttype,
									   &op_righttype);

			/*
			 * rightop is the constant or variable array value
			 */
			rightop = (Expr *) lsecond(saop->args);

			if (rightop && IsA(rightop, RelabelType))
				rightop = ((RelabelType *) rightop)->arg;

			Assert(rightop != NULL);

			if (index->rd_indam->amsearcharray)
			{
				/* Index AM will handle this like a simple operator */
				flags |= SK_SEARCHARRAY;
				if (IsA(rightop, Const))
				{
					/* OK, simple constant comparison value */
					scanvalue = ((Const *) rightop)->constvalue;
					if (((Const *) rightop)->constisnull)
						flags |= SK_ISNULL;
				}
				else
				{
					/* Need to treat this one as a runtime key */
					if (n_runtime_keys >= max_runtime_keys)
					{
						if (max_runtime_keys == 0)
						{
							max_runtime_keys = 8;
							runtime_keys = (IndexRuntimeKeyInfo *)
								palloc(max_runtime_keys * sizeof(IndexRuntimeKeyInfo));
						}
						else
						{
							max_runtime_keys *= 2;
							runtime_keys = (IndexRuntimeKeyInfo *)
								repalloc(runtime_keys, max_runtime_keys * sizeof(IndexRuntimeKeyInfo));
						}
					}
					runtime_keys[n_runtime_keys].scan_key = this_scan_key;
					runtime_keys[n_runtime_keys].key_expr =
						ExecInitExpr(rightop, planstate);

					/*
					 * Careful here: the runtime expression is not of
					 * op_righttype, but rather is an array of same; so
					 * TypeIsToastable() isn't helpful.  However, we can
					 * assume that all array types are toastable.
					 */
					runtime_keys[n_runtime_keys].key_toastable = true;
					n_runtime_keys++;
					scanvalue = (Datum) 0;
				}
			}
			else
			{
				/* Executor has to expand the array value */
				array_keys[n_array_keys].scan_key = this_scan_key;
				array_keys[n_array_keys].array_expr =
					ExecInitExpr(rightop, planstate);
				/* the remaining fields were zeroed by palloc0 */
				n_array_keys++;
				scanvalue = (Datum) 0;
			}

			/*
			 * initialize the scan key's fields appropriately
			 */
			ScanKeyEntryInitialize(this_scan_key,
								   flags,
								   varattno,	/* attribute number to scan */
								   op_strategy, /* op's strategy */
								   op_righttype,	/* strategy subtype */
								   saop->inputcollid,	/* collation */
								   opfuncid,	/* reg proc to use */
								   scanvalue);	/* constant */
		}
		else if (IsA(clause, NullTest))
		{
			/* indexkey IS NULL or indexkey IS NOT NULL */
			NullTest   *ntest = (NullTest *) clause;
			int			flags;

			Assert(!isorderby);

			/*
			 * argument should be the index key Var, possibly relabeled
			 */
			leftop = ntest->arg;

			if (leftop && IsA(leftop, RelabelType))
				leftop = ((RelabelType *) leftop)->arg;

			Assert(leftop != NULL);

			if (!(IsA(leftop, Var) &&
				  ((Var *) leftop)->varno == INDEX_VAR))
				elog(ERROR, "NullTest indexqual has wrong key");

			varattno = ((Var *) leftop)->varattno;

			/*
			 * initialize the scan key's fields appropriately
			 */
			switch (ntest->nulltesttype)
			{
				case IS_NULL:
					flags = SK_ISNULL | SK_SEARCHNULL;
					break;
				case IS_NOT_NULL:
					flags = SK_ISNULL | SK_SEARCHNOTNULL;
					break;
				default:
					elog(ERROR, "unrecognized nulltesttype: %d",
						 (int) ntest->nulltesttype);
					flags = 0;	/* keep compiler quiet */
					break;
			}

			ScanKeyEntryInitialize(this_scan_key,
								   flags,
								   varattno,	/* attribute number to scan */
								   InvalidStrategy, /* no strategy */
								   InvalidOid,	/* no strategy subtype */
								   InvalidOid,	/* no collation */
								   InvalidOid,	/* no reg proc for this */
								   (Datum) 0);	/* constant */
		}
#endif      /* ifdef 0 (disabled) */
		else
			elog(ERROR, "unsupported indexqual type: %d",
				 (int) nodeTag(clause));
	}
    
    /*
     * The number of lower bound and upper bound should really just differ by
     * one, and the whichever is larger, the last one of that should be
     * the last key column we vary on.
     */
    Assert(nlb - nub >= -1 && nlb - nub <= 1);
    partition_attno = Max(nlb, nub) - 1;
    if (lb[partition_attno].sk_strategy == BTEqualStrategyNumber)
    {
        Assert(nub == nlb);
        Assert(ub[partition_attno].sk_strategy == BTEqualStrategyNumber);
        ++partition_attno;
    }

    if (partition_attno > indnkeyatts)
    {
        /* all equality condition, no logical partition is possible. */
        subplan_info->allequi = true;
    }
    
    /* output all keys into subplan_info */
    subplan_info->n_scan_keys = subplan_info->n_partition_keys =
        partition_attno - 1 /* the equi-prefix */
        + nquals; /* the remaining checked index quals */

    if (!subplan_info->allequi)
    {
        subplan_info->n_lmost_partition_keys = subplan_info->n_partition_keys;
        subplan_info->n_rmost_partition_keys = subplan_info->n_partition_keys;
        subplan_info->n_partition_keys += 2; /* lb & ub */
        if (partition_attno >= nlb)
            subplan_info->n_lmost_partition_keys += 1; /* only ub */
        else
        {
            subplan_info->n_lmost_partition_keys += 2; /* lb & ub */
            subplan_info->n_scan_keys += 1;
        }
        if (partition_attno >= nub)
            subplan_info->n_rmost_partition_keys += 1; /* only lb */
        else
        {
            subplan_info->n_rmost_partition_keys += 2; /* lb & ub */
            subplan_info->n_scan_keys += 1;
        }
    }
    else
    {
        /* allequi*/
        subplan_info->n_partition_keys = 0;
        subplan_info->n_lmost_partition_keys = 0;
        subplan_info->n_rmost_partition_keys = 0;
    }
    
    subplan_info->scan_keys =
        (ScanKey) palloc(sizeof(ScanKeyData) * subplan_info->n_scan_keys);
    subplan_info->partition_keys =
        subplan_info->n_partition_keys == 0 ? NULL :
        (ScanKey) palloc(sizeof(ScanKeyData) * subplan_info->n_partition_keys);
    subplan_info->lmost_partition_keys = 
        subplan_info->n_lmost_partition_keys == 0 ? NULL :
        (ScanKey) palloc(sizeof(ScanKeyData) *
            subplan_info->n_lmost_partition_keys);
    subplan_info->rmost_partition_keys =
        subplan_info->n_rmost_partition_keys == 0 ? NULL :
        (ScanKey) palloc(sizeof(ScanKeyData) *
            subplan_info->n_rmost_partition_keys);
    
    /*
     * The index key processing code insists the scan keys to be sorted
     * on the attno, so we have to mix in the scan_keys into the bounds,
     * unfortunately.
     */
    iscankeys = 0; /* idx into subplan_info->partition_keys */
    j = 0; /* idx into scan_keys (the extra index equals) */
    for (attno = 1; attno < partition_attno; ++attno)
    {
        ScanKey key;
        Assert(lb[attno].sk_attno == attno);
        Assert(ub[attno].sk_attno == attno);
        Assert(lb[attno].sk_strategy == BTEqualStrategyNumber);
        Assert(ub[attno].sk_strategy == BTEqualStrategyNumber);
        
        /*
         * Prefer not using cross-type comparison but this is just a
         * best-effort thing: (1) we might have thrown away some same-type
         * comparison during the merge; (2) there may not be any same-type
         * comparison anyway.
         */
        if (lb[attno].sk_subtype == index->rd_opcintype[attno - 1])
            key = &lb[attno];
        else
            key = &ub[attno];

        /* 
         * We did not initialize fmgr_info for lb & ub arrays, so we must
         * do that here.
         */
        Assert(iscankeys < subplan_info->n_scan_keys);
        ScanKeyEntryInitialize(&subplan_info->scan_keys[iscankeys++],
                               key->sk_flags,
                               key->sk_attno,
                               key->sk_strategy,
                               key->sk_subtype,
                               key->sk_collation,
                               key->sk_func.fn_oid,
                               key->sk_argument);
    
        /*
         * The additional index quals have been initialized so just copy
         * it over.
         */
        while (j < nquals)
        {
            /* XXX this only works with regular single-attr scankeys */
            if (scan_keys[j].sk_attno == attno)
            {
                Assert(iscankeys < subplan_info->n_scan_keys);
                memcpy(&subplan_info->scan_keys[iscankeys++],
                       &scan_keys[j],
                       sizeof(ScanKeyData));
                ++j;
            }
            else
            {
                break;
            }
        }
    }
    
    if (!subplan_info->allequi && iscankeys > 0)
    {
        memcpy(subplan_info->partition_keys,
               subplan_info->scan_keys, sizeof(ScanKeyData) * iscankeys);
        memcpy(subplan_info->lmost_partition_keys,
               subplan_info->scan_keys, sizeof(ScanKeyData) * iscankeys);
        memcpy(subplan_info->rmost_partition_keys,
               subplan_info->scan_keys, sizeof(ScanKeyData) * iscankeys);
    }
    
    /* Now we're emitting the partition keys */
    ilmost = iscankeys;
    irmost = iscankeys;
    i = iscankeys; /* index into partition_keys */
    if (!subplan_info->allequi)
    {
        TupleDesc desc;
        Oid atttypid;
        Oid opfamily;
        Oid opno;
        Oid opfuncid;

        if (partition_attno < nlb)
        {
            ScanKey key = &lb[partition_attno];

            Assert(key->sk_attno == partition_attno);
            Assert(key->sk_strategy == BTGreaterStrategyNumber ||
                   key->sk_strategy == BTGreaterEqualStrategyNumber);

            /* has lb, emit it into lmost partition_keys and scankeys */
            Assert(ilmost < subplan_info->n_lmost_partition_keys);
            ScanKeyEntryInitialize(
                &subplan_info->lmost_partition_keys[ilmost++],
                key->sk_flags,
                key->sk_attno,
                key->sk_strategy,
                key->sk_subtype,
                key->sk_collation,
                key->sk_func.fn_oid,
                key->sk_argument);

            Assert(iscankeys < subplan_info->n_scan_keys);
            memcpy(&subplan_info->scan_keys[iscankeys++],
                   &subplan_info->lmost_partition_keys[ilmost - 1],
                   sizeof(ScanKeyData));
        }
        
        /* 
         * emit the runtime lb with >= strategy in partition_keys and
         * rmost_partition_keys 
         */
        Assert(i < subplan_info->n_partition_keys);
        Assert(irmost < subplan_info->n_rmost_partition_keys);
        subplan_info->lb_idx_in_partition_keys = i;
        subplan_info->lb_idx_in_rmost_partition = irmost;
        
        desc = RelationGetDescr(index);
        atttypid = desc->attrs[partition_attno - 1].atttypid;
        opfamily = index->rd_opfamily[partition_attno - 1];
        opno = get_opfamily_member(opfamily, atttypid, atttypid,
                                   BTGreaterEqualStrategyNumber);
        if (!OidIsValid(opno))
        {
            elog(ERROR, "failed to find >= operator for type %u in "
                        "opfamily %u", atttypid, opfamily);
        }
        opfuncid = get_opcode(opno);
        if (!OidIsValid(opfuncid))
        {
            elog(ERROR, "failed to find the function for >= operator (%u) "
                        "for type %u in opfamily %u",
                        opno, atttypid, opfamily);
        }

        ScanKeyEntryInitialize(&subplan_info->partition_keys[i++],
                               /*flags=*/0,
                               partition_attno,
                               BTGreaterEqualStrategyNumber,
                               atttypid,
                               desc->attrs[partition_attno - 1].attcollation,
                               opfuncid,
                               (Datum) 0 /* dummy for runtime value */);
        memcpy(&subplan_info->rmost_partition_keys[irmost++],
               &subplan_info->partition_keys[i - 1],
               sizeof(ScanKeyData));

        if (partition_attno < nub)
        {
            ScanKey key = &ub[partition_attno];

            Assert(key->sk_attno == partition_attno);
            Assert(key->sk_strategy == BTLessStrategyNumber ||
                   key->sk_strategy == BTLessEqualStrategyNumber);

            /* has ub, emit it into rmost partition_keys and scankeys */
            Assert(irmost < subplan_info->n_rmost_partition_keys);
            ScanKeyEntryInitialize(
                &subplan_info->rmost_partition_keys[irmost++],
                key->sk_flags,
                key->sk_attno,
                key->sk_strategy,
                key->sk_subtype,
                key->sk_collation,
                key->sk_func.fn_oid,
                key->sk_argument);

            Assert(iscankeys < subplan_info->n_scan_keys);
            memcpy(&subplan_info->scan_keys[iscankeys++],
                   &subplan_info->rmost_partition_keys[irmost - 1],
                   sizeof(ScanKeyData));
        }

        /* 
         * emit the runtime ub with < strategy in partition_keys and
         * lmost_partition_keys 
         */
        Assert(i < subplan_info->n_partition_keys);
        Assert(ilmost < subplan_info->n_lmost_partition_keys);
        subplan_info->ub_idx_in_partition_keys = i;
        subplan_info->ub_idx_in_lmost_partition = ilmost;
        
        opno = get_opfamily_member(opfamily, atttypid, atttypid,
                                   BTLessStrategyNumber);
        if (!OidIsValid(opno))
        {
            elog(ERROR, "failed to find < operator for type %u in "
                        "opfamily %u", atttypid, opfamily);
        }
        opfuncid = get_opcode(opno);
        if (!OidIsValid(opfuncid))
        {
            elog(ERROR, "failed to find the function for < operator (%u) "
                        "for type %u in opfamily %u",
                        opno, atttypid, opfamily);
        }

        ScanKeyEntryInitialize(&subplan_info->partition_keys[i++],
                               /*flags=*/0,
                               partition_attno,
                               BTLessStrategyNumber,
                               atttypid,
                               desc->attrs[partition_attno - 1].attcollation,
                               opfuncid,
                               (Datum) 0 /* dummy for runtime value */);
        memcpy(&subplan_info->lmost_partition_keys[ilmost++],
               &subplan_info->partition_keys[i - 1],
               sizeof(ScanKeyData));

        subplan_info->partition_attno = partition_attno;
        subplan_info->partition_atttypid = atttypid;
    }
    else
    {
        /* allequi */
        subplan_info->partition_attno = InvalidAttrNumber;
        subplan_info->partition_atttypid = InvalidOid;
    }
    
    /* paste the rest of the index quals into all four key arrays */
    nremquals = nquals - j;
    Assert(nremquals + i == subplan_info->n_partition_keys);
    Assert(nremquals + iscankeys == subplan_info->n_scan_keys);
    Assert(nremquals + ilmost == subplan_info->n_lmost_partition_keys);
    Assert(nremquals + irmost == subplan_info->n_rmost_partition_keys);
    if (nremquals > 0)
    {
        Assert(!subplan_info->allequi);
        memcpy(&subplan_info->partition_keys[i],
               &subplan_info->scan_keys[j],
               sizeof(ScanKeyData) * nremquals);
        memcpy(&subplan_info->scan_keys[iscankeys],
               &subplan_info->scan_keys[j],
               sizeof(ScanKeyData) * nremquals);
        memcpy(&subplan_info->lmost_partition_keys[ilmost],
               &subplan_info->scan_keys[j],
               sizeof(ScanKeyData) * nremquals);
        memcpy(&subplan_info->rmost_partition_keys[irmost],
               &subplan_info->scan_keys[j],
               sizeof(ScanKeyData) * nremquals);
    }

    /* 
     * For supporting our sorting and partitioning procedure on the partition
     * key, let's look up some support functions as belows.
     */
    if (subplan_info->partition_attno != InvalidAttrNumber)
    {
        SortSupport ssup = &subplan_info->partition_att_sortsupp;
        TupleDesc desc = RelationGetDescr(index);

        memset(ssup, 0, sizeof(SortSupportData));
        ssup->ssup_cxt = CurrentMemoryContext;
        ssup->ssup_collation = desc->attrs[partition_attno - 1].attcollation;
        /* 
         * We could still have null key values because the partition key may be
         * unbounded (e.g., x = 1 for an index over (x, y), where the partition
         * key will be y).
         * 
         * TODO maybe we won't care about this so much because, for efficiency
         * purposes, we might want to store NULLs separately. Let's see how
         * that works when we implement the DP in pswrctl.
         */
        ssup->ssup_nulls_first =
            !!(*index->rd_indoption & INDOPTION_NULLS_FIRST);
        ssup->ssup_attno = subplan_info->partition_attno;
        ssup->abbreviate = false;
    
        /*
         * The strategy number is chosen such that ssup->ssup_reverse is set to
         * false.
         */
        PrepareSortSupportFromIndexRel(index, BTLessStrategyNumber, ssup);
        Assert(!ssup->ssup_reverse);
        Assert(ssup->comparator);
    }
    
    /* mark this subplan as valid for pswr purpose */
    subplan_info->valid = true;
}

static inline bool
aqp_is_lb_strategy(StrategyNumber strategy)
{
    return strategy == BTGreaterStrategyNumber ||
           strategy == BTGreaterEqualStrategyNumber ||
           strategy == BTEqualStrategyNumber;
}

static inline bool
aqp_is_ub_strategy(StrategyNumber strategy)
{
    return strategy == BTLessStrategyNumber ||
           strategy == BTLessEqualStrategyNumber ||
           strategy == BTEqualStrategyNumber;
}

static inline bool
aqp_is_strict_strategy(StrategyNumber strategy)
{
    return strategy == BTLessStrategyNumber ||
        strategy == BTGreaterStrategyNumber;
}

/*
 * Adapted from _abt_compare_scankey_args except that we don't need to handle
 * NULLs here.
 */
static bool
aqp_compare_scankey_args(Relation index,
                         AttrNumber attno,
                         Oid optype,
                         Oid opfuncid,
                         Oid opcollid,
                         StrategyNumber strat,
                         Datum larg,
                         Oid lefttype,
                         Datum rarg,
                         Oid righttype,
                         bool *result)
{
    Oid opcintype;
    Oid cmp_op;

	opcintype = index->rd_opcintype[attno - 1];

    Assert(lefttype != InvalidOid);
    Assert(righttype != InvalidOid);
    Assert(optype != InvalidOid);

	/*
	 * If leftarg and rightarg match the types expected for the "op" scankey,
	 * we can use its already-looked-up comparison function.
	 */
	if (lefttype == opcintype && righttype == optype)
	{
		*result = DatumGetBool(OidFunctionCall2Coll(opfuncid,
												    opcollid,
												    larg,
												    rarg));
		return true;
	}

	/*
	 * Note that in AB-tree/NB-tree code, this function takes the already
	 * commuted strategy number from the preprocess keys, so it must commute
     * it back to get the correct comparison operator.
	 * However, we don't have to because whether the index order keys
     * in descending or ascending order does not impact whether this is a
     * lb or ub as long as we don't commute it in the first place.
	 *
	 * if (*rel->rd_indoption & INDOPTION_DESC)
     *		strat = ABTCommuteStrategyNumber(strat);
	 */
	cmp_op = get_opfamily_member(index->rd_opfamily[attno - 1],
								 lefttype,
								 righttype,
								 strat);
	if (OidIsValid(cmp_op))
	{
		RegProcedure cmp_proc = get_opcode(cmp_op);

		if (RegProcedureIsValid(cmp_proc))
		{
			*result = DatumGetBool(OidFunctionCall2Coll(cmp_proc,
														opcollid,
														larg,
														rarg));
			return true;
		}
	}

	/* Can't make the comparison */
	*result = false;			/* suppress compiler warnings */
	return false;
}

/*
 * We assume the caller won't give us a row comparison's rhs as the rhs, and we
 * cannot start with a row comparison in the last position in lb or ub.
 */
static bool
aqp_merge_current_qual_into_bounds(
    Relation index,
    ScanKey lb,
    int *p_nlb,
    ScanKey ub,
    int *p_nub,
    bool *emptyres,
    StrategyNumber op_strategy,
    Oid op_righttype,
    Oid opfuncid,
    Oid op_inputcollid,
    AttrNumber varattno,
    Datum scanvalue)
{
    /*
     * used_as_bound means the current bounds (lb/ub) implies the current qual,
     * and thus the bound does not have to be added an an extra index qual.
     * However, the current qual may be less restrictive so that it is not
     * really referenced in lb/ub.
     */
    bool used_as_bound = false;

    if (aqp_is_lb_strategy(op_strategy))
    {
        used_as_bound |= aqp_merge_current_qual_into_bounds_impl(
            index, lb, p_nlb, emptyres,
            op_strategy, op_righttype, opfuncid, op_inputcollid,
            varattno,
            scanvalue);
        if (*emptyres)
            return used_as_bound;
    }
   
    if (aqp_is_ub_strategy(op_strategy))
    {
        used_as_bound |= aqp_merge_current_qual_into_bounds_impl(
            index, ub, p_nub, emptyres,
            op_strategy, op_righttype, opfuncid, op_inputcollid,
            varattno,
            scanvalue);
    }

    return used_as_bound;
}

static bool
aqp_merge_current_qual_into_bounds_impl(
    Relation index,
    ScanKey lb,
    int *p_nlb,
    bool *emptyres,
    StrategyNumber op_strategy,
    Oid op_righttype,
    Oid opfuncid,
    Oid op_inputcollid,
    AttrNumber varattno,
    Datum scanvalue)
{
    /*
     * Although below we describe the algo. assuming the stratgegy
     * is >, >= or =, and we are solving for a lower bound. It should work
     * for the case of <, <= or = while we solve for an upper bound.
     */
    bool cmpres;
    ScanKey last = &lb[*p_nlb - 1];
    if (last->sk_attno == varattno)
    {
        if (last->sk_strategy == BTEqualStrategyNumber)
        {
            /* 
             * If the current clause is x op c, and the last one is
             * x = c', we check whether c' op c. 
             *
             * If this ends up being incomparable, we move x op c
             * into the additional quals since we can't check it
             * right now.
             *
             * If this is comparable and the result is true, that
             * means we can drop the current clause x op c as this
             * is less restrictive.
             *
             * Otherwise, it is an infeasible qual that will lead
             * to empty sample space.
             */
            if (aqp_compare_scankey_args(
                    index,
                    varattno,
                    op_righttype,
                    opfuncid,
                    op_inputcollid,
                    op_strategy,
                    last->sk_argument,
                    last->sk_subtype,
                    scanvalue,
                    op_righttype,
                    &cmpres))
            {
                if (cmpres)
                {
                    return true;
                }
                else
                {
                    *emptyres = true;
                    return false;
                }
            }
        }
        else
        {
            /*
             * The current clause is x op c and the last one is x
             * op2 c', where op2 may be >/>=.
             *
             * The first branch handles the case where op2 is >
             * or op is =. Then we compare whether c op2 c'.
             *
             */
            if (aqp_is_strict_strategy(last->sk_strategy) ||
                op_strategy == BTEqualStrategyNumber)
            {
                if (aqp_compare_scankey_args(
                        index,
                        varattno,
                        last->sk_subtype,
                        last->sk_func.fn_oid,
                        last->sk_collation,
                        last->sk_strategy,
                        scanvalue,
                        op_righttype,
                        last->sk_argument,
                        last->sk_subtype,
                        &cmpres))
                {
                    if (cmpres)
                    {
                        /* 
                         * c op2 c'
                         *
                         * This branch handles the following cases:
                         * 1. op2 is >, and op is any one of
                         * >/>=/=, then x op c is more restrictive
                         * than x op2 c'.
                         * 2. op2 is >=, and op is =, then x = c
                         * (>= c')  is more restrictive than x >=
                         * c'.
                         *
                         * Hence the current one will replace the
                         * previous one.
                         */
                        last->sk_strategy = op_strategy;
                        last->sk_subtype = op_righttype;
                        last->sk_collation = op_inputcollid;
                        last->sk_argument = scanvalue;
                        last->sk_func.fn_oid = opfuncid;
                        return true;
                    }
                    else
                    {
                        /* 
                         * !(c op2 c')
                         *
                         * If op is =, then x op c contradicts x
                         * op2 c'.
                         *
                         * If op is > or >=, then op2 must be >.
                         * So, x op c is less or as restrictive
                         * than x > c' and thus the current clause
                         * can be dropped.
                         */
                        if (op_strategy == BTEqualStrategyNumber)
                        {
                            /*
                             * !(c op2 c')
                             *
                             * Then x = c contradicts x op2 c'.
                             */
                            *emptyres = true;
                            return false;
                        }
                        return true;
                    }
                }
            }
            /*
             * This branch handles op2 is >= and op is not =. We
             * compare whether c' op c.
             */
            else
            {
                if (aqp_compare_scankey_args(
                        index,
                        varattno,
                        op_righttype,
                        opfuncid,
                        op_inputcollid,
                        op_strategy,
                        last->sk_argument,
                        last->sk_subtype,
                        scanvalue,
                        op_righttype,
                        &cmpres))
                {
                    if (cmpres)
                    {
                        /*
                         * c' op c.
                         *
                         * Since op is >/>=, the previous clause x >=
                         * c' is more or as restrictive.
                         */
                        return true;
                    }
                    else
                    {
                        /*
                         * !(c' op c)
                         *
                         * Then the new clause x op c is always more
                         * restrictive than the previous clause x
                         * >= c'.
                         */
                        last->sk_strategy = op_strategy;
                        last->sk_subtype = op_righttype;
                        last->sk_collation = op_inputcollid,
                        last->sk_argument = scanvalue;
                        last->sk_func.fn_oid = opfuncid;
                        return true;
                    }
                }
            }
        }
    }
    else if (last->sk_attno + 1 == varattno &&
             last->sk_strategy == BTEqualStrategyNumber) 
    {

        /* 
         * Don't fully initialize sk_func yet, as we may replace it
         * with more strict ones later. 
         */
        lb[*p_nlb].sk_flags = 0;
        lb[*p_nlb].sk_attno = varattno;
        lb[*p_nlb].sk_strategy = op_strategy;
        lb[*p_nlb].sk_subtype = op_righttype;
        lb[*p_nlb].sk_collation = op_inputcollid;
        lb[*p_nlb].sk_argument = scanvalue;

        /* 
         * Temporarily storing fn_oid in sk_func.fn_oid, but the
         * entire sk_func eventually needs to be initialized before
         * we return from the caller.
         */
        lb[*p_nlb].sk_func.fn_oid = opfuncid;

        ++*p_nlb;
        return true;
    }
    /* else can't be used as a bound key */

    return false;
}

void
aqp_setup_swr(void)
{
    RegisterExtensibleNodeMethods(&aqp_swr_scan_private_methods);
    RegisterExtensibleNodeMethods(&aqp_swr_index_only_scan_private_methods);
    RegisterExtensibleNodeMethods(&aqp_pswr_scan_private_methods);
    RegisterExtensibleNodeMethods(&aqp_pswr_indexonly_scan_private_methods);
    RegisterCustomScanMethods(&aqp_swrscan_methods);
    RegisterCustomScanMethods(&aqp_swrindexonlyscan_methods);
    RegisterCustomScanMethods(&aqp_progressive_swrscan_methods);
}

