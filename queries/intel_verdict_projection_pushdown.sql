\timing on

--Stratified Samplingx10

set max_parallel_workers_per_gather to 0;
set max_parallel_workers to 0;

with vt45_7 as (
          SELECT "date" AS "date",
               count(*) AS "verdict_group_size"
          FROM aqp.intel_lab_1000 AS vt29 GROUP BY "date"),
     vt45_8 as (
          SELECT s."temperature" AS "temperature",
               s."date" AS "date",
               "verdict_group_size" AS "verdict_group_size"
          FROM
          (SELECT *,
                    random() AS "verdict_rand"
          FROM aqp.intel_lab_1000 AS vt34) AS s
          INNER JOIN vt45_7 AS t ON (CASE
                                                  WHEN (s."date" IS NULL) THEN '1900-01-01'
                                                  ELSE s."date"
                                                  END) = (CASE
                                                            WHEN (t."date" IS NULL) THEN '1900-01-01'
                                                            ELSE t."date"
                                                       END)
          WHERE ("verdict_rand" < ((((select sum(verdict_group_size) from vt45_7) * 0.000065) / (select count(*) from vt45_7)) / "verdict_group_size"))),
     flights_st as (
          SELECT s."temperature" AS "temperature",
               s."date" AS "date",
               (cast("verdict_group_size_in_sample" AS float) / cast("verdict_group_size" AS float)) AS "verdict_vprob",
               MOD (cast(round((random() * 100)) AS integer),
                    100) AS "verdict_vpart"
          FROM vt45_8 AS s
          INNER JOIN
          (SELECT "date" AS "date",
                    count(*) AS "verdict_group_size_in_sample" FROM vt45_8 AS vt40
          GROUP BY "date") AS t ON (CASE
                                                  WHEN (s."date" IS NULL) THEN '1900-01-01'
                                                  ELSE s."date"
                                             END) = (CASE
                                                       WHEN (t."date" IS NULL) THEN '1900-01-01'
                                                       ELSE t."date"
                                                  END))
     SELECT round((sum((vt52."c_113" * vt52."__vpsize")) / sum(vt52."__vpsize"))) AS "c_113",
          (((stddev(vt52."c_113") * sqrt(avg(vt52."__vpsize"))) / sqrt(sum(vt52."__vpsize"))) * 1.96) AS "c_113_err"
     FROM
     (SELECT ((sum((1.0 / vt50."verdict_vprob")) / count(*)) * sum(count(*)) OVER ()) AS "c_113",
               vt50."verdict_vpart" AS "verdict_vpart",
               count(*) AS "__vpsize",
               avg(1.0) AS "verdict_vprob"
     FROM flights_st AS vt50
     WHERE (vt50."temperature" > 27)
          AND ((vt50."date" >= '2004-02-28')
               AND (vt50."date" < '2004-04-05'))
     GROUP BY vt50."verdict_vpart") AS vt52;
