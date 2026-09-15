-- universe
-- key: o_custkey
WITH verdict_params AS (
    SELECT 1.000::double precision AS verdict_sample_rate,
           100::integer AS verdict_partitions,
           100000::integer AS verdict_hash_modulus,
           100000::integer AS verdict_block_count
),
sampled_orders AS MATERIALIZED (
    SELECT vp.verdict_sample_rate AS verdict_vprob,
           mod(cast(floor((('x' || lpad(substr(md5(cast((o_custkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer), vp.verdict_partitions)::integer AS verdict_vpart,
           o_custkey
    FROM orders_60, verdict_params AS vp
    WHERE o_comment NOT LIKE '%special%requests%'
      AND cast(floor((('x' || lpad(substr(md5(cast((o_custkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) as integer)
),
sampled_customer AS MATERIALIZED (
    SELECT c_custkey
    FROM customer_60, verdict_params AS vp
    WHERE cast(floor((('x' || lpad(substr(md5(cast((c_custkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) as integer)
),
verdict_sample AS (
    SELECT o.verdict_vprob,
           o.verdict_vpart,
           (1.0)::double precision AS m_0
    FROM sampled_customer AS c,
         sampled_orders AS o
    WHERE c.c_custkey = o.o_custkey
),
verdict_part AS (
    SELECT
        ((sum(m_0 / NULLIF(verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS matching_order_count,
        verdict_vpart,
        count(*) AS __vpsize,
        avg(0.0) AS verdict_vprob
    FROM verdict_sample
    GROUP BY verdict_vpart
),
verdict_rollup AS (
    SELECT
        sum(matching_order_count * __vpsize) / NULLIF(sum(__vpsize), 0) AS matching_order_count__est,
        (((stddev(matching_order_count) * sqrt(avg(__vpsize))) / sqrt(sum(__vpsize))) * 1.96) AS matching_order_count__est_err
    FROM verdict_part
)
SELECT
    round(matching_order_count__est) AS matching_order_count,
    matching_order_count__est_err AS matching_order_count_err,
    matching_order_count__est_err / NULLIF(abs(matching_order_count__est), 0.0) AS matching_order_count_rel_ci
FROM verdict_rollup;
