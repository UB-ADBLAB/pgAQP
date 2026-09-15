select 
     approx_count(*) as y,
         approx_count_star_half_ci(0.95) as e
        from flights_julianx10 tablesample pswr(100000000,%INI%, %CI%, 0.95)
where cancelled = 1 and (julian_date >= %D1% and julian_date < %D2%);
