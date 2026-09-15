set pswr_chosen_plan_index to -1;
-- auto

-- Nation revenue
SELECT
    approx_sum(l.l_extendedprice * (1 - l.l_discount)) AS nation_revenue,
    approx_sum_half_ci(l.l_extendedprice * (1 - l.l_discount), 0.95) AS nation_revenue_ci
FROM
    lineitem_60 l TABLESAMPLE pswr(30000000, 100000, 0.05, 0.95)
    JOIN part_60 p TABLESAMPLE swr(1)
        ON p.p_partkey = l.l_partkey
    JOIN supplier_60 s TABLESAMPLE swr(1)
        ON s.s_suppkey = l.l_suppkey
    JOIN orders_60 o TABLESAMPLE swr(1)
        ON o.o_orderkey = l.l_orderkey
    JOIN customer_60 c TABLESAMPLE swr(1)
        ON c.c_custkey = o.o_custkey
    JOIN nation_60 n1 TABLESAMPLE swr(1)
        ON c.c_nationkey = n1.n_nationkey
    JOIN nation_60 n2 TABLESAMPLE swr(1)
        ON s.s_nationkey = n2.n_nationkey
    JOIN region_60 r TABLESAMPLE swr(1)
        ON n1.n_regionkey = r.r_regionkey
WHERE
    r.r_name = 'EUROPE'
    AND n2.n_name = 'GERMANY'
    AND o.o_orderdate >= date '1995-01-01'
    AND o.o_orderdate < date '1998-01-01'
    AND p.p_type = 'ECONOMY ANODIZED STEEL';

-- Total revenue
SELECT
    approx_sum(l.l_extendedprice * (1 - l.l_discount)) AS total_revenue,
    approx_sum_half_ci(l.l_extendedprice * (1 - l.l_discount), 0.95) AS total_revenue_ci
FROM
    lineitem_60 l TABLESAMPLE pswr(30000000, 100000, 0.05, 0.95)
    JOIN part_60 p TABLESAMPLE swr(1)
        ON p.p_partkey = l.l_partkey
    JOIN supplier_60 s TABLESAMPLE swr(1)
        ON s.s_suppkey = l.l_suppkey
    JOIN orders_60 o TABLESAMPLE swr(1)
        ON o.o_orderkey = l.l_orderkey
    JOIN customer_60 c TABLESAMPLE swr(1)
        ON c.c_custkey = o.o_custkey
    JOIN nation_60 n1 TABLESAMPLE swr(1)
        ON c.c_nationkey = n1.n_nationkey
    JOIN nation_60 n2 TABLESAMPLE swr(1)
        ON s.s_nationkey = n2.n_nationkey
    JOIN region_60 r TABLESAMPLE swr(1)
        ON n1.n_regionkey = r.r_regionkey
WHERE
    r.r_name = 'EUROPE'
    AND o.o_orderdate >= date '1995-01-01'
    AND o.o_orderdate < date '1998-01-01'
    AND p.p_type = 'ECONOMY ANODIZED STEEL';