-- stratified
WITH verdict_params AS (
    SELECT 0.000035::double precision AS verdict_sample_rate,
           100::integer AS verdict_partitions
),
verdict_group AS (
    SELECT l_returnflag,
           l_linestatus,
           count(*) AS verdict_group_size
    FROM lineitem_60
    GROUP BY l_returnflag, l_linestatus
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
      ON s.l_returnflag IS NOT DISTINCT FROM g.l_returnflag
     AND s.l_linestatus IS NOT DISTINCT FROM g.l_linestatus
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
        SELECT l_returnflag,
               l_linestatus,
               count(*) AS verdict_group_size_in_sample
        FROM verdict_seed
        GROUP BY l_returnflag, l_linestatus
    ) AS t
      ON s.l_returnflag IS NOT DISTINCT FROM t.l_returnflag
     AND s.l_linestatus IS NOT DISTINCT FROM t.l_linestatus
    CROSS JOIN verdict_params AS vp
),
verdict_part AS (
    SELECT
        ((sum(l_quantity / NULLIF(verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS sum_qty,
        ((sum(l_extendedprice / NULLIF(verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS sum_base_price,
        ((sum((l_extendedprice * (1 - l_discount)) / NULLIF(verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS sum_disc_price,
        ((sum((l_extendedprice * (1 - l_discount) * (1 + l_tax)) / NULLIF(verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS sum_charge,
        ((sum(1.0 / NULLIF(verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS count_order,
        verdict_vpart,
        count(*) AS __vpsize,
        avg(1.0) AS verdict_vprob
    FROM lineitem_st
    WHERE l_shipdate <= date '1998-12-01' - interval '90' day
      AND l_returnflag = 'A'
      AND l_linestatus = 'F'
    GROUP BY verdict_vpart
),
verdict_rollup AS (
    SELECT
        sum(sum_qty * __vpsize) / NULLIF(sum(__vpsize), 0) AS sum_qty__est,
        (((stddev(sum_qty) * sqrt(avg(__vpsize))) / sqrt(sum(__vpsize))) * 1.96) AS sum_qty__est_err,
        sum(sum_base_price * __vpsize) / NULLIF(sum(__vpsize), 0) AS sum_base_price__est,
        (((stddev(sum_base_price) * sqrt(avg(__vpsize))) / sqrt(sum(__vpsize))) * 1.96) AS sum_base_price__est_err,
        sum(sum_disc_price * __vpsize) / NULLIF(sum(__vpsize), 0) AS sum_disc_price__est,
        (((stddev(sum_disc_price) * sqrt(avg(__vpsize))) / sqrt(sum(__vpsize))) * 1.96) AS sum_disc_price__est_err,
        sum(sum_charge * __vpsize) / NULLIF(sum(__vpsize), 0) AS sum_charge__est,
        (((stddev(sum_charge) * sqrt(avg(__vpsize))) / sqrt(sum(__vpsize))) * 1.96) AS sum_charge__est_err,
        sum(count_order * __vpsize) / NULLIF(sum(__vpsize), 0) AS count_order__est,
        (((stddev(count_order) * sqrt(avg(__vpsize))) / sqrt(sum(__vpsize))) * 1.96) AS count_order__est_err
    FROM verdict_part
)
SELECT
    sum_qty__est AS sum_qty,
    sum_qty__est_err AS sum_qty_err,
    sum_qty__est_err / NULLIF(abs(sum_qty__est), 0.0) AS sum_qty_rel_ci,
    sum_base_price__est AS sum_base_price,
    sum_base_price__est_err AS sum_base_price_err,
    sum_base_price__est_err / NULLIF(abs(sum_base_price__est), 0.0) AS sum_base_price_rel_ci,
    sum_disc_price__est AS sum_disc_price,
    sum_disc_price__est_err AS sum_disc_price_err,
    sum_disc_price__est_err / NULLIF(abs(sum_disc_price__est), 0.0) AS sum_disc_price_rel_ci,
    sum_charge__est AS sum_charge,
    sum_charge__est_err AS sum_charge_err,
    sum_charge__est_err / NULLIF(abs(sum_charge__est), 0.0) AS sum_charge_rel_ci,
    round(count_order__est) AS count_order,
    count_order__est_err AS count_order_err,
    count_order__est_err / NULLIF(abs(count_order__est), 0.0) AS count_order_rel_ci
FROM verdict_rollup;
