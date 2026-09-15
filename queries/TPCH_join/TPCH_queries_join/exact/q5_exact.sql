select
	sum(l_extendedprice * (1 - l_discount)) as revenue
from
	customer_60,
	orders_60,
	lineitem_60,
	supplier_60,
	nation_60,
	region_60
where
	c_custkey = o_custkey
	and l_orderkey = o_orderkey
	and l_suppkey = s_suppkey
	and c_nationkey = s_nationkey
	and s_nationkey = n_nationkey
	and n_regionkey = r_regionkey
	and r_name = 'ASIA'
	and o_orderdate >= date '1994-01-01'
    AND o_orderdate < date '1995-01-01'
	and n_name = 'CHINA';
