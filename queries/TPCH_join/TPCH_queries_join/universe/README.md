Q1: single-table
table: lineitem
uniform
stratified

Q2: universe
table: partsupp/part
key: ps_partkey

Q3: universe
table: lineitem
key: l_orderkey

Q4: universe
table: lineitem
key: l_orderkey

Q5: universe
table: lineitem
key: l_orderkey

Q6: single-table
table: lineitem
uniform
stratified

Q7: universe
table: lineitem
key: l_orderkey

Q8: universe
table: lineitem
key: l_orderkey

Q9: universe
table: lineitem/orders
key: l_orderkey

Q10: universe
table: lineitem
key: l_orderkey

Q11: universe
table: partsupp
key: ps_suppkey

Q12: universe
table: lineitem
key: l_orderkey

Q13: universe
table: orders
key: o_custkey

Q14: universe
table: lineitem/part
key: l_partkey

Q15: universe
table: lineitem
key: l_suppkey

Q16: universe
table: partsupp
key: ps_suppkey

Q17: universe
table: lineitem
key: l_partkey

Q18: universe
table: lineitem
key: l_orderkey

Q19: universe
table: lineitem/part
key: l_partkey

Q20: universe
table: lineitem/partsupp
key: (l_partkey, l_suppkey)

Q21: universe
table: lineitem l1
key: l1.l_orderkey

Q22: single-table
table: customer
uniform
stratified
stratified
