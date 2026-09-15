#!/bin/bash

declare -A intervals
count=0

while [ $count -lt 50 ]
do
    num1=$(( (RANDOM % 99) + 1 ))
    num2=$(( (RANDOM % 99) + 1 ))
    
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
        echo $offset >> offset.txt
        echo $end >> end_dates.txt

        ((count++))
    fi
done
