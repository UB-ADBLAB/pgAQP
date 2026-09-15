select
	sum(ps_supplycost * ps_availqty) as value
from
	partsupp_60,
	supplier_60,
	nation_60
where
	ps_suppkey = s_suppkey
	and s_nationkey = n_nationkey
	and n_name = 'GERMANY';