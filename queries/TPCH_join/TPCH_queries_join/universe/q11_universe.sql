-- universe
-- key: ps_suppkey
WITH verdict_params AS (
    SELECT 0.035::double precision AS verdict_sample_rate,
           100::integer AS verdict_partitions,
           100000::integer AS verdict_hash_modulus,
           100000::integer AS verdict_block_count
),
sampled_partsupp AS MATERIALIZED (
    SELECT vp.verdict_sample_rate AS verdict_vprob,
           mod(cast(floor((('x' || lpad(substr(md5(cast((ps_suppkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer), vp.verdict_partitions)::integer AS verdict_vpart,
           ps_suppkey,
           ps_supplycost,
           ps_availqty
    FROM partsupp_60, verdict_params AS vp
    WHERE cast(floor((('x' || lpad(substr(md5(cast((ps_suppkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) as integer)
),
sampled_supplier AS MATERIALIZED (
    SELECT s_suppkey,
           s_nationkey
    FROM supplier_60, verdict_params AS vp
    WHERE cast(floor((('x' || lpad(substr(md5(cast((s_suppkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) as integer)
),
verdict_sample AS (
    SELECT ps.verdict_vprob,
           ps.verdict_vpart,
           (ps.ps_supplycost * ps.ps_availqty)::double precision AS m_0
    FROM sampled_partsupp AS ps,
         sampled_supplier AS s,
         nation_60
    WHERE ps.ps_suppkey = s.s_suppkey
      AND s.s_nationkey = n_nationkey
      AND n_name = 'GERMANY'
),
verdict_part AS (
    SELECT
        ((sum(m_0 / NULLIF(verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS value,
        verdict_vpart,
        count(*) AS __vpsize,
        avg(0.0) AS verdict_vprob
    FROM verdict_sample
    GROUP BY verdict_vpart
),
verdict_rollup AS (
    SELECT
        sum(value * __vpsize) / NULLIF(sum(__vpsize), 0) AS value__est,
        (((stddev(value) * sqrt(avg(__vpsize))) / sqrt(sum(__vpsize))) * 1.96) AS value__est_err
    FROM verdict_part
)
SELECT
    value__est AS value,
    value__est_err AS value_err,
    value__est_err / NULLIF(abs(value__est), 0.0) AS value_rel_ci
FROM verdict_rollup;
