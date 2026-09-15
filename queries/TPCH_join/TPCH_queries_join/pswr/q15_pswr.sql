set pswr_chosen_plan_index to 0;
-- use index over l_shipdate

SELECT
    approx_sum(l.l_extendedprice * (1 - l.l_discount)) AS total_revenue,
    approx_sum_half_ci(l.l_extendedprice * (1 - l.l_discount), 0.95) AS total_revenue_ci
FROM
    lineitem_60 l TABLESAMPLE pswr(30000000, 100000, 0.05, 0.95)
    JOIN supplier_60 s TABLESAMPLE swr(1)
      ON s.s_suppkey = l.l_suppkey
WHERE
	l_shipdate >= date '1995-01-01'
	and l_shipdate < date '1997-04-01';
