-- universe
-- key: l_partkey
WITH verdict_params AS (
    SELECT 0.150::double precision AS verdict_sample_rate,
           100::integer AS verdict_partitions,
           100000::integer AS verdict_hash_modulus,
           100000::integer AS verdict_block_count
),
sampled_lineitem AS MATERIALIZED (
    SELECT vp.verdict_sample_rate AS verdict_vprob,
           mod(cast(floor((('x' || lpad(substr(md5(cast((l_partkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer), vp.verdict_partitions)::integer AS verdict_vpart,
           l_partkey,
           l_extendedprice,
           l_discount
    FROM lineitem_60, verdict_params AS vp
    WHERE l_shipdate >= date '1995-09-01'
      AND l_shipdate < date '1995-10-01'
      AND cast(floor((('x' || lpad(substr(md5(cast((l_partkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) as integer)
),
sampled_part AS MATERIALIZED (
    SELECT p_partkey,
           p_type
    FROM part_60, verdict_params AS vp
    WHERE cast(floor((('x' || lpad(substr(md5(cast((p_partkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) as integer)
),
verdict_sample AS (
    SELECT l.verdict_vprob,
           l.verdict_vpart,
           (CASE WHEN p.p_type LIKE 'PROMO%' THEN l.l_extendedprice * (1 - l.l_discount) ELSE 0.0 END)::double precision AS m_0,
           (l.l_extendedprice * (1 - l.l_discount))::double precision AS m_1
    FROM sampled_lineitem AS l,
         sampled_part AS p
    WHERE l.l_partkey = p.p_partkey
),
verdict_part AS (
    SELECT
        ((sum(m_0 / NULLIF(verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS promo_revenue,
        ((sum(m_1 / NULLIF(verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS total_revenue,
        verdict_vpart,
        count(*) AS __vpsize,
        avg(0.0) AS verdict_vprob
    FROM verdict_sample
    GROUP BY verdict_vpart
),
verdict_rollup AS (
    SELECT
        sum(promo_revenue * __vpsize) / NULLIF(sum(__vpsize), 0) AS promo_revenue__est,
        sum(total_revenue * __vpsize) / NULLIF(sum(__vpsize), 0) AS total_revenue__est,
        (((stddev(100.00 * promo_revenue / NULLIF(total_revenue, 0.0)) * sqrt(avg(__vpsize))) / sqrt(sum(__vpsize))) * 1.96) AS promo_revenue__est_err
    FROM verdict_part
)
SELECT
    100.00 * promo_revenue__est / NULLIF(total_revenue__est, 0.0) AS promo_revenue,
    promo_revenue__est_err AS promo_revenue_err,
    promo_revenue__est_err / NULLIF(abs(100.00 * promo_revenue__est / NULLIF(total_revenue__est, 0.0)), 0.0) AS promo_revenue_rel_ci
FROM verdict_rollup;
