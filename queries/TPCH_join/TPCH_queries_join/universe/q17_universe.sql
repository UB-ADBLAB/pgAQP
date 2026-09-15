-- universe
-- key: l_partkey
WITH verdict_params AS (
    SELECT 0.070::double precision AS verdict_sample_rate,
           100::integer AS verdict_partitions,
           100000::integer AS verdict_hash_modulus,
           100000::integer AS verdict_block_count
),
sampled_lineitem AS MATERIALIZED (
    SELECT vp.verdict_sample_rate AS verdict_vprob,
           mod(cast(floor((('x' || lpad(substr(md5(cast((l_partkey)::text AS varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus AS double precision) * vp.verdict_block_count) AS integer), vp.verdict_partitions)::integer AS verdict_vpart,
           l_partkey,
           l_extendedprice
    FROM lineitem_60, verdict_params AS vp
    WHERE l_quantity >= 10
      AND l_quantity < 20
      AND cast(floor((('x' || lpad(substr(md5(cast((l_partkey)::text AS varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus AS double precision) * vp.verdict_block_count) AS integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) AS integer)
),
sampled_part AS MATERIALIZED (
    SELECT p_partkey
    FROM part_60, verdict_params AS vp
    WHERE p_brand = 'Brand#23'
      AND p_container = 'MED BOX'
      AND cast(floor((('x' || lpad(substr(md5(cast((p_partkey)::text AS varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus AS double precision) * vp.verdict_block_count) AS integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) AS integer)
),
verdict_sample AS (
    SELECT l.verdict_vprob,
           l.verdict_vpart,
           (l.l_extendedprice / 7.0)::double precision AS m_0
    FROM sampled_lineitem AS l,
         sampled_part AS p
    WHERE p.p_partkey = l.l_partkey
),
verdict_part AS (
    SELECT
        ((sum(m_0 / NULLIF(verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS avg_yearly,
        verdict_vpart,
        count(*) AS __vpsize,
        avg(0.0) AS verdict_vprob
    FROM verdict_sample
    GROUP BY verdict_vpart
),
verdict_rollup AS (
    SELECT
        sum(avg_yearly * __vpsize) / NULLIF(sum(__vpsize), 0) AS avg_yearly__est,
        (((stddev(avg_yearly) * sqrt(avg(__vpsize))) / sqrt(sum(__vpsize))) * 1.96) AS avg_yearly__est_err
    FROM verdict_part
)
SELECT
    avg_yearly__est AS avg_yearly,
    avg_yearly__est_err AS avg_yearly_err,
    avg_yearly__est_err / NULLIF(abs(avg_yearly__est), 0.0) AS avg_yearly_rel_ci
FROM verdict_rollup;
