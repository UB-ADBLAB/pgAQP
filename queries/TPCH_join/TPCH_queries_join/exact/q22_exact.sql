select
	count(*) as numcust,
	sum(c_acctbal) as totacctbal
from
	customer_60
where
	substring(c_phone from 1 for 2) = '13'
	and c_acctbal > 0;
