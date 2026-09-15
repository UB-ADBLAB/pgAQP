-- universe
-- key: l_orderkey
WITH verdict_params AS (
    SELECT 0.00045::double precision AS verdict_sample_rate,
           100::integer AS verdict_partitions,
           100000::integer AS verdict_hash_modulus,
           100000::integer AS verdict_block_count
),
sampled_lineitem AS MATERIALIZED (
    SELECT vp.verdict_sample_rate AS verdict_vprob,
           mod(cast(floor((('x' || lpad(substr(md5(cast((l_orderkey)::text AS varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus AS double precision) * vp.verdict_block_count) AS integer), vp.verdict_partitions)::integer AS verdict_vpart,
           l_orderkey,
           l_suppkey,
           l_extendedprice,
           l_discount
    FROM lineitem_60, verdict_params AS vp
    WHERE cast(floor((('x' || lpad(substr(md5(cast((l_orderkey)::text AS varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus AS double precision) * vp.verdict_block_count) AS integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) AS integer)
),
sampled_orders AS MATERIALIZED (
    SELECT o_orderkey,
           o_custkey
    FROM orders_60, verdict_params AS vp
    WHERE o_orderdate >= date '1994-01-01'
      AND o_orderdate < date '1995-01-01'
      AND cast(floor((('x' || lpad(substr(md5(cast((o_orderkey)::text AS varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus AS double precision) * vp.verdict_block_count) AS integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) AS integer)
),
verdict_sample AS (
    SELECT l.verdict_vprob,
           l.verdict_vpart,
           (l.l_extendedprice * (1 - l.l_discount))::double precision AS m_0
    FROM sampled_lineitem AS l,
         sampled_orders AS o,
         customer_60,
         supplier_60,
         nation_60,
         region_60
    WHERE c_custkey = o.o_custkey
      AND l.l_orderkey = o.o_orderkey
      AND l.l_suppkey = s_suppkey
      AND c_nationkey = s_nationkey
      AND s_nationkey = n_nationkey
      AND n_regionkey = r_regionkey
      AND r_name = 'ASIA'
      AND n_name = 'CHINA'
),
verdict_part AS (
    SELECT
        ((sum(m_0 / NULLIF(verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS revenue,
        verdict_vpart,
        count(*) AS __vpsize,
        avg(0.0) AS verdict_vprob
    FROM verdict_sample
    GROUP BY verdict_vpart
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
