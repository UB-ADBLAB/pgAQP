-- stratified
WITH verdict_params AS (
    SELECT 0.00006::double precision AS verdict_sample_rate,
           100::integer AS verdict_partitions
),
verdict_group AS (
    SELECT l_discount,
           count(*) AS verdict_group_size
    FROM lineitem_60
    GROUP BY l_discount
),
verdict_seed AS (
    SELECT s.*,
           g.verdict_group_size
    FROM (
        SELECT *,
               random() AS verdict_rand
        FROM lineitem_60
    ) AS s
    JOIN verdict_group AS g
      ON s.l_discount IS NOT DISTINCT FROM g.l_discount
    CROSS JOIN verdict_params AS vp
    WHERE s.verdict_rand < (
        (((SELECT sum(verdict_group_size) FROM verdict_group) * vp.verdict_sample_rate)
          / (SELECT count(*) FROM verdict_group))
        / g.verdict_group_size
    )
),
lineitem_st AS (
    SELECT s.*,
           (t.verdict_group_size_in_sample::double precision
            / s.verdict_group_size::double precision) AS verdict_vprob,
           mod(cast(floor(random() * vp.verdict_partitions) AS integer),
               vp.verdict_partitions) AS verdict_vpart
    FROM verdict_seed AS s
    JOIN (
        SELECT l_discount,
               count(*) AS verdict_group_size_in_sample
        FROM verdict_seed
        GROUP BY l_discount
    ) AS t
      ON s.l_discount IS NOT DISTINCT FROM t.l_discount
    CROSS JOIN verdict_params AS vp
),
verdict_part AS (
    SELECT
        ((sum((l_extendedprice * l_discount) / NULLIF(verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS revenue,
        verdict_vpart,
        count(*) AS __vpsize,
        avg(1.0) AS verdict_vprob
    FROM lineitem_st
    WHERE l_shipdate >= date '1994-01-01'
      AND l_shipdate < date '1997-01-01'
      AND l_discount >= 0.07
      AND l_discount <= 0.09
      AND l_quantity < 24
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
