set pswr_chosen_plan_index to 0;
-- use index over l_shipdate

SELECT
    approx_count(*) AS cnt,
    approx_count_star_half_ci(0.95) AS count_ci
FROM
    lineitem_60 l TABLESAMPLE pswr(30000000, 100000, 0.05, 0.95)
    JOIN partsupp_60 ps TABLESAMPLE swr(1)
        ON l.l_partkey = ps.ps_partkey AND l.l_suppkey = ps.ps_suppkey
    JOIN supplier_60 s TABLESAMPLE swr(1)
        ON s.s_suppkey = ps.ps_suppkey
    JOIN nation_60 n TABLESAMPLE swr(1)
        ON s.s_nationkey = n.n_nationkey
    JOIN part_60 p TABLESAMPLE swr(1)
        ON ps.ps_partkey = p.p_partkey
WHERE
    p.p_name LIKE 'green%'
    AND l.l_shipdate >= date '1995-01-01'
    AND l.l_shipdate < date '1997-01-01'
    AND n.n_name = 'CHINA'
    AND ps.ps_availqty > 6000;
