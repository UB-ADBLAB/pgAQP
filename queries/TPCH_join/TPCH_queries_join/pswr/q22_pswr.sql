set pswr_chosen_plan_index to 1;
-- use index over custkey_idx

SELECT
    approx_count(*) AS numcust,
    approx_count_star_half_ci(0.95) AS count_ci
FROM
    customer_60 TABLESAMPLE pswr(30000000, 100000, 0.05, 0.95)
WHERE
    substring(c_phone from 1 for 2) = '13'
    AND c_acctbal > 0;

SELECT
    approx_sum(c_acctbal) AS totacctbal,
    approx_sum_half_ci(c_acctbal, 0.95) AS totacctbal_ci
FROM
    customer_60 TABLESAMPLE pswr(30000000, 100000, 0.05, 0.95)
WHERE
    substring(c_phone from 1 for 2) = '13'
    AND c_acctbal > 0;
