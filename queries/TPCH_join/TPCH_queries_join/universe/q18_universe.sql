-- universe
-- key: l_orderkey
WITH verdict_params AS (
    SELECT 0.0027::double precision AS verdict_sample_rate,
           100::integer AS verdict_partitions,
           100000::integer AS verdict_hash_modulus,
           100000::integer AS verdict_block_count
),
sampled_lineitem AS MATERIALIZED (
    SELECT vp.verdict_sample_rate AS verdict_vprob,
           mod(cast(floor((('x' || lpad(substr(md5(cast((l_orderkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer), vp.verdict_partitions)::integer AS verdict_vpart,
           l_orderkey,
           l_quantity
    FROM lineitem_60, verdict_params AS vp
    WHERE l_quantity > 10
      AND cast(floor((('x' || lpad(substr(md5(cast((l_orderkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) as integer)
),
sampled_orders AS MATERIALIZED (
    SELECT o_orderkey,
           o_custkey
    FROM orders_60, verdict_params AS vp
    WHERE cast(floor((('x' || lpad(substr(md5(cast((o_orderkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) as integer)
),
verdict_sample AS (
    SELECT l.verdict_vprob,
           l.verdict_vpart,
           (l.l_quantity)::double precision AS m_0
    FROM customer_60,
         sampled_orders AS o,
         sampled_lineitem AS l
    WHERE c_custkey = o.o_custkey
      AND o.o_orderkey = l.l_orderkey
),
verdict_part AS (
    SELECT
        ((sum(m_0 / NULLIF(verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS total_quantity,
        verdict_vpart,
        count(*) AS __vpsize,
        avg(0.0) AS verdict_vprob
    FROM verdict_sample
    GROUP BY verdict_vpart
),
verdict_rollup AS (
    SELECT
        sum(total_quantity * __vpsize) / NULLIF(sum(__vpsize), 0) AS total_quantity__est,
        (((stddev(total_quantity) * sqrt(avg(__vpsize))) / sqrt(sum(__vpsize))) * 1.96) AS total_quantity__est_err
    FROM verdict_part
)
SELECT
    total_quantity__est AS total_quantity,
    total_quantity__est_err AS total_quantity_err,
    total_quantity__est_err / NULLIF(abs(total_quantity__est), 0.0) AS total_quantity_rel_ci
FROM verdict_rollup;
