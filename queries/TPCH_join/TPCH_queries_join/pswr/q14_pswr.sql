set pswr_chosen_plan_index to 0;
-- use index over l_shipdate

-- Promo revenue only
SELECT
    approx_sum(l.l_extendedprice * (1 - l.l_discount)) AS promo_revenue,
    approx_sum_half_ci(l.l_extendedprice * (1 - l.l_discount), 0.95) AS promo_revenue_ci
FROM
    lineitem_60 l TABLESAMPLE pswr(30000000, 100000, 0.05, 0.95)
    JOIN part_60 p TABLESAMPLE swr(1)
      ON l.l_partkey = p.p_partkey
WHERE
    l.l_shipdate >= date '1995-09-01'
    AND l.l_shipdate < date '1995-10-01'
    AND p.p_type >= 'PROMO'
    AND p.p_type < 'PROMP';

-- Total revenue only
SELECT
    approx_sum(l.l_extendedprice * (1 - l.l_discount)) AS total_revenue,
    approx_sum_half_ci(l.l_extendedprice * (1 - l.l_discount), 0.95) AS total_revenue_ci
FROM
    lineitem_60 l TABLESAMPLE pswr(30000000, 100000, 0.05, 0.95)
    JOIN part_60 p TABLESAMPLE swr(1)
      ON l.l_partkey = p.p_partkey
WHERE
    l.l_shipdate >= date '1995-09-01'
    AND l.l_shipdate < date '1995-10-01';
