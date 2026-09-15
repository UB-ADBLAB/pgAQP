set pswr_chosen_plan_index to 0;
-- use index over l_shipdate

SELECT
    approx_sum(l_quantity) AS sum_qty,
    approx_sum_half_ci(l_quantity, 0.95) AS sum_qty_ci
FROM
    lineitem_60 TABLESAMPLE pswr(30000000, 100000, 0.05, 0.95)
WHERE
    l_shipdate <= date '1998-12-01' - interval '90' day
    AND l_returnflag = 'A'
    AND l_linestatus = 'F';

SELECT
    approx_sum(l_extendedprice) AS sum_base_price,
    approx_sum_half_ci(l_extendedprice, 0.95) AS sum_base_price_ci
FROM
    lineitem_60 TABLESAMPLE pswr(30000000, 100000, 0.05, 0.95)
WHERE
    l_shipdate <= date '1998-12-01' - interval '90' day
    AND l_returnflag = 'A'
    AND l_linestatus = 'F';

SELECT
    approx_sum(l_extendedprice * (1 - l_discount)) AS sum_disc_price,
    approx_sum_half_ci(l_extendedprice * (1 - l_discount), 0.95) AS sum_disc_price_ci
FROM
    lineitem_60 TABLESAMPLE pswr(30000000, 100000, 0.05, 0.95)
WHERE
    l_shipdate <= date '1998-12-01' - interval '90' day
    AND l_returnflag = 'A'
    AND l_linestatus = 'F';

SELECT
    approx_sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge,
    approx_sum_half_ci(l_extendedprice * (1 - l_discount) * (1 + l_tax), 0.95) AS sum_charge_ci
FROM
    lineitem_60 TABLESAMPLE pswr(30000000, 100000, 0.05, 0.95)
WHERE
    l_shipdate <= date '1998-12-01' - interval '90' day
    AND l_returnflag = 'A'
    AND l_linestatus = 'F';

SELECT
    approx_count(*) AS count_order,
    approx_count_star_half_ci(0.95) AS count_ci
FROM
    lineitem_60 TABLESAMPLE pswr(30000000, 100000, 0.05, 0.95)
WHERE
    l_shipdate <= date '1998-12-01' - interval '90' day
    AND l_returnflag = 'A'
    AND l_linestatus = 'F';