-- universe
-- key: l_partkey
WITH verdict_params AS (
    SELECT 0.0072::double precision AS verdict_sample_rate,
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
    WHERE l_quantity >= 10
      AND l_quantity <= 20
      AND l_shipmode >= 'AIR'
      AND l_shipmode < 'AIS'
      AND l_shipinstruct = 'DELIVER IN PERSON'
      AND cast(floor((('x' || lpad(substr(md5(cast((l_partkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) as integer)
),
sampled_part AS MATERIALIZED (
    SELECT p_partkey
    FROM part_60, verdict_params AS vp
    WHERE p_brand = 'Brand#23'
      AND ((p_container >= 'LG' AND p_container < 'LH')
        OR (p_container >= 'MED' AND p_container < 'MEE')
        OR (p_container >= 'SM' AND p_container < 'SN'))
      AND p_size >= 1
      AND p_size <= 15
      AND cast(floor((('x' || lpad(substr(md5(cast((p_partkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) as integer)
),
verdict_part AS (
    SELECT
        ((sum((l.l_extendedprice * (1 - l.l_discount)) / NULLIF(l.verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS revenue,
        l.verdict_vpart AS verdict_vpart,
        count(*) AS __vpsize,
        avg(1.0) AS verdict_vprob
    FROM sampled_lineitem AS l,
         sampled_part AS p
    WHERE p.p_partkey = l.l_partkey
    GROUP BY l.verdict_vpart
),
verdict_rollup AS (
    SELECT
        sum(revenue * __vpsize) / NULLIF(sum(__vpsize), 0) AS revenue__est,
        (((stddev(revenue) * sqrt(avg(__vpsize))) / sqrt(sum(__vpsize))) * 1.96) AS revenue__est_err
    FROM verdict_part
)
SELECT
    revenue__est AS revenue,
    revenue__est_err AS revenue_err,
    revenue__est_err / NULLIF(abs(revenue__est), 0.0) AS revenue_rel_ci
FROM verdict_rollup;
