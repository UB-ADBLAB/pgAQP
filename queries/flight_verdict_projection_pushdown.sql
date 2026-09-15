\timing on

--Stratified Samplingx10

set max_parallel_workers_per_gather to 0;
set max_parallel_workers to 0;

with vt45_7 as (
          SELECT "julian_date" AS "julian_date",
               count(*) AS "verdict_group_size"
          FROM aqp.flights_julianx10 AS vt29 GROUP BY "julian_date"),
     vt45_8 as (
          SELECT s."cancelled" AS "cancelled",
               s."julian_date" AS "julian_date",
               "verdict_group_size" AS "verdict_group_size"
          FROM
          (SELECT *,
                    random() AS "verdict_rand"
          FROM aqp.flights_julianx10 AS vt34) AS s
          INNER JOIN vt45_7 AS t ON (CASE
                                                  WHEN (s."julian_date" IS NULL) THEN -9223372036854775807
                                                  ELSE s."julian_date"
                                                  END) = (CASE
                                                            WHEN (t."julian_date" IS NULL) THEN -9223372036854775807
                                                            ELSE t."julian_date"
                                                       END)
          WHERE ("verdict_rand" < ((((select sum(verdict_group_size) from vt45_7) * 0.0001) / (select count(*) from vt45_7)) / "verdict_group_size"))),
     flights_st as (
          SELECT s."cancelled" AS "cancelled",
               s."julian_date" AS "julian_date",
               (cast("verdict_group_size_in_sample" AS float) / cast("verdict_group_size" AS float)) AS "verdict_vprob",
               MOD (cast(round((random() * 100)) AS integer),
                    100) AS "verdict_vpart"
          FROM vt45_8 AS s
          INNER JOIN
          (SELECT "julian_date" AS "julian_date",
                    count(*) AS "verdict_group_size_in_sample" FROM vt45_8 AS vt40
          GROUP BY "julian_date") AS t ON (CASE
                                                  WHEN (s."julian_date" IS NULL) THEN -9223372036854775807
                                                  ELSE s."julian_date"
                                             END) = (CASE
                                                       WHEN (t."julian_date" IS NULL) THEN -9223372036854775807
                                                       ELSE t."julian_date"
                                                  END))
     SELECT round((sum((vt52."c_113" * vt52."__vpsize")) / sum(vt52."__vpsize"))) AS "c_113",
          (((stddev(vt52."c_113") * sqrt(avg(vt52."__vpsize"))) / sqrt(sum(vt52."__vpsize"))) * 1.96) AS "c_113_err"
     FROM
     (SELECT ((sum((1.0 / vt50."verdict_vprob")) / count(*)) * sum(count(*)) OVER ()) AS "c_113",
               vt50."verdict_vpart" AS "verdict_vpart",
               count(*) AS "__vpsize",
               avg(1.0) AS "verdict_vprob"
     FROM flights_st AS vt50
     WHERE (vt50."cancelled" = 1)
          AND ((vt50."julian_date" >= 2452158)
               AND (vt50."julian_date" < 2452174))
     GROUP BY vt50."verdict_vpart") AS vt52;
