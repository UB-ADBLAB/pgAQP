-- universe
-- key: ps_partkey
WITH verdict_params AS (
    SELECT 0.0021::double precision AS verdict_sample_rate,
           100::integer AS verdict_partitions,
           100000::integer AS verdict_hash_modulus,
           100000::integer AS verdict_block_count
),
sampled_partsupp AS MATERIALIZED (
    SELECT vp.verdict_sample_rate AS verdict_vprob,
           mod(cast(floor((('x' || lpad(substr(md5(cast((ps_partkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer), vp.verdict_partitions)::integer AS verdict_vpart,
           ps_partkey,
           ps_suppkey
    FROM partsupp_60, verdict_params AS vp
    WHERE cast(floor((('x' || lpad(substr(md5(cast((ps_partkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) as integer)
),
sampled_part AS MATERIALIZED (
    SELECT p_partkey
    FROM part_60, verdict_params AS vp
    WHERE p_type LIKE '%BRASS'
      AND cast(floor((('x' || lpad(substr(md5(cast((p_partkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) as integer)
),
verdict_sample AS (
    SELECT ps.verdict_vprob,
           ps.verdict_vpart,
           (1.0)::double precision AS m_0
    FROM sampled_part AS p,
         supplier_60,
         sampled_partsupp AS ps,
         nation_60,
         region_60
    WHERE p.p_partkey = ps.ps_partkey
      AND s_suppkey = ps.ps_suppkey
      AND s_nationkey = n_nationkey
      AND n_regionkey = r_regionkey
      AND r_name = 'EUROPE'
),
verdict_part AS (
    SELECT
        ((sum(m_0 / NULLIF(verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS matching_supplier_part_count,
        verdict_vpart,
        count(*) AS __vpsize,
        avg(0.0) AS verdict_vprob
    FROM verdict_sample
    GROUP BY verdict_vpart
),
verdict_rollup AS (
    SELECT
        sum(matching_supplier_part_count * __vpsize) / NULLIF(sum(__vpsize), 0) AS matching_supplier_part_count__est,
        (((stddev(matching_supplier_part_count) * sqrt(avg(__vpsize))) / sqrt(sum(__vpsize))) * 1.96) AS matching_supplier_part_count__est_err
    FROM verdict_part
)
SELECT
    round(matching_supplier_part_count__est) AS matching_supplier_part_count,
    matching_supplier_part_count__est_err AS matching_supplier_part_count_err,
    matching_supplier_part_count__est_err / NULLIF(abs(matching_supplier_part_count__est), 0.0) AS matching_supplier_part_count_rel_ci
FROM verdict_rollup;
