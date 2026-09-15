select
	sum(l_extendedprice * (1 - l_discount)) as revenue
from
	customer_60,
	orders_60,
	lineitem_60,
	nation_60
where
	c_custkey = o_custkey
	and l_orderkey = o_orderkey
	and o_orderdate >= date '1995-01-01'
	and o_orderdate < date '1995-01-01' + interval '3' month
	and l_returnflag = 'R'
	and c_nationkey = n_nationkey;