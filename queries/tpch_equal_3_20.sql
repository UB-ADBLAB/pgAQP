\i default_settings.sql

--PSWR

set optimization_strategy to 3;

select approx_sum(l_extendedprice * (1-l_discount)) as sum_disc_price, approx_sum_half_ci(l_extendedprice * (1-l_discount), 0.95)
from lineitem_3_20 tablesample pswr(100000000,100000,1935672743.350552,0.95)
where shipdate_julian >= 2448623
and shipdate_julian < 2451179
and l_receiptdate - l_shipdate > 49;
