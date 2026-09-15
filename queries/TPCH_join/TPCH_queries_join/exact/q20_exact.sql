select
	count(*) as cnt
from
	supplier_60,
	nation_60,
	partsupp_60,
	part_60,
	lineitem_60
where
	s_nationkey = n_nationkey
	and s_suppkey = ps_suppkey
	and ps_partkey = p_partkey
	and l_partkey = ps_partkey
	and l_suppkey = ps_suppkey
	and p_name LIKE 'green%'
	AND l_shipdate >= date '1995-01-01'
	AND l_shipdate < date '1997-01-01'
	AND n_name = 'CHINA'
	AND ps_availqty > 6000;
