#!/bin/bash

psql -p 8654 test_dataset -c "create table intel_lab_1000_new (date date not null, time time without time zone not null, epoch int not null, moteid int not null, temperature real, humidity real, light real, voltage real);"

psql -p 8654 test_dataset -c "
create index on intel_lab_1000_new using abtree(date) with (aggregation_type = int8, agg_support = abt_count_support);"

for ((i = 1; i <= 1; i++))
do
    psql -p 8654 test_dataset -c "
    copy intel_lab_1000_new from 'intel_lab_delete.csv' csv header DELIMITER ',';"
    echo $i
done
