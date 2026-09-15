\timing on

--Exact

set max_parallel_workers_per_gather to 0;
set max_parallel_workers to 0;

--explain select count(*) 
--from census_income_duplicate
--where hours_per_week >= 1 and hours_per_week < 100 and income is true;

select count(*) 
from census_income_duplicate
where hours_per_week >= 1 and hours_per_week < 100 and income is true;


