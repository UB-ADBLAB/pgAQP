select
	count(ps_suppkey) as supplier_cnt
from
	partsupp_60,
	part_60,
	supplier_60
where
	p_partkey = ps_partkey
	and ps_suppkey = s_suppkey
	and p_brand <> 'Brand#12'
	and p_type not like 'SMALL%'
	and p_brand = 'Brand#23'
	and p_type = 'ECONOMY ANODIZED STEEL'
	and p_size >= 10
	and p_size < 20
	and s_comment not like '%Customer%Complaints%';