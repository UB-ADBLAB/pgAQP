-- uniform
WITH verdict_params AS (
    SELECT 0.0022::double precision AS verdict_sample_rate,
           100::integer AS verdict_partitions
),
customer_uf AS (
    SELECT t.*,
           vp.verdict_sample_rate AS verdict_vprob,
           mod(cast(floor(random() * vp.verdict_partitions) AS integer),
               vp.verdict_partitions) AS verdict_vpart
    FROM customer_60 AS t
    CROSS JOIN verdict_params AS vp
    WHERE random() < vp.verdict_sample_rate
),
verdict_part AS (
    SELECT customer.verdict_vpart AS verdict_vpart,
           count(*) AS __vpsize,
           ((sum(1.0 / NULLIF(customer.verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS numcust,
           ((sum(customer.c_acctbal / NULLIF(customer.verdict_vprob, 0.0)) / count(*)) * sum(count(*)) OVER ()) AS totacctbal
    FROM customer_uf AS customer
    WHERE substring(customer.c_phone from 1 for 2) = '13'
      AND customer.c_acctbal > 0
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
