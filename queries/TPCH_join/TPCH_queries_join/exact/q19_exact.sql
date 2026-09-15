select
	sum(l_extendedprice * (1 - l_discount)) as revenue
from
	lineitem_60,
	part_60
where
	p_partkey = l_partkey
	and p_brand = 'Brand#23'
	and (
		(p_container >= 'LG' and p_container < 'LH')
		or
		(p_container >= 'MED' and p_container < 'MEE')
		or
		(p_container >= 'SM' and p_container < 'SN')
	)
	and l_quantity >= 10
	and l_quantity <= 20
	and p_size >= 1
	and p_size <= 15
	and l_shipmode >= 'AIR'
	and l_shipmode < 'AIS'
	and l_shipinstruct = 'DELIVER IN PERSON';