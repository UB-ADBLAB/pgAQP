pgAQP: A PostgreSQL extension for approximate query processing
======================
# Overview
This is the code base for OptiAQP: Index-Assisted Stratified Sampling for Online Aggregation (ICDE '27) [arXiv preprint](https://arxiv.org/abs/2604.28141). In this work, we design optimized index-assisted stratified sampling method for online aggregation style approximate query processing.

# Environment

We tested on Ubuntu Linux 22.04 LTS. Source code were built with GCC 11.3, with
-O3 flags.

# How to build

1. Build PostgreSQL 13.1 with AB-tree in the `pg13_abtree` directory. We use the following flags and commands:

        cd pg13_abtree
        CFLAGS=-O3 ./configure --prefix=<installation-path> --disable-debug
        make
        make install

3. Export `PGDATA` to some data path, and add the `bin` directory under the
installation path to the `PATH` environment variable.

4. Build the OptiAQP extension:

        cd aqp_pgext
        make
        make install

5. The TPC-H data generator with special date ranges is available under dbgen. To generate data:
    
        cd dbgen
        make

    Then you may customize `pswr_tpch_table.sh` to update `n` (the number of high variance date ranges) and `s` the scale factors.

    This version of dbgen is modified from [the TPC-H skewed data generator from Microsoft Research](https://github.com/SrikanthKandula/tpch_dbgen_zipf_skew).

6. The other datasets are available through the following links:
    - [flight](https://doi.org/10.7910/DVN/HG7NV7) (we enlarged to 10x)
    - [intel](https://db.csail.mit.edu/labdata/labdata.html) (we enlarged to 1000x)
    - [census](https://doi.org/10.24432/C5GP7S) (we enlarged to 10000x)

7. Queries used in the experiments are under queries.



