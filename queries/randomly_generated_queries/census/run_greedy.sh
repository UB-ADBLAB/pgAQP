#!/bin/bash

JD_START=2447070
JD_END=2454587

mapfile -t start_dates < start_dates.txt
mapfile -t end_dates < end_dates.txt
mapfile -t cis < exact_answer.log
mapfile -t offsets < offset.txt

TOTAL=${#start_dates[@]}

for ((i=0; i<TOTAL; i++))
do
    r_ci=${cis[$i]}
    CI=$(awk "BEGIN {print $r_ci * 0.005}")

    r_offset=${offsets[$i]}
    OFFSET=$(awk "BEGIN { prod=$r_offset * 200; print (prod < 100000 ? prod : 100000) }")

    D1=${start_dates[$i]}
    D2=${end_dates[$i]}

    offset=offset
    
    if [ $i -eq 0 ]
    then
        cat census_greedy_setting.sql
    fi
    echo "\timing off"
    echo "SELECT '====RANGE', $D1 ,$D2, $CI, $OFFSET;"
    echo "\timing on"
    for ((j = 0; j < 5; ++j))
    do
         cat census_sizeopt.sql |\
            sed "s/%INI%/$OFFSET/" |\
            sed "s/%CI%/$CI/" |\
            sed "s/%D1%/$D1/" |\
            sed "s/%D2%/$D2/"
    done
done | psql -p 8654 test_dataset 2>&1 | tee census_greedy_answer.log

