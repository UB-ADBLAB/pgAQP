set pswr_chosen_plan_index to 0;
-- use index over l_shipdate

SELECT
    approx_count(*) AS numwait,
    approx_count_star_half_ci(0.95) AS count_ci
FROM
    lineitem_60 l TABLESAMPLE pswr(30000000, 100000, 0.05, 0.95)
    JOIN supplier_60 s TABLESAMPLE swr(1)
        ON s.s_suppkey = l.l_suppkey
    JOIN orders_60 o TABLESAMPLE swr(1)
        ON o.o_orderkey = l.l_orderkey
    JOIN nation_60 n TABLESAMPLE swr(1)
        ON s.s_nationkey = n.n_nationkey
WHERE
    o.o_orderstatus = 'F'
    AND l.l_receiptdate > l.l_commitdate
    AND n.n_name = 'GERMANY';
