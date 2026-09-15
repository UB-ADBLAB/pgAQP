select count(*), count(distinct hours_per_week) as ndv
from census_income_duplicate
where hours_per_week >= %D1% and hours_per_week < %D2%;
