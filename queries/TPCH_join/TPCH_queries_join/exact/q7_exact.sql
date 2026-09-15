select
	sum(l_extendedprice * (1 - l_discount)) as revenue
from
	supplier_60,
	lineitem_60,
	orders_60,
	customer_60,
	nation_60 n1,
	nation_60 n2
where
	s_suppkey = l_suppkey
	and o_orderkey = l_orderkey
	and c_custkey = o_custkey
	and s_nationkey = n1.n_nationkey
	and c_nationkey = n2.n_nationkey
	and (
		(n1.n_name = 'UNITED STATES' and n2.n_name = 'CHINA')
		or
		(n1.n_name = 'CHINA' and n2.n_name = 'UNITED STATES')
	)
	and l_shipdate >= date '1995-01-01'
	and l_shipdate < date '1997-01-01';