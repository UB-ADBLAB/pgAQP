-- universe
-- key: l_suppkey
WITH verdict_params AS (
    SELECT 0.100::double precision AS verdict_sample_rate,
           100::integer AS verdict_partitions,
           100000::integer AS verdict_hash_modulus,
           100000::integer AS verdict_block_count
),
sampled_lineitem AS MATERIALIZED (
    SELECT vp.verdict_sample_rate AS verdict_vprob,
           mod(cast(floor((('x' || lpad(substr(md5(cast((l_suppkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer), vp.verdict_partitions)::integer AS verdict_vpart,
           l_suppkey,
           l_extendedprice,
           l_discount
    FROM lineitem_60, verdict_params AS vp
    WHERE l_shipdate >= date '1995-01-01'
      AND l_shipdate < date '1997-04-01'
      AND cast(floor((('x' || lpad(substr(md5(cast((l_suppkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) as integer)
),
sampled_supplier AS MATERIALIZED (
    SELECT s_suppkey
    FROM supplier_60, verdict_params AS vp
    WHERE cast(floor((('x' || lpad(substr(md5(cast((s_suppkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) as integer)
),
verdict_sample AS (
    SELECT l.verdict_vprob,
           l.verdict_vpart,
           (l.l_extendedprice * (1 - l.l_discount))::double precision AS m_0
    FROM sampled_supplier AS s,
         sampled_lineitem AS l
    WHERE s.s_suppkey = l.l_suppkey
),
verdict_part AS (
    SELECT
        ((sum(m_0 / NULLIF(verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS total_revenue,
        verdict_vpart,
        count(*) AS __vpsize,
        avg(0.0) AS verdict_vprob
    FROM verdict_sample
    GROUP BY verdict_vpart
),
verdict_rollup AS (
    SELECT
        sum(total_revenue * __vpsize) / NULLIF(sum(__vpsize), 0) AS total_revenue__est,
        (((stddev(total_revenue) * sqrt(avg(__vpsize))) / sqrt(sum(__vpsize))) * 1.96) AS total_revenue__est_err
    FROM verdict_part
)
SELECT
    total_revenue__est AS total_revenue,
    total_revenue__est_err AS total_revenue_err,
    total_revenue__est_err / NULLIF(abs(total_revenue__est), 0.0) AS total_revenue_rel_ci
FROM verdict_rollup;
