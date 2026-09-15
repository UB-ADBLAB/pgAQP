-- stratified
WITH verdict_params AS (
    SELECT 0.0022::double precision AS verdict_sample_rate,
           100::integer AS verdict_partitions
),
verdict_group AS (
    SELECT substring(c_phone from 1 for 2) AS verdict_group_key,
           count(*) AS verdict_group_size
    FROM customer_60
    GROUP BY substring(c_phone from 1 for 2)
),
verdict_seed AS (
    SELECT s.*,
           g.verdict_group_size
    FROM (
        SELECT *,
               random() AS verdict_rand
        FROM customer_60
    ) AS s
    JOIN verdict_group AS g
      ON (substring(s.c_phone from 1 for 2)) IS NOT DISTINCT FROM g.verdict_group_key
    CROSS JOIN verdict_params AS vp
    WHERE s.verdict_rand < (
        (((SELECT sum(verdict_group_size) FROM verdict_group) * vp.verdict_sample_rate)
          / (SELECT count(*) FROM verdict_group))
        / g.verdict_group_size
    )
),
customer_st AS (
    SELECT s.*,
           (t.verdict_group_size_in_sample::double precision
            / s.verdict_group_size::double precision) AS verdict_vprob,
           mod(cast(floor(random() * vp.verdict_partitions) AS integer),
               vp.verdict_partitions) AS verdict_vpart
    FROM verdict_seed AS s
    JOIN (
        SELECT substring(c_phone from 1 for 2) AS verdict_group_key,
               count(*) AS verdict_group_size_in_sample
        FROM verdict_seed
        GROUP BY substring(c_phone from 1 for 2)
    ) AS t
      ON (substring(c_phone from 1 for 2)) IS NOT DISTINCT FROM t.verdict_group_key
    CROSS JOIN verdict_params AS vp
),
verdict_part AS (
    SELECT customer.verdict_vpart AS verdict_vpart,
           count(*) AS __vpsize,
        ((sum((1.0) / NULLIF(customer.verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS numcust,
        ((sum((c_acctbal) / NULLIF(customer.verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS totacctbal
    FROM customer_st customer
    WHERE substring(c_phone from 1 for 2) = '13'
      AND c_acctbal > 0
    GROUP BY customer.verdict_vpart
),
verdict_rollup AS (
    SELECT
        sum(numcust * __vpsize) / NULLIF(sum(__vpsize), 0) AS numcust__est,
        (((stddev(numcust) * sqrt(avg(__vpsize))) / sqrt(sum(__vpsize))) * 1.96) AS numcust__est_err,
        sum(totacctbal * __vpsize) / NULLIF(sum(__vpsize), 0) AS totacctbal__est,
        (((stddev(totacctbal) * sqrt(avg(__vpsize))) / sqrt(sum(__vpsize))) * 1.96) AS totacctbal__est_err
    FROM verdict_part
)
SELECT
    round(numcust__est) AS numcust,
    numcust__est_err AS numcust_err,
    numcust__est_err / NULLIF(abs(numcust__est), 0.0) AS numcust_rel_ci,
    totacctbal__est AS totacctbal,
    totacctbal__est_err AS totacctbal_err,
    totacctbal__est_err / NULLIF(abs(totacctbal__est), 0.0) AS totacctbal_rel_ci
FROM verdict_rollup;
