#ifndef AQP_PLANNER_H
#define AQP_PLANNER_H

#include <nodes/extensible.h>
#include <nodes/execnodes.h>
#include <nodes/pathnodes.h>

/* in aqp_planner.c */
extern void aqp_setup_planner_hook(void);
extern void aqp_rewrite_sample_prob_dummy_var_for_explain(PlanState *state);
extern TableSampleClause *aqp_find_swr_sampler_in_plan(PlannerInfo *root);

/* in aqp_samplepath.c */
extern void aqp_setup_sample_path_hook(void);
extern List *aqp_reparameterize_custom_path_by_child(PlannerInfo *root,
                                                     List *custom_private,
                                                     RelOptInfo *child_rel);

/* in aqp_swrscan.c */
extern void aqp_setup_swr(void);

/* in aqp_approx_agg_rewrite.c */
extern bool aqp_check_and_rewrite_approx_agg(Query *query);

/* in aqp_pagg.c */
extern void aqp_setup_create_upper_paths_hook(void);

/* in aqp_pswrctl_node.h */
extern void aqp_setup_pswrctl_node(void);

extern bool aqp_fn_oid_cached;
extern Oid aqp_sample_prob_oid;
extern Oid aqp_running_sample_size_oid;
extern Oid aqp_running_sample_budget_oid;
extern Oid aqp_running_state_id_oid;
extern Oid aqp_swr_tsm_handler_oid;
extern Oid aqp_pswr_tsm_handler_oid;
extern Oid aqp_sum_float8_oid;
extern Oid aqp_float8_mul_opno;
extern Oid aqp_float8_mul_funcid;
extern Oid aqp_float8_div_opno;
extern Oid aqp_float8_div_funcid;
extern Oid aqp_float48_div_opno;
extern Oid aqp_float48_div_funcid;
extern Oid aqp_erf_inv_oid;

/* 
 * TODO we might end up with a large number of approx_xxx aggregation
 * functions so we should probably make these a hash table. (e.g., use
 * lib/simplehash.h).
 */
extern Oid aqp_approx_sum_oid;
extern Oid aqp_approx_sum_half_ci_oid;
extern Oid aqp_approx_count_oid;
extern Oid aqp_approx_count_half_ci_oid;
extern Oid aqp_approx_progressive_count_half_ci_oid;
extern Oid aqp_approx_count_any_oid;
extern Oid aqp_approx_count_any_half_ci_oid;

extern Oid aqp_float8_accum_oid;
extern Oid aqp_clt_half_ci_final_func_oid;

extern Oid aqp_progressive_float8_accum_oid;
extern Oid aqp_progressive_clt_half_ci_final_func_oid;

extern Oid aqp_approx_sum_internal_oid;
extern Oid aqp_approx_sum_internal_accum_oid;
extern Oid aqp_approx_sum_internal_final_oid;
extern Oid aqp_approx_sum_clt_half_ci_internal_final_oid;

#endif      /* AQP_PLANNER_H */
