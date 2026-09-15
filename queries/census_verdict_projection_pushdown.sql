\timing on

--Stratified Samplingx10

set max_parallel_workers_per_gather to 0;
set max_parallel_workers to 0;

with vt45_7 as (
          SELECT "hours_per_week" AS "hours_per_week",
               count(*) AS "verdict_group_size"
          FROM aqp.census_income_duplicate AS vt29 GROUP BY "hours_per_week"),
     vt45_8 as (
          SELECT s."income" AS "income",
               s."hours_per_week" AS "hours_per_week",
               "verdict_group_size" AS "verdict_group_size"
          FROM
          (SELECT *,
                    random() AS "verdict_rand"
          FROM aqp.census_income_duplicate AS vt34) AS s
          INNER JOIN vt45_7 AS t ON (CASE
                                                  WHEN (s."hours_per_week" IS NULL) THEN -9223372036854775807
                                                  ELSE s."hours_per_week"
                                                  END) = (CASE
                                                            WHEN (t."hours_per_week" IS NULL) THEN -9223372036854775807
                                                            ELSE t."hours_per_week"
                                                       END)
          WHERE ("verdict_rand" < ((((select sum(verdict_group_size) from vt45_7) * 0.03) / (select count(*) from vt45_7)) / "verdict_group_size"))),
     census_st as (
          SELECT s."income" AS "income",
               s."hours_per_week" AS "hours_per_week",
               (cast("verdict_group_size_in_sample" AS float) / cast("verdict_group_size" AS float)) AS "verdict_vprob",
               MOD (cast(round((random() * 100)) AS integer),
                    100) AS "verdict_vpart"
          FROM vt45_8 AS s
          INNER JOIN
          (SELECT "hours_per_week" AS "hours_per_week",
                    count(*) AS "verdict_group_size_in_sample" FROM vt45_8 AS vt40
          GROUP BY "hours_per_week") AS t ON (CASE
                                                  WHEN (s."hours_per_week" IS NULL) THEN -9223372036854775807
                                                  ELSE s."hours_per_week"
                                             END) = (CASE
                                                       WHEN (t."hours_per_week" IS NULL) THEN -9223372036854775807
                                                       ELSE t."hours_per_week"
                                                  END))
     SELECT round((sum((vt52."c_113" * vt52."__vpsize")) / sum(vt52."__vpsize"))) AS "c_113",
          (((stddev(vt52."c_113") * sqrt(avg(vt52."__vpsize"))) / sqrt(sum(vt52."__vpsize"))) * 1.96) AS "c_113_err"
     FROM
     (SELECT ((sum((1.0 / vt50."verdict_vprob")) / count(*)) * sum(count(*)) OVER ()) AS "c_113",
               vt50."verdict_vpart" AS "verdict_vpart",
               count(*) AS "__vpsize",
               avg(1.0) AS "verdict_vprob"
     FROM census_st AS vt50
     WHERE (vt50."income" is true)
          AND ((vt50."hours_per_week" >= 1)
               AND (vt50."hours_per_week" < 100))
     GROUP BY vt50."verdict_vpart") AS vt52;
