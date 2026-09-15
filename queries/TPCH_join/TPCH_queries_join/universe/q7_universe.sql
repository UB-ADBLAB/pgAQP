-- universe
-- key: l_orderkey
WITH verdict_params AS (
    SELECT 0.00015::double precision AS verdict_sample_rate,
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
    WHERE l_shipdate >= date '1995-01-01'
      AND l_shipdate < date '1997-01-01'
      AND cast(floor((('x' || lpad(substr(md5(cast((l_orderkey)::text AS varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus AS double precision) * vp.verdict_block_count) AS integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) AS integer)
),
sampled_orders AS MATERIALIZED (
    SELECT o_orderkey,
           o_custkey
    FROM orders_60, verdict_params AS vp
    WHERE cast(floor((('x' || lpad(substr(md5(cast((o_orderkey)::text AS varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus AS double precision) * vp.verdict_block_count) AS integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) AS integer)
),
verdict_sample AS (
    SELECT l.verdict_vprob,
           l.verdict_vpart,
           (l.l_extendedprice * (1 - l.l_discount))::double precision AS m_0
    FROM supplier_60,
         sampled_lineitem AS l,
         sampled_orders AS o,
         customer_60,
         nation_60 n1,
         nation_60 n2
    WHERE s_suppkey = l.l_suppkey
      AND o.o_orderkey = l.l_orderkey
      AND c_custkey = o.o_custkey
      AND s_nationkey = n1.n_nationkey
      AND c_nationkey = n2.n_nationkey
      AND ((n1.n_name = 'UNITED STATES' AND n2.n_name = 'CHINA')
        OR (n1.n_name = 'CHINA' AND n2.n_name = 'UNITED STATES'))
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
