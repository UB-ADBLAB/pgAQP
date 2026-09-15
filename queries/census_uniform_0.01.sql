\i default_settings.sql

--PSWR

set optimization_strategy to 1;
set dp_intervals_count to 1;

select 
     approx_count(*) as y,
     approx_count_star_half_ci(0.95) as e
from census_income_duplicate tablesample pswr(100000000,18800, 784100, 0.95) 
where hours_per_week >= 1 and hours_per_week < 100
and income is true;
