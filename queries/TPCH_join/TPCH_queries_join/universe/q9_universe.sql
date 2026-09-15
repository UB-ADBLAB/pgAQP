-- universe
-- key: l_orderkey
WITH verdict_params AS (
    SELECT 0.00065::double precision AS verdict_sample_rate,
           100::integer AS verdict_partitions,
           100000::integer AS verdict_hash_modulus,
           100000::integer AS verdict_block_count
),
sampled_lineitem AS MATERIALIZED (
    SELECT vp.verdict_sample_rate AS verdict_vprob,
           mod(cast(floor((('x' || lpad(substr(md5(cast((l_orderkey)::text AS varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus AS double precision) * vp.verdict_block_count) AS integer), vp.verdict_partitions)::integer AS verdict_vpart,
           l_orderkey,
           l_partkey,
           l_suppkey,
           l_extendedprice,
           l_discount,
           l_quantity
    FROM lineitem_60, verdict_params AS vp
    WHERE cast(floor((('x' || lpad(substr(md5(cast((l_orderkey)::text AS varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus AS double precision) * vp.verdict_block_count) AS integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) AS integer)
),
sampled_orders AS MATERIALIZED (
    SELECT o_orderkey
    FROM orders_60, verdict_params AS vp
    WHERE o_orderdate >= date '1995-01-01'
      AND o_orderdate <= date '1995-12-31'
      AND cast(floor((('x' || lpad(substr(md5(cast((o_orderkey)::text AS varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus AS double precision) * vp.verdict_block_count) AS integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) AS integer)
),
verdict_sample AS (
    SELECT l.verdict_vprob,
           l.verdict_vpart,
           (l.l_extendedprice * (1 - l.l_discount) - ps_supplycost * l.l_quantity)::double precision AS m_0
    FROM part_60,
         supplier_60,
         sampled_lineitem AS l,
         partsupp_60,
         sampled_orders AS o,
         nation_60
    WHERE s_suppkey = l.l_suppkey
      AND ps_suppkey = l.l_suppkey
      AND ps_partkey = l.l_partkey
      AND p_partkey = l.l_partkey
      AND o.o_orderkey = l.l_orderkey
      AND s_nationkey = n_nationkey
      AND p_name LIKE '%green%'
      AND n_name = 'CHINA'
),
verdict_part AS (
    SELECT
        ((sum(m_0 / NULLIF(verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS sum_profit,
        verdict_vpart,
        count(*) AS __vpsize,
        avg(0.0) AS verdict_vprob
    FROM verdict_sample
    GROUP BY verdict_vpart
),
verdict_rollup AS (
    SELECT
        sum(sum_profit * __vpsize) / NULLIF(sum(__vpsize), 0) AS sum_profit__est,
        (((stddev(sum_profit) * sqrt(avg(__vpsize))) / sqrt(sum(__vpsize))) * 1.96) AS sum_profit__est_err
    FROM verdict_part
)
SELECT
    sum_profit__est AS sum_profit,
    sum_profit__est_err AS sum_profit_err,
    sum_profit__est_err / NULLIF(abs(sum_profit__est), 0.0) AS sum_profit_rel_ci
FROM verdict_rollup;
