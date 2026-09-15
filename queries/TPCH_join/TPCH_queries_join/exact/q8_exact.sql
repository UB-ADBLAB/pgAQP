select
	sum(case
		when n2.n_name = 'GERMANY' then l_extendedprice * (1 - l_discount)
		else 0
	end) / sum(l_extendedprice * (1 - l_discount)) as mkt_share
from
	part_60,
	supplier_60,
	lineitem_60,
	orders_60,
	customer_60,
	nation_60 n1,
	nation_60 n2,
	region_60
where
	p_partkey = l_partkey
	and s_suppkey = l_suppkey
	and l_orderkey = o_orderkey
	and o_custkey = c_custkey
	and c_nationkey = n1.n_nationkey
	and n1.n_regionkey = r_regionkey
	and r_name = 'EUROPE'
	and s_nationkey = n2.n_nationkey
	and o_orderdate >= date '1995-01-01'
	and o_orderdate < date '1998-01-01'
	and p_type = 'ECONOMY ANODIZED STEEL';
