\i default_settings.sql

--PSWR

set pswr_tree to off;
set batch_sampling to on;

select 
     approx_count(*) as y,
     approx_count_star_half_ci(0.95) as e
from census_income_duplicate tablesample pswr(100000000,18800, 784100, 0.95) 
where hours_per_week >= 1 and hours_per_week < 100
and income is true;
