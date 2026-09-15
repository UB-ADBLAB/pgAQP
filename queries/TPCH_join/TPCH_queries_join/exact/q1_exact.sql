select
	sum(l_quantity) as sum_qty,
	sum(l_extendedprice) as sum_base_price,
	sum(l_extendedprice * (1 - l_discount)) as sum_disc_price,
	sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) as sum_charge,
	count(*) as count_order
from
	lineitem_60
where
	l_shipdate <= date '1998-12-01' - interval '90' day
	and l_returnflag = 'A'
	and l_linestatus = 'F';