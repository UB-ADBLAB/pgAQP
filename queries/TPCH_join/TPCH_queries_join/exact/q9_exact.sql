select
	sum(l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity) as sum_profit
from
	part_60,
	supplier_60,
	lineitem_60,
	partsupp_60,
	orders_60,
	nation_60
where
	s_suppkey = l_suppkey
	and ps_suppkey = l_suppkey
	and ps_partkey = l_partkey
	and p_partkey = l_partkey
	and o_orderkey = l_orderkey
	and s_nationkey = n_nationkey
	and p_name like '%green%'
	and n_name = 'CHINA'
	and o_orderdate >= date '1995-01-01'
	and o_orderdate <= date '1995-12-31';