\i default_settings.sql

--PSWR

set pswr_tree to off;
set batch_sampling to on;

select 
     approx_count(*) as y,
     approx_count_star_half_ci(0.95) as e
from flights_julianx10 tablesample pswr(100000000,3200, 8341.3, 0.95) 
where cancelled = 1 and (julian_date >= 2452158 and julian_date < 2452174);
