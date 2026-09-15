#!/bin/bash

psql -p 8654 test_dataset -c "
CREATE TABLE if not exists census_income_duplicate(age    INTEGER NOT NULL,
workclass    char(25),
fnlwgt       INTEGER NOT NULL,
education    char(25) NOT NULL,
education_num        INTEGER NOT NULL,
marital_status       char(25) NOT NULL,
occupation   char(25),
relationship char(25) NOT NULL,
race CHAR(25) NOT NULL,
sex  CHAR(25) NOT NULL,
capital_gain INTEGER NOT NULL,
capital_loss INTEGER NOT NULL,
hours_per_week       INTEGER NOT NULL,
native_country       CHAR(50) NOT NULL,
income       bool);"

for ((i = 1; i <= 10000; i++))
do
    psql -p 8654 test_dataset -c "
    copy census_income_duplicate from 'adult_bi.csv' csv header DELIMITER ',';"
    echo $i
done

psql -p 8654 test_dataset -c "
create index on census_income_duplicate using abtree(hours_per_week) with (aggregation_type = int8, agg_support = abt_count_support);"

