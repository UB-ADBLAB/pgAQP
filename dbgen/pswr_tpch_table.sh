#!/bin/bash

#Z: 0.9 1.3 1.9
#D: 30
#A: 60
#predict: days > 49


./dbgen -vf -T o -s 1 -z $1 -n 5 -D 30 -A 60 -F 2 >> LINEITEM_s1_z$1_n5_D$2_A$3_front_date.txt





CREATE TABLE LINEITEM_s1_z$1_n5_D$2_A$3_front (L_ORDERKEY        INTEGER NOT NULL,
                                               L_PARTKEY         INTEGER NOT NULL,
                                               L_SUPPKEY         INTEGER NOT NULL,
                                               L_LINENUMBER      INTEGER NOT NULL,
                                               L_QUANTITY        DECIMAL(15,2) NOT NULL,
                                               L_EXTENDEDPRICE   DECIMAL(15,2) NOT NULL,
                                               L_DISCOUNT        DECIMAL(15,2) NOT NULL,
                                               L_TAX             DECIMAL(15,2) NOT NULL,
                                               L_RETURNFLAG      CHAR(1) NOT NULL,
                                               L_LINESTATUS      CHAR(1) NOT NULL,
                                               L_SHIPDATE        DATE NOT NULL,
                                               L_COMMITDATE      DATE NOT NULL,
                                               L_RECEIPTDATE     DATE NOT NULL,
                                               L_SHIPINSTRUCT    CHAR(25) NOT NULL,
                                               L_SHIPMODE        CHAR(10) NOT NULL,
                                               L_COMMENT         VARCHAR(44) NOT NULL);
copy LINEITEM_s1_z$1_n5_D$2_A$3_front from 'lineitem.tbl' csv header delimiter '|';

alter table LINEITEM_s1_z$1_n5_D$2_A$3_front add shipdate_julian int;
update LINEITEM_s1_z$1_n5_D$2_A$3_front set shipdate_julian = extract(julian from l_shipdate);  
