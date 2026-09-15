set pswr_chosen_plan_index to 0;
-- use index over l_shipdate

SELECT
    approx_sum(l.l_extendedprice * (1 - l.l_discount)) AS revenue,
    approx_sum_half_ci(l.l_extendedprice * (1 - l.l_discount), 0.95) AS revenue_ci
FROM
    lineitem_60 l TABLESAMPLE pswr(30000000, 100000, 0.05, 0.95)
    JOIN orders_60 o TABLESAMPLE swr(1)
        ON l.l_orderkey = o.o_orderkey
    JOIN customer_60 c TABLESAMPLE swr(1)
        ON c.c_custkey = o.o_custkey
    JOIN supplier_60 s TABLESAMPLE swr(1)
        ON l.l_suppkey = s.s_suppkey
    JOIN nation_60 n TABLESAMPLE swr(1)
        ON s.s_nationkey = n.n_nationkey
    JOIN region_60 r TABLESAMPLE swr(1)
        ON n.n_regionkey = r.r_regionkey
WHERE
    c.c_nationkey = s.s_nationkey
    AND r.r_name = 'ASIA'
    AND o.o_orderdate >= date '1994-01-01'
    AND o.o_orderdate < date '1995-01-01'
    AND n.n_name = 'CHINA';
