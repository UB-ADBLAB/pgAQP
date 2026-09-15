#!/bin/bash

JD_START=2447070
JD_END=2454587

mapfile -t start_dates < start_dates.txt
mapfile -t end_dates < end_dates.txt

TOTAL=${#start_dates[@]}

for ((i=0; i<TOTAL; i++))
do
    D1=${start_dates[$i]}
    D2=${end_dates[$i]}

    echo "SELECT '====RANGE', $D1 ,$D2;"
    cat flight_exact.sql |\
        sed "s/%D1%/$D1/" |\
        sed "s/%D2%/$D2/"

done | psql -p 8765 pswr_test 2>&1 | tee flight_exact_answer.log

