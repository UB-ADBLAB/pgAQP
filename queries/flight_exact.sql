\timing on

--Exact

-- PostgreSQL will choose index scan by default if we build an AB-tree on
-- julian_date. To test exact with full scan, uncomment the following three
-- lines.
--set enable_indexscan to off;
--set enable_indexonlyscan to off;
--set enable_bitmapscan to off;

set max_parallel_workers_per_gather to 0;
set max_parallel_workers to 0;


--explain select count(*) 
--from flights_julianx10
--where cancelled = 1 and (julian_date >= 2452158 and julian_date < 2452174);

select count(*) 
from flights_julianx10
where cancelled = 1 and (julian_date >= 2452158 and julian_date < 2452174);
