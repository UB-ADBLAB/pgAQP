#!/bin/bash

MIN=2448624
MAX=2451088
RANGE=$(( MAX - MIN + 1 ))

declare -A intervals
count=0

while [ $count -lt 50 ]
do
    num1=$(( (RANDOM % (RANGE + 1)) + MIN ))
    num2=$(( (RANDOM % (RANGE + 1)) + MIN ))
    
    if [ $num1 -eq $num2 ]; then 
        continue
    fi

    if [ $num1 -le $num2 ]; then
        start=$num1
        end=$num2
    else
        start=$num2
        end=$num1
    fi
    offset=$(( end - start ))

    key="${start}-${end}"
    if [[ -z "${intervals[$key]}" ]]; then
        intervals[$key]=1

        echo $start >> start_dates.txt
        echo $offset >> offset_old.txt
        echo $end >> end_dates.txt

        ((count++))
    fi
done
