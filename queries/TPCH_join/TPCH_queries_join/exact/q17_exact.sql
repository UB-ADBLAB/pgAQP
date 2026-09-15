select
	sum(l_extendedprice) / 7.0 as avg_yearly
from
	lineitem_60,
	part_60
where
	p_partkey = l_partkey
	and p_brand = 'Brand#23'
	and p_container = 'MED BOX'
	and l_quantity >= 10
	and l_quantity < 20;