#ifndef AQP_PSWRCTL_NODE_H
#define AQP_PSWRCTL_NODE_H

#include "aqp.h"

#include <access/skey.h>
#include <nodes/extensible.h>
#include <utils/tuplestore.h>

#include "aqp_swrscan.h"

typedef struct AQPPSWRControlPathPrivate
{
    ExtensibleNode      extnode;

    Expr                *sample_size_expr;

    Expr                *initial_phase_sample_size_expr;

    Expr                *step_sample_size_expr;

    Expr                *err0_expr;

    Expr                *confidence0_expr;
} AQPPSWRControlPathPrivate;

typedef CustomPath AQPPSWRControlPath;

typedef struct AQPPSWRControlPrivate
{
    ExtensibleNode      extnode;

    /* 
     * The constant sample budget if the sample size specified in the
     * TABLESAMPLE is known at QO time.
     *
     * For the rest of the parameters, don't bother find them during
     * planning time because they do not matter for cost estimation.
     */
    uint64      sample_budget;

    Expr        *sample_size_expr;

    Expr        *initial_phase_sample_size_expr;

    Expr        *step_sample_size_expr;

    Expr        *err0_expr;

    Expr        *confidence0_expr;
    
    int         pswrctl_info_paramid; 
} AQPPSWRControlPrivate;

extern CustomPathMethods aqp_pswrctl_path_methods;
extern CustomScanMethods aqp_pswrctl_methods;
extern CustomExecMethods aqp_pswrctl_exec_methods;

extern void aqp_fix_pswrctl(
    CustomScan *cscan,
    int pswrctl_info_paramid);

typedef struct AQPPSWRCtlSubplanInfoData {
    bool        valid;

    bool        emptyres;
    
    /* 
     * There's an equi-condition on the entire index key, so no logical
     * partitioning is possible. 
     *
     * When this is true, the pswr should not try to perform more than 1 phase
     * and optimize the logical data partitioning and sample size allocation.
     *
     * XXX Technically, we could also partition on tids, but the semantics of
     * this might not be clear. (Maybe some temporal correlation?) Nevertheless
     * this special case is probably not worth considering for now.
     *
     * When this is true, partition_keys, lmost_partition_keys and
     * rmost_partition_keys are set to NULL, and n_partition_keys,
     * n_lmost_partition_keys and n_rmost_partition_keys are set to 0.
     */
    bool        allequi;

    int         nidxkeys;

    int         partition_attno;

    Oid         partition_atttypid;
    
    /*
     * The fast comparison sort support functions.
     *
     * We expect to have a valid compraator function set up here. See
     * include/utils/sortsupp.h.
     *
     * We don't enable abbreviated keys just for simplicity, but XXX let's see
     * if we actually need that for some interesting use cases in the future.
     */
    SortSupportData partition_att_sortsupp;

    /* 
     * scanKeys is the original index quals, including both the original
     * lb & ub.
     *
     * This is currently the same as those in
     * AQPProgressiveSampleScanSubplanState, but we keep a copy of the
     * references here so we won't need to find it deep down the plan state
     * tree.
     */
    ScanKey    scan_keys;
    
    /*
     * The first two slots are the lower bound and upper bound of the keys,
     * while the remaining ones are copies from the original index scan keys,
     * which will be only checked but not going to be used as scan keys.
     * (Ideally, they should not appear here for pswr as we might be curious to
     * know why the tuple is rejected).
     *
     * NB: _abt_preprocess_keys/_bt_preprocess_keys insist that the index
     * attrno must in non-decreasing order in the scan keys we pass to
     * the rescan function.
     *
     * [0] is index key >= the logical partition lb.
     * [1] is index key < the logical partition ub.
     *
     * For convenience, we also make both [0] and [1] to be row comparisons
     * even if the index only has one key since we will be free to store the
     * BTORDER_PROC (the three-way comparison function on a index key column
     * that returns 1/0/-1), which will be useful for us to perform sorting
     * in repartitioning process.
     *
     * Since the row comparison we build here always compares against the full
     * index key, it is safe to append any additional scankeys the original
     * indexqual has since it should have been in increasing order anyway.
     */
    ScanKey     partition_keys;

    /*
     * This represents the left-most partition if there're at least two
     * partitions in a phase.
     *
     * It is needed because the original lb may not exist or requires a
     * different comparison strategy/cross-type operator.
     */
    ScanKey     lmost_partition_keys;
    
    /*
     * This represents the right-most partition if there're at most two
     * partitions in a phase.
     *
     * Similar to above, this is needed because the original ub may not exist
     * or requires a different comparison strategy/cross-type operator.
     */
    ScanKey     rmost_partition_keys;

    int         n_scan_keys;

    int         n_partition_keys;

    int         n_lmost_partition_keys;

    int         n_rmost_partition_keys;

    int         lb_idx_in_partition_keys;

    int         ub_idx_in_partition_keys;

    int         ub_idx_in_lmost_partition;
    
    int         lb_idx_in_rmost_partition;
} AQPPSWRCtlSubplanInfoData;

