-- universe
-- key: (l_partkey)::text || ':' || (l_suppkey)::text
WITH verdict_params AS (
    SELECT 0.100::double precision AS verdict_sample_rate,
           100::integer AS verdict_partitions,
           100000::integer AS verdict_hash_modulus,
           100000::integer AS verdict_block_count
),
sampled_lineitem AS MATERIALIZED (
    SELECT vp.verdict_sample_rate AS verdict_vprob,
           mod(cast(floor((('x' || lpad(substr(md5(cast(((l_partkey)::text || ':' || (l_suppkey)::text)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer), vp.verdict_partitions)::integer AS verdict_vpart,
           l_partkey,
           l_suppkey
    FROM lineitem_60, verdict_params AS vp
    WHERE l_shipdate >= date '1995-01-01'
      AND l_shipdate < date '1997-01-01'
      AND cast(floor((('x' || lpad(substr(md5(cast(((l_partkey)::text || ':' || (l_suppkey)::text)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) as integer)
),
sampled_partsupp AS MATERIALIZED (
    SELECT ps_partkey,
           ps_suppkey
    FROM partsupp_60, verdict_params AS vp
    WHERE ps_availqty > 6000
      AND cast(floor((('x' || lpad(substr(md5(cast(((ps_partkey)::text || ':' || (ps_suppkey)::text)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) as integer)
),
verdict_sample AS (
    SELECT l.verdict_vprob,
           l.verdict_vpart,
           (1.0)::double precision AS m_0
    FROM supplier_60,
         nation_60,
         sampled_partsupp AS ps,
         part_60,
         sampled_lineitem AS l
    WHERE s_nationkey = n_nationkey
      AND s_suppkey = ps.ps_suppkey
      AND ps.ps_partkey = p_partkey
      AND l.l_partkey = ps.ps_partkey
      AND l.l_suppkey = ps.ps_suppkey
      AND p_name LIKE 'green%'
      AND n_name = 'CHINA'
),
verdict_part AS (
    SELECT
        ((sum(m_0 / NULLIF(verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS cnt,
        verdict_vpart,
        count(*) AS __vpsize,
        avg(0.0) AS verdict_vprob
    FROM verdict_sample
    GROUP BY verdict_vpart
),
verdict_rollup AS (
    SELECT
        sum(cnt * __vpsize) / NULLIF(sum(__vpsize), 0) AS cnt__est,
        (((stddev(cnt) * sqrt(avg(__vpsize))) / sqrt(sum(__vpsize))) * 1.96) AS cnt__est_err
    FROM verdict_part
)
SELECT
    round(cnt__est) AS cnt,
    cnt__est_err AS cnt_err,
    cnt__est_err / NULLIF(abs(cnt__est), 0.0) AS cnt_rel_ci
FROM verdict_rollup;
