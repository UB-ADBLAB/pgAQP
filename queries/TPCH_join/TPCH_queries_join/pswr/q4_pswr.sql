set pswr_chosen_plan_index to 0;
-- use index over l_shipdate

select
	approx_count(o_orderkey) as order_count,
  approx_count_star_half_ci(0.95) AS count_ci
FROM lineitem_60 l TABLESAMPLE pswr(30000000, 100000, 0.05, 0.95)
JOIN orders_60 o TABLESAMPLE swr(1)
  ON o.o_orderkey = l.l_orderkey
WHERE o.o_orderdate >= date '1993-07-01'
  AND o.o_orderdate < date '1993-07-01' + interval '3' month
  AND l.l_commitdate < l.l_receiptdate
  AND o.o_orderpriority = '1-URGENT';