\timing on

--Exact

set max_parallel_workers_per_gather to 0;
set max_parallel_workers to 0;


--explain select sum(l_extendedprice * (1-l_discount)) as sum_disc_price 
--from lineitem_1_20 
--where shipdate_julian >= 2448623
--and shipdate_julian < 2451179
--and l_receiptdate - l_shipdate > 49;

select sum(l_extendedprice * (1-l_discount)) as sum_disc_price 
from lineitem_3_20 
where shipdate_julian >= 2448623
and shipdate_julian < 2451179
and l_receiptdate - l_shipdate > 49;
