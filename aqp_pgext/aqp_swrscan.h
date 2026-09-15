#ifndef AQP_SWRSCAN_H
#define AQP_SWRSCAN_H

#include "aqp.h"

#include <access/genam.h>
#include <access/relscan.h>
#include <access/skey.h>
#include <nodes/extensible.h>
#include <nodes/execnodes.h>
#include <nodes/pg_list.h>
#include <lib/rbtree.h>
#include <utils/sampling.h>

#include "aqp_sample_scan_state.h"

#define AQP_ISSFLAG_INDEXONLY 0x1u
#define AQP_ISSFLAG_PSWR_SUBPLAN 0x2u

#define AQPISSIsIndexOnly(node) \
    (!!(((AQPIndexSampleScanPrivate *)(node))->flags & AQP_ISSFLAG_INDEXONLY))
#define AQPISSIsPSWRSubPlan(node) \
    (!!(((AQPIndexSampleScanPrivate *)(node))->flags \
        & AQP_ISSFLAG_PSWR_SUBPLAN))

#define AQPSWRScanPrivateName "AQPSWRScanPrivate"
#define AQPSWRIndexOnlyScanPrivateName "AQPSWRIndexOnlyScanPrivate"
#define AQPPSWRScanPrivateName "AQPProgressiveSWRScanPrivate"
#define AQPPSWRIndexOnlyScanPrivateName "AQPProgresiveSWRIndexOnlyScanPrivate"

typedef struct DPInfoTreeNode
{
	RBTNode		rbtnode;
	Datum		key;
    uint64      count_p1;
    uint64      count_p1_p2;
} DPInfoTreeNode;

typedef struct DPInfo
{
	Datum		upper_key;
    Datum       lower_key;
    uint64      count_p1;
    uint64      count_p1_p2;
} DPInfo;

typedef struct DPOutput
{
	int		    *id;
    Datum		lower_key;
    Datum		upper_key;
    uint64      ni;
    float8      percentage;
} DPOutput;

/*
 * This is the combined private structure for AQPIndexSampleScan for SWRScan,
 * SWRIndexOnlyScan.
 */
typedef struct AQPIndexSampleScanPrivate {
    ExtensibleNode  extnode;
    
    /*
     * See AQP_ISSFLAG_xxx macros.
     */
    unsigned        flags;
    
    /* OID of the index to sample from */
    Oid             indexid;

    /* list of index quals (usually OpExprs) */
    List            *indexqual;

    /* list index quals in original form (non-NULL for SWRScan only) */
    List            *indexqualorig;
    
    /* Number of samples to fetch if it is a known constant. */
    uint64          sample_size;
    
    /* The repeatable expression in the tablesample clause */
    Expr            *repeatable_expr;
    
    /* Non-null if the sample size is not constant. */
    Expr            *sample_size_expr;
    
    /* 
     * In new aggregation rewriting impl, we use this global parameter to pass
     * sampler input & output info sideway. This allows us to write to or read
     * from the aggregation function regardless of what is actually being
     * passed along in the query pipeline.  There will be one shared across the
     * entire level of the plan (but subplan has its own).
     *
     * This is declared as INT8 type, and will store the param id to the
     * AQPPSWRCtlInfo structure.
     */
    Param           *pswrctl_info_param;
} AQPIndexSampleScanPrivate;

/*
 * Extended private structures for SWRScan and SWRIndexOnlyScan as PSWR
 * subplans.
 */
typedef struct AQPProgressiveSampleScanPrivate {
    AQPIndexSampleScanPrivate issp;
    
    List            *qptlist;

    List            *qpqual;

    List            *indextlist;

    int             indexattr;
    
    int16           typlen;

    bool            typbyval;

    /*Oid           indextype;*/

    /*Oid           indexopfamily;*/

    FmgrInfo        compareFn;

    Oid             supportCollation;

    Param           *lower_param;
    
    Param           *upper_param;

    RegProcedure    lower_opfuncid;

    RegProcedure    upper_opfuncid;

    Param           *running_sample_size_param;

    Param           *running_sample_budget_param;

    Param           *running_state_id_param;

} AQPProgressiveSampleScanPrivate;

