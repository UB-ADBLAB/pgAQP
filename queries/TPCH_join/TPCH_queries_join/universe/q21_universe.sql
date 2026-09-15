-- universe
-- key: l1.l_orderkey
WITH verdict_params AS (
    SELECT 0.0025::double precision AS verdict_sample_rate,
           100::integer AS verdict_partitions,
           100000::integer AS verdict_hash_modulus,
           100000::integer AS verdict_block_count
),
sampled_lineitem AS MATERIALIZED (
    SELECT vp.verdict_sample_rate AS verdict_vprob,
           mod(cast(floor((('x' || lpad(substr(md5(cast((l1.l_orderkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer), vp.verdict_partitions)::integer AS verdict_vpart,
           l1.l_orderkey,
           l1.l_suppkey
    FROM lineitem_60 l1, verdict_params AS vp
    WHERE l1.l_receiptdate > l1.l_commitdate
      AND cast(floor((('x' || lpad(substr(md5(cast((l1.l_orderkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) as integer)
),
sampled_orders AS MATERIALIZED (
    SELECT o_orderkey
    FROM orders_60, verdict_params AS vp
    WHERE o_orderstatus = 'F'
      AND cast(floor((('x' || lpad(substr(md5(cast((o_orderkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) as integer)
),
verdict_sample AS (
    SELECT l1.verdict_vprob,
           l1.verdict_vpart,
           (1.0)::double precision AS m_0
    FROM supplier_60,
         sampled_lineitem AS l1,
         sampled_orders AS o,
         nation_60
    WHERE s_suppkey = l1.l_suppkey
      AND o.o_orderkey = l1.l_orderkey
      AND s_nationkey = n_nationkey
      AND n_name = 'GERMANY'
),
verdict_part AS (
    SELECT
        ((sum(m_0 / NULLIF(verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS numwait,
        verdict_vpart,
        count(*) AS __vpsize,
        avg(0.0) AS verdict_vprob
    FROM verdict_sample
    GROUP BY verdict_vpart
),
verdict_rollup AS (
    SELECT
        sum(numwait * __vpsize) / NULLIF(sum(__vpsize), 0) AS numwait__est,
        (((stddev(numwait) * sqrt(avg(__vpsize))) / sqrt(sum(__vpsize))) * 1.96) AS numwait__est_err
    FROM verdict_part
)
SELECT
    round(numwait__est) AS numwait,
    numwait__est_err AS numwait_err,
    numwait__est_err / NULLIF(abs(numwait__est), 0.0) AS numwait_rel_ci
FROM verdict_rollup;
