 CREATE TABLE LINEITEM_3_20 (L_ORDERKEY        INTEGER NOT NULL,                                                                                 
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

create index on lineitem_3_20 using abtree(shipdate) with (aggregation_type = int8, agg_support = abt_count_support);
