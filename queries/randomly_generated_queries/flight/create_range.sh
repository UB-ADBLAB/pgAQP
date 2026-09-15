#!/bin/bash

JD_START=2447070
JD_END=2454587
COUNT=1000

mapfile -t start_dates < <(shuf -i ${JD_START}-$((JD_END-1)) -n ${COUNT} | tee start_dates.txt)

for DS in "${start_dates[@]}"
do
    L_DE=$((DS + 1))
    remain_days=$((JD_END - L_DE))
    if [ "$remain_days" -eq 0 ] 
    then
        OFFSET=0
    else
        OFFSET=$(((RANDOM << 15 | RANDOM) % (remain_days + 1)))
    fi

    DE=$((L_DE + OFFSET))
    echo "$OFFSET" >> offset.txt
    echo "$DE" >> end_dates.txt
done 
