set pswr_chosen_plan_index to 0;
-- use index over l_shipdate

SELECT
    approx_sum(l_extendedprice * l_discount) AS revenue,
    approx_sum_half_ci(l_extendedprice * l_discount, 0.95) AS revenue_ci
FROM
    lineitem_60 TABLESAMPLE pswr(30000000, 100000, 0.05, 0.95)
WHERE
    l_shipdate >= date '1994-01-01'
    AND l_shipdate < date '1997-01-01'
    AND l_discount >= 0.07
	AND l_discount <=  0.09
    AND l_quantity < 24;
