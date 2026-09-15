select sum(l_extendedprice * (1-l_discount)) as sum_disc_price, count(distinct shipdate_julian)
from lineitem_3_20 
where shipdate_julian >= %D1%
and shipdate_julian < %D2%
and l_receiptdate - l_shipdate > 49;
