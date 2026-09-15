set pswr_chosen_plan_index to 0;
-- use index over l_shipdate

SELECT
    approx_sum(l.l_quantity) AS total_quantity,
    approx_sum_half_ci(l.l_quantity, 0.95) AS total_quantity_ci
FROM
    lineitem_60 l TABLESAMPLE pswr(30000000, 100000, 0.05, 0.95)
    JOIN orders_60 o TABLESAMPLE swr(1)
      ON o.o_orderkey = l.l_orderkey
    JOIN customer_60 c TABLESAMPLE swr(1)
      ON c.c_custkey = o.o_custkey
WHERE
    l.l_quantity > 10;
