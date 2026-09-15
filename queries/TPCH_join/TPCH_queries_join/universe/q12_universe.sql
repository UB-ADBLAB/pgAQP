-- universe
-- key: l_orderkey
WITH verdict_params AS (
    SELECT 0.0007::double precision AS verdict_sample_rate,
           100::integer AS verdict_partitions,
           100000::integer AS verdict_hash_modulus,
           100000::integer AS verdict_block_count
),
sampled_lineitem AS MATERIALIZED (
    SELECT vp.verdict_sample_rate AS verdict_vprob,
           mod(cast(floor((('x' || lpad(substr(md5(cast((l_orderkey)::text AS varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus AS double precision) * vp.verdict_block_count) AS integer), vp.verdict_partitions)::integer AS verdict_vpart,
           l_orderkey
    FROM lineitem_60, verdict_params AS vp
    WHERE l_shipmode = 'MAIL'
      AND l_commitdate < l_receiptdate
      AND l_shipdate < l_commitdate
      AND l_receiptdate >= date '1995-01-01'
      AND l_receiptdate < date '1996-01-01'
      AND cast(floor((('x' || lpad(substr(md5(cast((l_orderkey)::text AS varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus AS double precision) * vp.verdict_block_count) AS integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) AS integer)
),
sampled_orders AS MATERIALIZED (
    SELECT o_orderkey,
           o_orderpriority
    FROM orders_60, verdict_params AS vp
    WHERE cast(floor((('x' || lpad(substr(md5(cast((o_orderkey)::text AS varchar)), 1, 8), 16, '0'))::bit(64)::bigint % vp.verdict_hash_modulus) / cast(vp.verdict_hash_modulus AS double precision) * vp.verdict_block_count) AS integer) < cast(ceil(vp.verdict_sample_rate * vp.verdict_block_count) AS integer)
),
verdict_sample AS (
    SELECT l.verdict_vprob,
           l.verdict_vpart,
           (CASE WHEN o.o_orderpriority < '3' THEN 1.0 ELSE 0.0 END)::double precision AS m_0,
           (CASE WHEN o.o_orderpriority >= '3' THEN 1.0 ELSE 0.0 END)::double precision AS m_1
    FROM sampled_orders AS o,
         sampled_lineitem AS l
    WHERE o.o_orderkey = l.l_orderkey
),
verdict_part AS (
    SELECT
        ((sum(m_0 / NULLIF(verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS high_line_count,
        ((sum(m_1 / NULLIF(verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS low_line_count,
        verdict_vpart,
        count(*) AS __vpsize,
        avg(0.0) AS verdict_vprob
    FROM verdict_sample
    GROUP BY verdict_vpart
),
verdict_rollup AS (
    SELECT
        sum(high_line_count * __vpsize) / NULLIF(sum(__vpsize), 0) AS high_line_count__est,
        (((stddev(high_line_count) * sqrt(avg(__vpsize))) / sqrt(sum(__vpsize))) * 1.96) AS high_line_count__est_err,
        sum(low_line_count * __vpsize) / NULLIF(sum(__vpsize), 0) AS low_line_count__est,
        (((stddev(low_line_count) * sqrt(avg(__vpsize))) / sqrt(sum(__vpsize))) * 1.96) AS low_line_count__est_err
    FROM verdict_part
)
SELECT
    round(high_line_count__est) AS high_line_count,
    high_line_count__est_err AS high_line_count_err,
    high_line_count__est_err / NULLIF(abs(high_line_count__est), 0.0) AS high_line_count_rel_ci,
    round(low_line_count__est) AS low_line_count,
    low_line_count__est_err AS low_line_count_err,
    low_line_count__est_err / NULLIF(abs(low_line_count__est), 0.0) AS low_line_count_rel_ci
FROM verdict_rollup;
