\timing on

--Exact

set max_parallel_workers_per_gather to 0;
set max_parallel_workers to 0;

--explain select count(*) 
--from intel_lab_1000
--where date >= '2004-02-28' and date < '2004-04-05' and temperature > 27;

select count(*) 
from intel_lab_1000
where date >= '2004-02-28' and date < '2004-04-05' and temperature > 27;


