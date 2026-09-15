select
	sum(l_extendedprice * (1 - l_discount)) as total_revenue
from
	supplier_60,
	lineitem_60
where
	s_suppkey = l_suppkey
	and l_shipdate >= date '1995-01-01'
	and l_shipdate < date '1997-04-01';
