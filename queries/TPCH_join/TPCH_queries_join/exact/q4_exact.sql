select
	count(o_orderkey) as order_count
from
	orders_60,
	lineitem_60
where
	o_orderkey = l_orderkey
	and o_orderdate >= date '1993-07-01'
	and o_orderdate < date '1993-07-01' + interval '3' month
	and l_commitdate < l_receiptdate
	and o_orderpriority = '1-URGENT';