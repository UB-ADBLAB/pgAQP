#ifndef AQP_PAGG_H
#define AQP_PAGG_H

#include "aqp.h"

#include <nodes/nodes.h>

struct AQPPSWRControlState;
struct AQPPSWRCtlInfoData;

typedef struct AQPApproxAggTransState
{
    struct AQPPSWRCtlInfoData  *pswrctl_info;


    uint8       flags;

    /* group by info */

    bool        grouped;

    bool        is_template;

    Datum       *grouped_kv_values;
    bool        *grouped_kv_value_set;
    uint64      grouped_kv_capacity;

    /* Number of non-NULL sampled expression values */
    uint32      n;
    
    /* 16 bytes */

    /* 
     * Sum of non-NULL sampled expression values.
     *
     * Let X_i = e(t_i) / p_i. Then
     * Sx = \sum_{i=1}^n X_i
     */
    /* float8      Sx; */
    
    /* 
     * Sum of squared distance to the sample mean.
     *
     * Let X_i = e(t_i) / p_i. Then
     * Sxx = \sum_{i=1}^n (X_i - Sx / n)^2
     *
     */
    /* float8      Sxx; */

    float8      mu; /* mu = Sx/n */

    float8      var; /* var = Sxx/(n*(n-1)) */

    /* 32 bytes */
    
    /*
     * This is the cumulative sum of Sx / N for each partition in the current
     * phase, where N is the per-partition sample size, found from
     * pswrctl_info. (not the accepted sample size stored in n!).
     *
     * This is the unbiased sum estimation of the values derived from the
     * samples in the entire phase.
     */
    float8      mu_phase;
    
    /*
     * This is the cumulative sum of Sxx / (N * (N - 1)) for each partition in
     * the current phase, where N is the per-partition sample size, found from
     * pswrctl_info. (not the accepted sample size stored in n!).
     *
     * This is the unbiased variance estimation of mu_phase.
     */
    float8      var_phase;

    /* 48 bytes */
    
    /*
     * This is the final estimator by doing a weighted average of all
     * estimators provided by each phase as follows, where N_i is the sample
     * size for phase i:
     *
     * mu = \sum_i mu_i * N_i / (\sum_i N_i)
     */
    float8      mu_total;
    
    float8      height;
    /*
     * This is the final variance by doing a weighted average of all estimators
     * provided by each phase as follows, where N_i is the sample size for
     * phase i:
     *
     * var = \sum var_i * (N_i / \sum_i N_i)^2
     */
    float8      var_total;

    float8      var_0;

    float8      var_total_pre;

    float8      var_total_next;

    /* 64 bytes */

    /*
     * sum estitmation and unbiased variance estimation and sample size 
     * for each partition and the inital phase. 
     */

    float8     *mu_part;

    float8     *var_part;

    uint32     *n_part;

    float8     *height_part;

    uint32     *done; /* yes-1, no-0*/
    
    /* uint32     *sub_idx; */

    uint32     *sort_idx;

    float8     *weights;

    uint32        *used;

    uint32     split_idx;

    uint32     dp_start_idx;
} AQPApproxAggTransState;

StaticAssertDecl(sizeof(AQPApproxAggTransState) <= 256,
                 "sizeof(AQPApproxAggTransState) > 256");

/* 32-byte, half of a cache line. */
typedef struct AQPPrefixStats
{
    Datum       key;
    uint64      m;
    float8      Sx;
    float8      Sxx;
    float8      mu;
    float8      var;
    uint32      h;
} AQPPrefixStats;

#define AQP_APPROX_AGG_TRANSFLAG_WANT_STATISTICS 0x1
#define AQP_APPROX_AGG_TRANSFLAG_FINALIZED 0x2
#define AQP_INTERNAL_PAGE_STATISTICS 0x3
#define AQP_LEAF_PAGE_STATISTICS 0x4

#define AQP_OPTIMIZATION_STRATEGY_DP 1
#define AQP_OPTIMIZATION_STRATEGY_OPT_STRAT 2
#define AQP_OPTIMIZATION_STRATEGY_EQUAL_STRAT 3

extern void aqp_approx_sum_transstate_init(struct AQPPSWRControlState *state,
                                           AQPApproxAggTransState *transstate);
extern void aqp_approx_sum_transstate_finalize(
    AQPApproxAggTransState *transstate);
extern void aqp_approx_sum_transstate_reset_partition(
    AQPApproxAggTransState *transstate);
extern void aqp_approx_sum_transstate_reset_phase(
    AQPApproxAggTransState *transstate);
extern void aqp_gpsa_compute_metainfo(struct AQPPSWRControlState *pswrctl);
extern void aqp_gpsa_compute_metainfo_optimized_stratified(struct AQPPSWRControlState *pswrctl);
extern void aqp_gpsa_compute_metainfo_equal_stratified(struct AQPPSWRControlState *pswrctl);
extern void aqp_gpsa_compute_metainfo_height(struct AQPPSWRControlState *pswrctl);
extern void aqp_approx_sum_check_ci(struct AQPPSWRControlState *pswrctl);
extern void aqp_continue_descent_dp(struct AQPPSWRControlState *pswrctl);
extern void aqp_pswr_cap_allocation_to_remaining_budget(
    struct AQPPSWRControlState *pswrctl);

#endif  /* AQP_PAGG_H */
