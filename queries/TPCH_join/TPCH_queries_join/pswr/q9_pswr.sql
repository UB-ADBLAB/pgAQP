set pswr_chosen_plan_index to -1;
-- auto

SELECT
    approx_sum(l.l_extendedprice * (1 - l.l_discount) - ps.ps_supplycost * l.l_quantity) AS sum_profit,
    approx_sum_half_ci(l.l_extendedprice * (1 - l.l_discount) - ps.ps_supplycost * l.l_quantity, 0.95) AS sum_profit_ci
FROM
    lineitem_60 l TABLESAMPLE pswr(30000000, 100000, 0.05, 0.95)
    JOIN orders_60 o TABLESAMPLE swr(1)
        ON o.o_orderkey = l.l_orderkey
    JOIN supplier_60 s TABLESAMPLE swr(1)
        ON s.s_suppkey = l.l_suppkey
    JOIN partsupp_60 ps TABLESAMPLE swr(1)
        ON ps.ps_suppkey = l.l_suppkey AND ps.ps_partkey = l.l_partkey
    JOIN part_60 p TABLESAMPLE swr(1)
        ON p.p_partkey = l.l_partkey
    JOIN nation_60 n TABLESAMPLE swr(1)
        ON s.s_nationkey = n.n_nationkey
WHERE
    p.p_name LIKE '%green%'
    AND n.n_name = 'CHINA'
    AND o.o_orderdate >= date '1995-01-01'
    AND o.o_orderdate <= date '1995-12-31';
