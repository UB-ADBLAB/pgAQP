select count(*), count(distinct julian_date) as ndv
from flights_julianx10
where cancelled = 1 and (julian_date >= %D1% and julian_date < %D2%);
