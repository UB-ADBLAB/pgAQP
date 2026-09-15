set pswr_chosen_plan_index to 0;
-- use index over l_shipdate

-- High line count only
SELECT
    approx_count(*) AS high_line_count,
    approx_count_star_half_ci(0.95) AS count_ci
FROM
    lineitem_60 l TABLESAMPLE pswr(30000000, 100000, 0.05, 0.95)
    JOIN orders_60 o TABLESAMPLE swr(1)
      ON o.o_orderkey = l.l_orderkey
WHERE
    l.l_shipmode = 'MAIL'
    AND l.l_commitdate < l.l_receiptdate
    AND l.l_shipdate < l.l_commitdate
    AND l.l_receiptdate >= date '1995-01-01'
    AND l.l_receiptdate < date '1996-01-01'
    AND o.o_orderpriority < '3';

-- Low line count only
SELECT
    approx_count(*) AS low_line_count,
    approx_count_star_half_ci(0.95) AS count_ci
FROM
    lineitem_60 l TABLESAMPLE pswr(30000000, 100000, 0.05, 0.95)
    JOIN orders_60 o TABLESAMPLE swr(1)
      ON o.o_orderkey = l.l_orderkey
WHERE
    l.l_shipmode = 'MAIL'
    AND l.l_commitdate < l.l_receiptdate
    AND l.l_shipdate < l.l_commitdate
    AND l.l_receiptdate >= date '1995-01-01'
    AND l.l_receiptdate < date '1996-01-01'
    AND o.o_orderpriority >= '3';
