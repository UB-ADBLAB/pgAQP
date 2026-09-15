-- universe
-- key: l_orderkey
WITH verdict_params AS (
    SELECT 0.020::double precision AS verdict_sample_rate,
           100::integer AS verdict_partitions,
           100000::integer AS verdict_hash_modulus,
           100000::integer AS verdict_block_count
),
sampled_lineitem AS MATERIALIZED (
    SELECT vp.verdict_sample_rate AS verdict_vprob,
           mod(cast(floor((('x' || lpad(substr(md5(cast((l_orderkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer), vp.verdict_partitions)::integer AS verdict_vpart,
           l_orderkey,
           l_partkey,
           l_suppkey,
           l_extendedprice,
           l_discount
    FROM lineitem_60, verdict_params AS vp
    WHERE cast(floor((('x' || lpad(substr(md5(cast((l_orderkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) as integer)
),
sampled_orders AS MATERIALIZED (
    SELECT o_orderkey,
           o_custkey
    FROM orders_60, verdict_params AS vp
    WHERE o_orderdate >= date '1995-01-01'
      AND o_orderdate < date '1998-01-01'
      AND cast(floor((('x' || lpad(substr(md5(cast((o_orderkey)::text as varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus as double precision) * vp.verdict_block_count) as integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) as integer)
),
verdict_sample AS (
    SELECT l.verdict_vprob,
           l.verdict_vpart,
           (CASE WHEN n2.n_name = 'GERMANY' THEN l.l_extendedprice * (1 - l.l_discount) ELSE 0.0 END)::double precision AS m_0,
           (l.l_extendedprice * (1 - l.l_discount))::double precision AS m_1
    FROM part_60,
         supplier_60,
         sampled_lineitem AS l,
         sampled_orders AS o,
         customer_60,
         nation_60 n1,
         nation_60 n2,
         region_60
    WHERE p_partkey = l.l_partkey
      AND s_suppkey = l.l_suppkey
      AND l.l_orderkey = o.o_orderkey
      AND o.o_custkey = c_custkey
      AND c_nationkey = n1.n_nationkey
      AND n1.n_regionkey = r_regionkey
      AND r_name = 'EUROPE'
      AND s_nationkey = n2.n_nationkey
      AND p_type = 'ECONOMY ANODIZED STEEL'
),
verdict_part AS (
    SELECT
        ((sum(m_0 / NULLIF(verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS nation_revenue,
        ((sum(m_1 / NULLIF(verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS total_revenue,
        verdict_vpart,
        count(*) AS __vpsize,
        avg(0.0) AS verdict_vprob
    FROM verdict_sample
    GROUP BY verdict_vpart
),
verdict_rollup AS (
    SELECT
        sum(nation_revenue * __vpsize) / NULLIF(sum(__vpsize), 0) AS nation_revenue__est,
        sum(total_revenue * __vpsize) / NULLIF(sum(__vpsize), 0) AS total_revenue__est,
        (((stddev(((1.0) * nation_revenue / NULLIF(total_revenue, 0.0))) * sqrt(avg(__vpsize))) / sqrt(sum(__vpsize))) * 1.96) AS mkt_share_err
    FROM verdict_part
)
SELECT
    (1.0) * nation_revenue__est / NULLIF(total_revenue__est, 0.0) AS mkt_share,
    mkt_share_err,
    mkt_share_err / NULLIF(abs((1.0) * nation_revenue__est / NULLIF(total_revenue__est, 0.0)), 0.0) AS mkt_share_rel_ci
FROM verdict_rollup;
