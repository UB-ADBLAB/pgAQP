set pswr_chosen_plan_index to -1;
-- auto

SELECT
    approx_count(*) AS matching_order_count,
    approx_count_star_half_ci(0.95) AS count_ci
FROM
    orders_60 o TABLESAMPLE pswr(30000000, 100000, 0.05, 0.95)
    JOIN customer_60 c TABLESAMPLE swr(1)
      ON c.c_custkey = o.o_custkey
WHERE
    o.o_comment NOT LIKE '%special%requests%';
