select
	sum(l_quantity) as total_quantity
from
	customer_60,
	orders_60,
	lineitem_60
where
	c_custkey = o_custkey
	and o_orderkey = l_orderkey
	and l_quantity > 10;