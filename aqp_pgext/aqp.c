#include "aqp.h"

#include <utils/guc.h>

#include <miscadmin.h>

#include "aqp_planner.h"
#include "aqp_swrscan.h"
#include "aqp_sample_scan_state.h"
#include "aqp_explain.h"

PG_MODULE_MAGIC;

extern void _PG_init(void);

bool aqp_tablesample_swr_ask_for_samples_attempted;
int aqp_pswr_chosen_plan_index;
bool aqp_enable_pswr;
int aqp_initial_phase_sample_size;
bool aqp_enable_new_agg_impl;
int aqp_dp_intervals_count;
float8 aqp_dp_linear_coefficient;
int aqp_optimization_strategy;
bool aqp_batch_sampling;
int aqp_subtree_sample_size;
float8 aqp_subtree_stop_k;
bool aqp_batch_sampling_dp;
bool aqp_pswr_tree;
bool aqp_pswr_to_swr;
bool aqp_enable_pswr_switch_to_uniform;
int aqp_progressive_round_size;

/* new change for switch new_agg_impl */
bool aqp_use_new_agg_impl;

static void aqp_add_guc_entries(void);

void _PG_init(void)
{
    /*elog(NOTICE, "loading aqp extension"); */
    aqp_add_guc_entries();
    aqp_setup_pswrctl_node();
    aqp_setup_swr();
    aqp_setup_sample_path_hook();
    aqp_setup_create_upper_paths_hook();
    aqp_setup_planner_hook();
    aqp_setup_hooks_for_explain();
}

static void
aqp_add_guc_entries(void)
{
    DefineCustomBoolVariable("enable_new_agg_impl",
                             "enable_new_agg_impl",
                             "sets whether to enable the new"
                             "aggregation operator implementation",
                             &aqp_enable_new_agg_impl,
                             false,
                             PGC_USERSET,
                             0,
                             NULL, NULL, NULL);

    DefineCustomBoolVariable("tablesample_swr_ask_for_samples_attempted",
                             "tablesample_swr_ask_for_samples_attempted",
                             "sets whether tablesample swr should issue a "
                             "NOTICE level message of \"N = <Number of "
                             "samples attempted>\"",
                             &aqp_tablesample_swr_ask_for_samples_attempted,
                             false,
                             PGC_USERSET,
                             0,
                             NULL, NULL, NULL);

    DefineCustomIntVariable("pswr_chosen_plan_index",
                            "pswr_chosen_plan_index",
                            "sets the index of the plan to use by default in pswr (debug only)",
                            &aqp_pswr_chosen_plan_index,
                            -1,
                            -1,
                            INT_MAX,
                            PGC_USERSET,
                            0,
                            NULL, NULL, NULL);

    DefineCustomBoolVariable("enable_pswr",
                             "enable_pswr",
                             "sets whether to enable progressive sampling with replacement",
                             &aqp_enable_pswr,
                             false,
                             PGC_USERSET,
                             0,
                             NULL, NULL, NULL);

    DefineCustomIntVariable("initial_phase_sample_size",
                             "initial_phase_sample_size",
                             "sets the sample size for the initial phase",
                             &aqp_initial_phase_sample_size,
                             -1,
                             -1,
                             INT_MAX,
                             PGC_USERSET,
                             0,
                             NULL, NULL, NULL);

    DefineCustomIntVariable("dp_intervals_count",
                            "dp_intervals_count",
                            "sets the number of intervals for DP",
                            &aqp_dp_intervals_count,
                            100,
                            1,
                            INT_MAX,
                            PGC_USERSET,
                            0,
                            NULL, NULL, NULL);

    DefineCustomRealVariable("dp_linear_coefficient",
                             "dp_linear_coefficient",
                             "sets the linear coefficient for DP to determine K",
                             &aqp_dp_linear_coefficient,
                             100.0,
                             0.0,
                             get_float8_infinity(),
                             PGC_USERSET,
                             0,
                             NULL, NULL, NULL);

    DefineCustomIntVariable("optimization_strategy",
                             "optimization_strategy",
                             "sets which optimization strategy will be used",
                             &aqp_optimization_strategy,
                             1,
                             -1,
                             INT_MAX,
                             PGC_USERSET,
                             0,
                             NULL, NULL, NULL);
    DefineCustomBoolVariable("batch_sampling",
                             "batch_sampling",
                             "sets whether to use batch sampling in abtree",
                             &aqp_batch_sampling,
                             false,
                             PGC_USERSET,
                             0,
                             NULL, NULL, NULL);
    DefineCustomIntVariable("subtree_sample_size",
                            "subtree_sample_size",
                            "sets the sample size for each partition",
                            &aqp_subtree_sample_size, 
                            30,
                            1,
                            INT_MAX,
                            PGC_USERSET,
                            0,
                            NULL, NULL, NULL);
    DefineCustomRealVariable("subtree_stop_k",
                             "subtree_stop_k",
                             "sets the stop k for splitting subtrees",
                             &aqp_subtree_stop_k,
                             0.004,
                             0.0,
                             get_float8_infinity(),
                             PGC_USERSET,
                             0,
                             NULL, NULL, NULL);
    DefineCustomBoolVariable("batch_sampling_dp",
                             "batch_sampling_dp",
                             "sets whether to use batch sampling with dp in abtree",
                             &aqp_batch_sampling_dp,
                             false,
                             PGC_USERSET,
                             0,
                             NULL, NULL, NULL); 
    DefineCustomBoolVariable("pswr_tree",
                             "pswr_tree",
                             "sets whether to use tree height in dp in abtree",
                             &aqp_pswr_tree,
                             false,
                             PGC_USERSET,
                             0,
                             NULL, NULL, NULL); 
    DefineCustomBoolVariable("pswr_to_swr",
                             "pswr_to_swr",
                             "sets whether to switch to swr when n0 is small",
                             &aqp_pswr_to_swr,
                             false,
                             PGC_USERSET,
                             0,
                             NULL, NULL, NULL); 
    
    DefineCustomBoolVariable("enable_pswr_switch_to_uniform",
                             "enable_pswr_switch_to_uniform",
                             "sets whether to progressively sample and switch to uniform when pswr does not work",
                             &aqp_enable_pswr_switch_to_uniform,
                             false,
                             PGC_USERSET,
                             0,
                             NULL, NULL, NULL);
    DefineCustomIntVariable("progressive_round_size",
                            "progressive_round_size",
                            "sets the sample size for progressively sampling in phase 2",
                            &aqp_progressive_round_size, 
                            100000,
                            1,
                            INT_MAX,
                            PGC_USERSET,
                            0,
                            NULL, NULL, NULL);
}
