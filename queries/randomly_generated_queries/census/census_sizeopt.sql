select 
    approx_count(*) as y,
    approx_count_star_half_ci(0.95) as e
from census_income_duplicate tablesample pswr(100000000,%INI%, %CI%, 0.95)
where hours_per_week >= %D1% and hours_per_week < %D2% and income is true;
