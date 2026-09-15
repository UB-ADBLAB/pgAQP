select
	sum(case
		when o_orderpriority < '3'
			then 1
		else 0
	end) as high_line_count,
	sum(case
		when o_orderpriority >= '3'
			then 1
		else 0
	end) as low_line_count
from
	orders_60,
	lineitem_60
where
	o_orderkey = l_orderkey
	and l_shipmode = 'MAIL'
	and l_commitdate < l_receiptdate
	and l_shipdate < l_commitdate
	and l_receiptdate >= date '1995-01-01'
	and l_receiptdate < date '1996-01-01';