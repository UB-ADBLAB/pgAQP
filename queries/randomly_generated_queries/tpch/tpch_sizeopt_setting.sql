set enable_new_agg_impl to on;
set abtree_lock_lrpath_during_sampling to on;
set dp_intervals_count to 100;
set dp_linear_coefficient to 100.0;

/* select pg_backend_pid(); */

/* set batch_sampling to true; */
/* set batch_sampling_dp to true; */
set subtree_stop_k to 0.004;
set subtree_sample_size to 600;

set statement_timeout to '5min'; 

set optimization_strategy to 2;
/* set pswr_tree to on; */
