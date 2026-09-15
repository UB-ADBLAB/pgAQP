select
	count(*) as matching_supplier_part_count
from
	part_60,
	supplier_60,
	partsupp_60,
	nation_60,
	region_60
where
	p_partkey = ps_partkey
	and s_suppkey = ps_suppkey
	and p_type like '%BRASS'
	and s_nationkey = n_nationkey
	and n_regionkey = r_regionkey
	and r_name = 'EUROPE';