set pswr_chosen_plan_index to 0;
-- use index over p_size

SELECT
    approx_count(*) AS matching_supplier_part_count,
    approx_count_star_half_ci(0.95) AS count_ci
FROM
    part_60 TABLESAMPLE pswr(30000000, 50*200, 0.05, 0.95),
    partsupp_60 TABLESAMPLE swr(1),
    supplier_60 TABLESAMPLE swr(1),
    nation_60 TABLESAMPLE swr(1),
    region_60 TABLESAMPLE swr(1)
WHERE
    part_60.p_partkey = partsupp_60.ps_partkey
    AND supplier_60.s_suppkey = partsupp_60.ps_suppkey
    AND part_60.p_type LIKE '%BRASS'
    AND supplier_60.s_nationkey = nation_60.n_nationkey
    AND nation_60.n_regionkey = region_60.r_regionkey
    AND region_60.r_name = 'EUROPE';
