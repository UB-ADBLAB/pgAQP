-- universe
-- key: ps_suppkey
WITH verdict_params AS (
    SELECT 0.012::double precision AS verdict_sample_rate,
           100::integer AS verdict_partitions,
           100000::integer AS verdict_hash_modulus,
           100000::integer AS verdict_block_count
),
sampled_partsupp AS MATERIALIZED (
    SELECT vp.verdict_sample_rate AS verdict_vprob,
           mod(cast(floor((('x' || lpad(substr(md5(cast((ps_suppkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer), vp.verdict_partitions)::integer AS verdict_vpart,
           ps_partkey,
           ps_suppkey
    FROM partsupp_60, verdict_params AS vp
    WHERE cast(floor((('x' || lpad(substr(md5(cast((ps_suppkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) as integer)
),
sampled_supplier AS MATERIALIZED (
    SELECT s_suppkey
    FROM supplier_60, verdict_params AS vp
    WHERE s_comment NOT LIKE '%Customer%Complaints%'
      AND cast(floor((('x' || lpad(substr(md5(cast((s_suppkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) as integer)
),
verdict_sample AS (
    SELECT ps.verdict_vprob,
           ps.verdict_vpart,
           1.0::double precision AS m_0
    FROM sampled_partsupp AS ps,
         part_60,
         sampled_supplier AS s
    WHERE p_partkey = ps.ps_partkey
      AND ps.ps_suppkey = s.s_suppkey
      AND p_brand <> 'Brand#12'
      AND p_type NOT LIKE 'SMALL%'
      AND p_brand = 'Brand#23'
      AND p_type = 'ECONOMY ANODIZED STEEL'
      AND p_size >= 10
      AND p_size < 20
),
verdict_part AS (
    SELECT
        ((sum(m_0 / NULLIF(verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS supplier_cnt,
        verdict_vpart,
        count(*) AS __vpsize,
        avg(0.0) AS verdict_vprob
    FROM verdict_sample
    GROUP BY verdict_vpart
),
verdict_rollup AS (
    SELECT
        sum(supplier_cnt * __vpsize) / NULLIF(sum(__vpsize), 0) AS supplier_cnt__est,
        (((stddev(supplier_cnt) * sqrt(avg(__vpsize))) / sqrt(sum(__vpsize))) * 1.96) AS supplier_cnt__est_err
    FROM verdict_part
)
SELECT
    round(supplier_cnt__est) AS supplier_cnt,
    supplier_cnt__est_err AS supplier_cnt_err,
    supplier_cnt__est_err / NULLIF(abs(supplier_cnt__est), 0.0) AS supplier_cnt_rel_ci
FROM verdict_rollup;
