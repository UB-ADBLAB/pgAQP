#ifndef AQP_H
#define AQP_H

#include <postgres.h>
#include <math.h>

/*
 * XXX: We must build against the customized PG 13.1 with AB-tree.
 * TODO: The plan is to move all additional support out of PG 13.1 and
 * support higher PG versions.
 */
#ifndef HAS_BUILTIN_ABTREE_SUPPORT
#error  "This extension must be compiled against PG 13.1 with builtin AB-tree support"
#endif

#include <fmgr.h>

/*
 * TODO this is fixed to aqp right now, which makes the module not
 * relocatable. Does it make sense to make this relocatable in the future?
 */
#define AQP_SCHEMA "aqp"

/*
 * GUC variables
 */
extern bool aqp_tablesample_swr_ask_for_samples_attempted;
extern int aqp_pswr_chosen_plan_index; /* for debugging only */
extern bool aqp_enable_pswr;
extern int aqp_initial_phase_sample_size;
extern bool aqp_enable_new_agg_impl;
extern int aqp_dp_intervals_count;
extern double aqp_dp_linear_coefficient;
extern int aqp_optimization_strategy;
extern bool aqp_batch_sampling;
extern int aqp_subtree_sample_size;
extern double aqp_subtree_stop_k;
extern bool aqp_batch_sampling_dp;
extern bool aqp_pswr_tree;
extern bool aqp_pswr_to_swr;

extern bool aqp_enable_pswr_switch_to_uniform;
extern int aqp_progressive_round_size;

/* new change for switch new_agg_impl */
extern bool aqp_use_new_agg_impl;

#endif      /* AQP_H */

