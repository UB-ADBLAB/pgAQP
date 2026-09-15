set pswr_chosen_plan_index to 0;
-- use index over l_shipdate

SELECT
    approx_sum(l.l_extendedprice * (1 - l.l_discount)) AS revenue,
    approx_sum_half_ci(l.l_extendedprice * (1 - l.l_discount), 0.95) AS revenue_ci
FROM
    lineitem_60 l TABLESAMPLE pswr(30000000, 100000, 0.05, 0.95)
    JOIN supplier_60 s TABLESAMPLE swr(1)
        ON s.s_suppkey = l.l_suppkey
    JOIN orders_60 o TABLESAMPLE swr(1)
        ON o.o_orderkey = l.l_orderkey
    JOIN customer_60 c TABLESAMPLE swr(1)
        ON c.c_custkey = o.o_custkey
    JOIN nation_60 n1 TABLESAMPLE swr(1)
        ON s.s_nationkey = n1.n_nationkey
    JOIN nation_60 n2 TABLESAMPLE swr(1)
        ON c.c_nationkey = n2.n_nationkey
WHERE
    n1.n_name = 'UNITED STATES'
    AND n2.n_name = 'CHINA'
    AND l.l_shipdate >= date '1995-01-01'
    AND l.l_shipdate < date '1997-01-01';

SELECT
    approx_sum(l.l_extendedprice * (1 - l.l_discount)) AS revenue,
    approx_sum_half_ci(l.l_extendedprice * (1 - l.l_discount), 0.95) AS revenue_ci
FROM
    lineitem_60 l TABLESAMPLE pswr(30000000, 100000, 0.05, 0.95)
    JOIN supplier_60 s TABLESAMPLE swr(1)
        ON s.s_suppkey = l.l_suppkey
    JOIN orders_60 o TABLESAMPLE swr(1)
        ON o.o_orderkey = l.l_orderkey
    JOIN customer_60 c TABLESAMPLE swr(1)
        ON c.c_custkey = o.o_custkey
    JOIN nation_60 n1 TABLESAMPLE swr(1)
        ON s.s_nationkey = n1.n_nationkey
    JOIN nation_60 n2 TABLESAMPLE swr(1)
        ON c.c_nationkey = n2.n_nationkey
WHERE
    n1.n_name = 'CHINA'
    AND n2.n_name = 'UNITED STATES'
    AND l.l_shipdate >= date '1995-01-01'
    AND l.l_shipdate < date '1997-01-01';