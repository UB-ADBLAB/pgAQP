set pswr_chosen_plan_index to -1;
-- auto

SELECT
    approx_sum(ps.ps_supplycost * ps.ps_availqty) AS value,
    approx_sum_half_ci(ps.ps_supplycost * ps.ps_availqty, 0.95) AS value_ci
FROM
    partsupp_60 ps TABLESAMPLE pswr(30000000, 100000, 0.05, 0.95)
    JOIN supplier_60 s TABLESAMPLE swr(1)
      ON ps.ps_suppkey = s.s_suppkey
    JOIN nation_60 n TABLESAMPLE swr(1)
      ON s.s_nationkey = n.n_nationkey
WHERE
    n.n_name = 'GERMANY';