typedef AQPPSWRCtlSubplanInfoData *AQPPSWRCtlSubplanInfo;

typedef struct AQPTableSamplerCtlData {
    float8      inv_prob;
    uint64      num_samples_fetched;
    bool        is_driver;
    uint64      sample_size;
} AQPTableSamplerCtlData;

typedef AQPTableSamplerCtlData *AQPTableSamplerCtl;

typedef struct AQPPSWRCtlInfoData {
    /* 
     * We add it here such that we don't have to store it within each
     * AQPApproxAggTransState object. Saves 8 bytes to fit everything into
     * one cache line per transtate...
     */
    struct AQPPSWRControlState *pswrctl;

    /*
     * Output from the sampler.
     */
    bool        rejected;

    float8      inv_prob;
    
    uint64      num_samples_fetched;

    uint64      next_kv_idx;

    Datum       *recorded_kv_pairs;

    uint32      *samples_height;

    /*
     * Index in subplan_info[] of the chosen PSWR index candidate.
     * This is not a physical table sampler id.
     */
    int         driver_subplan_id;

    /*
     * Physical table sampler state. subplan_info[] saves PSWR index candidates,
     * not join-table sampler nodes.
     */
    int         driver_sampler_id;
    int         pswr_driver_sampler_id;
    int         nsamplers;
    AQPTableSamplerCtl sampler_ctl;
    
    /*
     * Batch sampling currently owns AB-tree batch state per PSWR scan node.
     * Multiple PSWR sampler nodes in a join would each try to initialize and
     * advance batch partitions, which silently corrupts estimates.
     */
    void        *batch_sampling_pswr_node;

    /*
     * Input from pswrctl.
     */
    int         cur_plan_id; /* need to be set in aqp_pswr_scan_begin before other begins */

    uint64      sample_size;

    ScanKey     current_scan_keys;

    int         n_current_scan_keys;

    bool        want_partition_key;
    
    /*
     * These are set up during begin.
     */
    int         nsubplans;

    AQPPSWRCtlSubplanInfo subplan_info;

    int         height_max;
    int         height_min;
    float8      height_avg;

    float8      time_max;
    float8      time_min;
    float8      time_avg;

} AQPPSWRCtlInfoData;

typedef AQPPSWRCtlInfoData *AQPPSWRCtlInfo;

/* defined in aqp_pagg.h */
typedef struct AQPApproxAggTransState AQPApproxAggTransState;

typedef struct AQPApproxAggRollbackState
{
    uint8       flags;
    uint32      n;
    float8      mu;
    float8      var;
    float8      mu_phase;
    float8      var_phase;
    float8      mu_total;
    float8      height;
    float8      var_total;
    float8      var_0;
    float8      var_total_pre;
    float8      var_total_next;
    uint32      split_idx;
    uint32      dp_start_idx;
} AQPApproxAggRollbackState;

typedef struct AQPPSWRRollbackCheckpoint
{
    int         ntrans;
    uint64      total_samples_fetched;
    uint64      initial_and_uniform_sample_size;
    float8      progressive_last_ci;
    AQPApproxAggRollbackState *trans;
} AQPPSWRRollbackCheckpoint;

typedef struct AQPPSWRPlanProbeTransSnapshot
{
    AQPApproxAggRollbackState scalar;
    float8      mu_part0;
    float8      var_part0;
    uint32      n_part0;
    float8      height_part0;
} AQPPSWRPlanProbeTransSnapshot;

