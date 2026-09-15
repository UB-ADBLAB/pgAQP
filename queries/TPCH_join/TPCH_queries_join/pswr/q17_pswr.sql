set pswr_chosen_plan_index to -1;
-- auto

SELECT
    approx_sum(l.l_extendedprice) / 7.0 AS avg_yearly,
    approx_sum_half_ci(l.l_extendedprice, 0.95) / 7.0 AS avg_yearly_ci
FROM
    part_60 p TABLESAMPLE pswr(30000000, 100000, 0.05, 0.95)
    JOIN lineitem_60 l TABLESAMPLE swr(1)
      ON p.p_partkey = l.l_partkey
WHERE
    p.p_brand = 'Brand#23'
    AND p.p_container = 'MED BOX'
    AND l.l_quantity >= 10
    AND l.l_quantity < 20;
