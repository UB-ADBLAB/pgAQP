set pswr_chosen_plan_index to -1;
-- auto

select
    approx_sum(l.l_extendedprice * (1 - l.l_discount)) AS revenue,
    approx_sum_half_ci(l.l_extendedprice * (1 - l.l_discount), 0.95) AS revenue_ci
from
    part_60 p TABLESAMPLE pswr(30000000, 100000, 0.05, 0.95)
    JOIN lineitem_60 l TABLESAMPLE swr(1)
      ON p.p_partkey = l.l_partkey
where
	p_brand = 'Brand#23'
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