set pswr_chosen_plan_index to 0;
-- use index over p_size

SELECT
    approx_count(ps.ps_suppkey) AS supplier_cnt,
    approx_count_star_half_ci(0.95) AS count_ci
FROM
    part_60 p TABLESAMPLE pswr(30000000, 100000, 0.05, 0.95)
    JOIN partsupp_60 ps TABLESAMPLE swr(1)
      ON p.p_partkey = ps.ps_partkey
    JOIN supplier_60 s TABLESAMPLE swr(1)
      ON ps.ps_suppkey = s.s_suppkey
WHERE
    p.p_brand <> 'Brand#12'
    AND p.p_type NOT LIKE 'SMALL%'
    AND p.p_brand = 'Brand#23'
    AND p.p_type = 'ECONOMY ANODIZED STEEL'
    AND p.p_size >= 10
    AND p.p_size < 20
    AND s.s_comment NOT LIKE '%Customer%Complaints%';
