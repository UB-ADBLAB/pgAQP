-- universe
-- key: l_orderkey
WITH verdict_params AS (
    SELECT 0.0048::double precision AS verdict_sample_rate,
           100::integer AS verdict_partitions,
           100000::integer AS verdict_hash_modulus,
           100000::integer AS verdict_block_count
),
sampled_lineitem AS MATERIALIZED (
    SELECT vp.verdict_sample_rate AS verdict_vprob,
           mod(cast(floor((('x' || lpad(substr(md5(cast((l_orderkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer), vp.verdict_partitions)::integer AS verdict_vpart,
           l_orderkey
    FROM lineitem_60, verdict_params AS vp
    WHERE l_commitdate < l_receiptdate
      AND cast(floor((('x' || lpad(substr(md5(cast((l_orderkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) as integer)
),
sampled_orders AS MATERIALIZED (
    SELECT o_orderkey
    FROM orders_60, verdict_params AS vp
    WHERE o_orderdate >= date '1993-07-01'
      AND o_orderdate < date '1993-07-01' + interval '3' month
      AND o_orderpriority = '1-URGENT'
      AND cast(floor((('x' || lpad(substr(md5(cast((o_orderkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) as integer)
),
verdict_sample AS (
    SELECT l.verdict_vprob,
           l.verdict_vpart,
           1.0::double precision AS m_0
    FROM sampled_lineitem AS l,
         sampled_orders AS o
    WHERE o.o_orderkey = l.l_orderkey
),
verdict_part AS (
    SELECT
        ((sum(m_0 / NULLIF(verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS order_count,
        verdict_vpart,
        count(*) AS __vpsize,
        avg(0.0) AS verdict_vprob
    FROM verdict_sample
    GROUP BY verdict_vpart
),
verdict_rollup AS (
    SELECT
        sum(order_count * __vpsize) / NULLIF(sum(__vpsize), 0) AS order_count__est,
        (((stddev(order_count) * sqrt(avg(__vpsize))) / sqrt(sum(__vpsize))) * 1.96) AS order_count__est_err
    FROM verdict_part
)
SELECT
    round(order_count__est) AS order_count,
    order_count__est_err AS order_count_err,
    order_count__est_err / NULLIF(abs(order_count__est), 0.0) AS order_count_rel_ci
FROM verdict_rollup;
