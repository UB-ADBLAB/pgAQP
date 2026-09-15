select
	count(*) as matching_order_count
from
	customer_60,
	orders_60
where
	c_custkey = o_custkey
	and o_comment not like '%special%requests%';