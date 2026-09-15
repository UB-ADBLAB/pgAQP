select approx_sum(l_extendedprice * (1-l_discount)) as sum_disc_price, approx_sum_half_ci(l_extendedprice * (1-l_discount), 0.95)
from lineitem_3_20 tablesample pswr(100000000,%INI%,%CI%,0.95)
where shipdate_julian >= %D1%
and shipdate_julian < %D2%
and l_receiptdate - l_shipdate > 49;
