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
    JOIN nation_60 n TABLESAMPLE swr(1)
        ON c.c_nationkey = n.n_nationkey
WHERE
    o.o_orderdate >= date '1995-01-01'
    AND o.o_orderdate < date '1995-01-01' + interval '3' month
    AND l.l_returnflag = 'R';