typedef struct AQPIndexSampleScanState {
    CustomScanState         css;

    /* execution state for indexqual expressions */
    ExprState               *indexqual;

    /* execution state for indexqual expressions */
    ExprState               *indexqualorig;

    /* scan key structure for index quals */
    struct ScanKeyData      *scan_keys;

    /* number of scan keys */
    int                     num_scan_keys;

    /* info about runtime scan keys */
    IndexRuntimeKeyInfo     *runtime_keys;

    /* number of runtime scan keys */
    int                     num_runtime_keys;

    /* whether the runtime scan keys have been computed */
    bool                    runtime_keys_ready;

    /* the context for evaluting the runtime scan keys */
    ExprContext             *runtime_context;

    /* index relation descriptor */
    Relation                indexrel;

    /* index scan descriptor  */
    IndexScanDesc           scandesc;

    /* table slot for heap tuple fetch in case of need for visibility check */
    TupleTableSlot          *table_slot;
    
    /* buffer for visibility checks */
    Buffer                  vmbuffer;

    /* want probability */
    bool                    want_prob;

    bool                    p2matter;

    bool                    is_partition;

    bool                    use_batch_sampling;

    double                  percentage;

    TupleTableSlot          *allnullslot;

    ParamExecData           *running_sample_size;
    
    ParamExecData           *running_sample_budget;

    ParamExecData           *running_state_id;
    
    struct AQPPSWRCtlInfoData *pswrctl_info;

    /* Index in pswrctl_info->subplan_info for this PSWR index candidate. */
    int                     pswrctl_subplan_id;

    /*
     * Physical sampler id in pswrctl_info->sampler_ctl[].  This must not be
     * confused with pswrctl_subplan_id, which is a PSWR index-candidate id.
     */
    int                     pswrctl_sampler_id;    

    /* the sample scan state (TODO deprecate this) */
    AQPSampleScanState      sss;

    SamplerRandomState      rand_state;
} AQPIndexSampleScanState;

typedef struct AQPProgressiveSampleScanSubplanState {
    TupleTableSlot *scanslot;

    TupleDesc scan_descriptor;

    const TupleTableSlotOps *scanops;
    
    ProjectionInfo *projInfo;

    bool resultopsset;

    bool resultopsfixed;

    const TupleTableSlotOps *resultops;
    
    ExprState *qual;

    ExprState *indexqual;

    ExprState *indexqualorig;

    Relation indexrel;
    
    /* 
     * TODO should all keys be made runtime keys, since we can partition
     * the key space?
     */
    struct ScanKeyData *scan_keys;

    int num_scan_keys;

    IndexRuntimeKeyInfo *runtime_keys;

    int num_runtime_keys;

    struct IndexScanDescData *scandesc;

    int indexattr;

    Oid indexkeytype;

    Oid indexkeyopfamily;

    FmgrInfo compareFn;

    Oid supportCollation;

    int16 typlen;

    bool typbyval;
    
    RBTree *dpinfotree;

    int inputcnt;

    DPOutput *dpoutput;

    int outputcnt;

    int out_index;

    TupleTableSlot *allnullslot;

    int lower_paramid;

    int upper_paramid;

    RegProcedure lower_opfuncid;
    
    RegProcedure upper_opfuncid;

    ExprState *runtime_l_expr;

    ExprState *runtime_u_expr;

    ScanKey upper_scankey;
    
    ScanKey lower_scankey;
    
    ExprContext *runtime_context;
} AQPProgressiveSampleScanSubplanState;

typedef struct AQPProgressiveSampleScanState {
    AQPIndexSampleScanState isss;

    List *orig_tlist;

    int nsubplans;

    int selected_subplan_idx;

    TupleTableSlot *swr_scanslot;

    TupleDesc swr_scan_descriptor;

    const TupleTableSlotOps *swr_scanops;

    ProjectionInfo *swr_projInfo;
    
    bool swr_resultopsset;

    bool swr_resultopsfixed;

    const TupleTableSlotOps *swr_resultops;
    
    /* The total sampling size budget. */
    uint64 sample_budget;

    uint64 rem_sample_budget;

    bool   is_index_only_scan;
    
    bool   is_collect_finish;

    TupleTableSlot *swr_allnullslot;

    AQPProgressiveSampleScanSubplanState subplan_states[FLEXIBLE_ARRAY_MEMBER];
} AQPProgressiveSampleScanState;

extern CustomScanMethods aqp_swrscan_methods;
extern CustomExecMethods aqp_swrscan_exec_methods;
extern CustomScanMethods aqp_swrindexonlyscan_methods;
extern CustomExecMethods aqp_swrindexonlyscan_exec_methods;
extern CustomScanMethods aqp_progressive_swrscan_methods;
extern CustomExecMethods aqp_progressive_swrscan_exec_methods;
extern CustomExecMethods aqp_swrscan_exec_methods_new;
extern CustomExecMethods aqp_swrindexonlyscan_exec_methods_new;
extern CustomExecMethods aqp_pswrscan_exec_methods_new;

extern Node *aqp_index_sample_scan_create_state(CustomScan *cscan);

static inline bool
aqp_plan_is_index_sample_scan(Plan *plan)
{
    /*
     * NOTE All CustomScanMethods for index sample scans have the same
     * state creation function. Keep in sync with aqp_swrscan.c if this is
     * changed.
     */
    return plan != NULL && IsA(plan, CustomScan) &&
        ((CustomScan *) plan)->methods->CreateCustomScanState ==
            aqp_index_sample_scan_create_state;
}

/* 
 * These attribute numbers only appear in query plans and should never be
 * evaluated by projection expressions.
 */
#define AQPSampleProbAttributeNumber        (-10)
#define AQPInvSampleProbAttributeNumber     (-11)

#endif  /* AQP_SWRSCAN_H */
