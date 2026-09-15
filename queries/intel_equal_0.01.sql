\i default_settings.sql

--PSWR

set optimization_strategy to 3;

select 
     approx_count(*) as y,
     approx_count_star_half_ci(0.95) as e
from intel_lab_1000 tablesample pswr(100000000,7400, 5792300, 0.95) 
where date >= '2004-02-28' and date < '2004-04-05' and temperature > 27;