typedef struct AQPPSWRPlanProbeSnapshot
{
    bool        valid;
    int         plan_id;
    uint64      fetched;
    uint64      nintvls;
    uint64      score;
    uint64      prefix_len;
    float8      elapsed_ms;

    TupleTableSlot *slot;
    AQPPSWRPlanProbeTransSnapshot *trans;

    struct AQPPrefixStats *prefix_stats;
    Datum       *prefix_keys;
} AQPPSWRPlanProbeSnapshot;

typedef struct AQPPSWRControlState
{
    CustomScanState     css;
    
    /* points to the global param so we don't need to fetch on each exec call */
    AQPPSWRCtlInfo      pswrctl_info;

    bool                emptyres;

    bool                do_pswr;

    bool                do_plan_switching;
    
    int                 n_valid_subplans;

    int                 *valid_subplan_idx;

    ExprState           *sample_size_expr_state;

    ExprState           *initial_phase_sample_size_expr_state;

    ExprState           *step_sample_size_expr_state;

    ExprState           *err0_expr_state;

    ExprState           *confidence0_expr_state;
    
    /* We allocate the accumulation states for each node. */
    uint64              initial_phase_sample_size;

    uint64              initial_and_uniform_sample_size;

    uint64              initial_sample_size_total;

    uint64              step_sample_size;

    uint64              sample_budget;

    float8              err0;

    float8              relative_ci;

    float8              confidence0;

    bool                last_phase;

    int                 cur_phase;

    int                 cur_phase_n_partitions;

    int                 cur_partition;

    uint64              cur_phase_sample_size;

    uint64              cur_descent_partitions;

    uint64              leaf_page_sample_size;

    uint64              total_partitions;

    uint64              total_samples_fetched;

    int                 ntrans;

    /* information needed for DP */

    /*Datum               *bound_keys; //reused for result

    float8              *m;

    float8              *Sx; 

    float8              *Sxx;
    */
    uint64              nintvls;

    struct AQPPrefixStats *prefix_stats; /* using for optimal stratified sampling and pswr  */
    Datum               *prefix_keys; /* using for equal stratified sampling  */

    float8              *gpsa_opt_cost;

    float8              *gpsa_opt_height;

    float8              *gpsa_opt_sigma;

    float8              *gpsa_opt_cost2;

    int                 *gpsa_opt_prev_idx;

    Datum               *gpsa_opt_ub;

    uint64              *gpsa_opt_sample_size;

    float8              *gpsa_opt_sample_percent;

    /* information needed for progressively sampling in phase 2 and switch */

    bool                do_progressive_sampling;

    bool                do_switch_to_uniform;

    bool                progressive_current_phase_enabled;
    bool                batch_resume_start_progressive;
    bool                progressive_uniform_candidate;

    uint64              progressive_partition_budget_capacity;

    uint64              stratified_phase_sample_size;
    uint64              uniform_phase_sample_size;

    uint64              prev_stratified_phase_sample_size;

    float8              phase_start_ci;
    float8              progressive_last_ci;

    int                 stratified_probe_rounds;
    int                 progressive_bad_rounds;

    uint64              discarded_probe_samples;
    bool                progressive_bad_checkpoint_valid;
    struct AQPPSWRRollbackCheckpoint *progressive_bad_checkpoint;

    uint64              phase_remaining_budget;
    
    uint64              current_round_budget;

    uint64              *partition_total_budget;

    uint64              *partition_remaining_budget;
    
    uint64              *partition_round_budget;
    
    /* greedy info */

    int                 continue_descent; /* no: 0; yes: 1; cant: -1 */

    int                 continue_descent_dp;

    int                 descent_try_sample_size;

    int                 *dp_prev_idx;

    int                 dp_k_partitions;

    uint32              tree_level;

    /* DP result */
    /*float8              **dp;
    int                 **index; */

    /*int                 dp_count; */

    bool                grouped;

    Tuplestorestate     *grouped_results;
    TupleTableSlot      *grouped_output_slot;
    bool                grouped_results_ready;
    bool                grouped_resume_next_partition;

    AQPApproxAggTransState *transstate;
} AQPPSWRControlState;


#define AQPPSWRControlPathName "AQPPSWRControlPath"
#define AQPPSWRControlPathPrivateName "AQPPSWRControlPathPrivate"
#define AQPPSWRControlPrivateName "AQPPSWRControlPrivateName"
#define AQPPSWRControlName "AQPPSWRControl"
#define AQPPSWRControlStateName "AQPPSWRControlState"

#endif  /* AQP_PSWRCTL_NODE_H */
