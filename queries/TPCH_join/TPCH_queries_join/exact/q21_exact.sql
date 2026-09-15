select
	count(*) as numwait
from
	supplier_60,
	lineitem_60 l1,
	orders_60,
	nation_60
where
	s_suppkey = l1.l_suppkey
	and o_orderkey = l1.l_orderkey
	and o_orderstatus = 'F'
	and l1.l_receiptdate > l1.l_commitdate
	and s_nationkey = n_nationkey
	and n_name = 'GERMANY';