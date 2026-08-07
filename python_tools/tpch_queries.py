#!/usr/bin/env python3
"""
tpch_queries.py — the 22 TPC-H queries as templates, plus their parameters.

WHAT WEEK 35 OWNS HERE, AND WHAT IT DOES NOT. Week 35 builds the harness; Week
36 is "Port queries to the documented SwiftQL dialect / Close query-specific
parser, execution and optimizer correctness gaps". So these templates are
written in the SwiftQL dialect wherever the rewrite is already DOCUMENTED and
mechanical, and a template the dialect still cannot express is a RECORDED
OUTCOME rather than a blocker — run_tpch.py has an UNPORTED state for exactly
that, and the list of them IS Week 36's worklist.

Dialect rewrites already decided elsewhere and applied here:

  * `extract(year from d)`  ->  `SUBSTRING(d, 1, 4)`.  README Week 25's
    hand-forward. Note the result is a STRING year where the TPC-H answers show
    an integer; normalize()'s coerce() runs float() on both sides, so '1995' and
    1995 already compare equal. That coincidence is recorded rather than relied
    on -- Week 36 decides whether to keep normalizing or grow a conversion.
  * `substring(x from a for b)`  ->  `SUBSTRING(x, a, b)`.
  * Comma joins  ->  explicit `JOIN ... ON`. The parser has no comma-join form
    (`unexpected trailing input ... (got ',')`), and every TPC-H comma join has
    an equivalent ON condition in the WHERE clause.
  * `<>`  ->  `!=`. The lexer has no `<>` token; `!=` is the dialect's
    inequality. Affects Q12, Q16 and Q21.
  * `create view` (Q15)  ->  a derived table (Week 34), whose columns are
    QUALIFIED by the alias. An unqualified reference to a derived column
    reported `column not found` where two derived relations in the query
    project the same name.
  * Interval arithmetic on a constant date is supported and constant-folded
    (Week 25), so `date '1994-01-01' + interval '1' year` is left as-is where
    the standard text uses it -- except that a placeholder cannot appear inside
    a date literal, so bounded ranges are given as two pre-computed dates.

PARAMETERS. TPC-H templates carry substitution parameters. The spec's
*validation* parameters are the ones a published answer set corresponds to, and
they are what CORRECTNESS runs use -- a randomized parameter makes a correctness
result unrepeatable, which is the opposite of this week's checkpoint. Week 37
may draw seeded random parameters for BENCHMARK runs; that is a different use
and must not share this default.

Substitution is by :NAME with an explicit longest-name-first replace. Neither
%-formatting nor str.format is usable: TPC-H bodies contain '%' inside LIKE
patterns and both would misfire.
"""

# Every entry: qid -> SQL template with :NAME placeholders.
TEMPLATES = {

    "q1": """
        SELECT l_returnflag, l_linestatus,
               SUM(l_quantity) AS sum_qty,
               SUM(l_extendedprice) AS sum_base_price,
               SUM(l_extendedprice * (1 - l_discount)) AS sum_disc_price,
               SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge,
               AVG(l_quantity) AS avg_qty,
               AVG(l_extendedprice) AS avg_price,
               AVG(l_discount) AS avg_disc,
               COUNT(*) AS count_order
        FROM lineitem
        WHERE l_shipdate <= ':DELTA_DATE'
        GROUP BY l_returnflag, l_linestatus
        ORDER BY l_returnflag, l_linestatus
    """,

    # Correlated scalar subquery over an aggregate body whose body itself joins
    # four relations -- the Week 34 rewrite's outer edge. If it is refused, the
    # refusal message is the finding.
    "q2": """
        SELECT s_acctbal, s_name, n_name, p_partkey, p_mfgr, s_address, s_phone, s_comment
        FROM part
        JOIN partsupp ON p_partkey = ps_partkey
        JOIN supplier ON s_suppkey = ps_suppkey
        JOIN nation ON s_nationkey = n_nationkey
        JOIN region ON n_regionkey = r_regionkey
        WHERE p_size = :SIZE
          AND p_type LIKE '%:TYPE'
          AND r_name = ':REGION'
          AND ps_supplycost = (
              SELECT MIN(ps_supplycost)
              FROM partsupp
              JOIN supplier ON s_suppkey = ps_suppkey
              JOIN nation ON s_nationkey = n_nationkey
              JOIN region ON n_regionkey = r_regionkey
              WHERE p_partkey = ps_partkey AND r_name = ':REGION')
        ORDER BY s_acctbal DESC, n_name, s_name, p_partkey
        LIMIT 100
    """,

    "q3": """
        SELECT l_orderkey,
               SUM(l_extendedprice * (1 - l_discount)) AS revenue,
               o_orderdate, o_shippriority
        FROM customer
        JOIN orders ON c_custkey = o_custkey
        JOIN lineitem ON l_orderkey = o_orderkey
        WHERE c_mktsegment = ':SEGMENT'
          AND o_orderdate < ':DATE'
          AND l_shipdate > ':DATE'
        GROUP BY l_orderkey, o_orderdate, o_shippriority
        ORDER BY revenue DESC, o_orderdate
        LIMIT 10
    """,

    # Correlated EXISTS -- Week 33's decorrelation to a semi-join.
    "q4": """
        SELECT o_orderpriority, COUNT(*) AS order_count
        FROM orders
        WHERE o_orderdate >= ':DATE'
          AND o_orderdate < ':DATE_PLUS_3M'
          AND EXISTS (SELECT * FROM lineitem
                      WHERE l_orderkey = o_orderkey AND l_commitdate < l_receiptdate)
        GROUP BY o_orderpriority
        ORDER BY o_orderpriority
    """,

    "q5": """
        SELECT n_name, SUM(l_extendedprice * (1 - l_discount)) AS revenue
        FROM customer
        JOIN orders ON c_custkey = o_custkey
        JOIN lineitem ON l_orderkey = o_orderkey
        JOIN supplier ON l_suppkey = s_suppkey AND c_nationkey = s_nationkey
        JOIN nation ON s_nationkey = n_nationkey
        JOIN region ON n_regionkey = r_regionkey
        WHERE r_name = ':REGION'
          AND o_orderdate >= ':DATE'
          AND o_orderdate < ':DATE_PLUS_1Y'
        GROUP BY n_name
        ORDER BY revenue DESC
    """,

    "q6": """
        SELECT SUM(l_extendedprice * l_discount) AS revenue
        FROM lineitem
        WHERE l_shipdate >= ':DATE'
          AND l_shipdate < ':DATE_PLUS_1Y'
          AND l_discount BETWEEN :DISCOUNT_LO AND :DISCOUNT_HI
          AND l_quantity < :QUANTITY
    """,

    # extract(year from l_shipdate) -> SUBSTRING(l_shipdate, 1, 4): a STRING year.
    "q7": """
        SELECT supp_nation, cust_nation, l_year, SUM(volume) AS revenue
        FROM (
            SELECT n1.n_name AS supp_nation, n2.n_name AS cust_nation,
                   SUBSTRING(l_shipdate, 1, 4) AS l_year,
                   l_extendedprice * (1 - l_discount) AS volume
            FROM supplier
            JOIN lineitem ON s_suppkey = l_suppkey
            JOIN orders ON o_orderkey = l_orderkey
            JOIN customer ON c_custkey = o_custkey
            JOIN nation n1 ON s_nationkey = n1.n_nationkey
            JOIN nation n2 ON c_nationkey = n2.n_nationkey
            WHERE l_shipdate BETWEEN '1995-01-01' AND '1996-12-31'
        ) AS shipping
        WHERE (supp_nation = ':NATION1' AND cust_nation = ':NATION2')
           OR (supp_nation = ':NATION2' AND cust_nation = ':NATION1')
        GROUP BY supp_nation, cust_nation, l_year
        ORDER BY supp_nation, cust_nation, l_year
    """,

    "q8": """
        SELECT o_year,
               SUM(CASE WHEN nation = ':NATION' THEN volume ELSE 0 END) / SUM(volume) AS mkt_share
        FROM (
            SELECT SUBSTRING(o_orderdate, 1, 4) AS o_year,
                   l_extendedprice * (1 - l_discount) AS volume,
                   n2.n_name AS nation
            FROM part
            JOIN lineitem ON p_partkey = l_partkey
            JOIN supplier ON s_suppkey = l_suppkey
            JOIN orders ON l_orderkey = o_orderkey
            JOIN customer ON o_custkey = c_custkey
            JOIN nation n1 ON c_nationkey = n1.n_nationkey
            JOIN region ON n1.n_regionkey = r_regionkey
            JOIN nation n2 ON s_nationkey = n2.n_nationkey
            WHERE r_name = ':REGION'
              AND o_orderdate BETWEEN '1995-01-01' AND '1996-12-31'
              AND p_type = ':TYPE'
        ) AS all_nations
        GROUP BY o_year
        ORDER BY o_year
    """,

    "q9": """
        SELECT nation, o_year, SUM(amount) AS sum_profit
        FROM (
            SELECT n_name AS nation,
                   SUBSTRING(o_orderdate, 1, 4) AS o_year,
                   l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity AS amount
            FROM part
            JOIN lineitem ON p_partkey = l_partkey
            JOIN supplier ON s_suppkey = l_suppkey
            JOIN partsupp ON ps_suppkey = l_suppkey AND ps_partkey = l_partkey
            JOIN orders ON o_orderkey = l_orderkey
            JOIN nation ON s_nationkey = n_nationkey
            WHERE p_name LIKE '%:COLOR%'
        ) AS profit
        GROUP BY nation, o_year
        ORDER BY nation, o_year DESC
    """,

    "q10": """
        SELECT c_custkey, c_name,
               SUM(l_extendedprice * (1 - l_discount)) AS revenue,
               c_acctbal, n_name, c_address, c_phone, c_comment
        FROM customer
        JOIN orders ON c_custkey = o_custkey
        JOIN lineitem ON l_orderkey = o_orderkey
        JOIN nation ON c_nationkey = n_nationkey
        WHERE o_orderdate >= ':DATE'
          AND o_orderdate < ':DATE_PLUS_3M'
          AND l_returnflag = 'R'
        GROUP BY c_custkey, c_name, c_acctbal, c_phone, n_name, c_address, c_comment
        ORDER BY revenue DESC
        LIMIT 20
    """,

    # Uncorrelated scalar subquery in HAVING -- Week 31's materialization.
    "q11": """
        SELECT ps_partkey, SUM(ps_supplycost * ps_availqty) AS value
        FROM partsupp
        JOIN supplier ON ps_suppkey = s_suppkey
        JOIN nation ON s_nationkey = n_nationkey
        WHERE n_name = ':NATION'
        GROUP BY ps_partkey
        HAVING SUM(ps_supplycost * ps_availqty) > (
            SELECT SUM(ps_supplycost * ps_availqty) * :FRACTION
            FROM partsupp
            JOIN supplier ON ps_suppkey = s_suppkey
            JOIN nation ON s_nationkey = n_nationkey
            WHERE n_name = ':NATION')
        ORDER BY value DESC
    """,

    "q12": """
        SELECT l_shipmode,
               SUM(CASE WHEN o_orderpriority = '1-URGENT' OR o_orderpriority = '2-HIGH'
                        THEN 1 ELSE 0 END) AS high_line_count,
               SUM(CASE WHEN o_orderpriority != '1-URGENT' AND o_orderpriority != '2-HIGH'
                        THEN 1 ELSE 0 END) AS low_line_count
        FROM orders
        JOIN lineitem ON o_orderkey = l_orderkey
        WHERE l_shipmode IN (':SHIPMODE1', ':SHIPMODE2')
          AND l_commitdate < l_receiptdate
          AND l_shipdate < l_commitdate
          AND l_receiptdate >= ':DATE'
          AND l_receiptdate < ':DATE_PLUS_1Y'
        GROUP BY l_shipmode
        ORDER BY l_shipmode
    """,

    # LEFT OUTER JOIN (Week 29) plus a derived table (Week 34).
    "q13": """
        SELECT c_count, COUNT(*) AS custdist
        FROM (
            SELECT c_custkey AS c_custkey, COUNT(o_orderkey) AS c_count
            FROM customer
            LEFT JOIN orders ON c_custkey = o_custkey
                            AND o_comment NOT LIKE '%:WORD1%:WORD2%'
            GROUP BY c_custkey
        ) AS c_orders
        GROUP BY c_count
        ORDER BY custdist DESC, c_count DESC
    """,

    "q14": """
        SELECT 100.00 * SUM(CASE WHEN p_type LIKE 'PROMO%'
                                 THEN l_extendedprice * (1 - l_discount)
                                 ELSE 0 END) /
               SUM(l_extendedprice * (1 - l_discount)) AS promo_revenue
        FROM lineitem
        JOIN part ON l_partkey = p_partkey
        WHERE l_shipdate >= ':DATE'
          AND l_shipdate < ':DATE_PLUS_1M'
    """,

    # The standard text creates a VIEW. Rewritten as a derived table (Week 34),
    # joined to itself so the MAX(total_revenue) comparison has both sides.
    "q15": """
        SELECT s_suppkey, s_name, s_address, s_phone, revenue0.total_revenue
        FROM supplier
        JOIN (
            SELECT l_suppkey AS supplier_no,
                   SUM(l_extendedprice * (1 - l_discount)) AS total_revenue
            FROM lineitem
            WHERE l_shipdate >= ':DATE' AND l_shipdate < ':DATE_PLUS_3M'
            GROUP BY l_suppkey
        ) AS revenue0 ON s_suppkey = revenue0.supplier_no
        WHERE revenue0.total_revenue = (
            SELECT MAX(revenue1.total_revenue)
            FROM (
                SELECT l_suppkey AS supplier_no,
                       SUM(l_extendedprice * (1 - l_discount)) AS total_revenue
                FROM lineitem
                WHERE l_shipdate >= ':DATE' AND l_shipdate < ':DATE_PLUS_3M'
                GROUP BY l_suppkey
            ) AS revenue1)
        ORDER BY s_suppkey
    """,

    # NOT IN (subquery) -> Week 32's anti-join, plus COUNT(DISTINCT) -> Week 34.
    "q16": """
        SELECT p_brand, p_type, p_size, COUNT(DISTINCT ps_suppkey) AS supplier_cnt
        FROM partsupp
        JOIN part ON p_partkey = ps_partkey
        WHERE p_brand != ':BRAND'
          AND p_type NOT LIKE ':TYPE%'
          AND p_size IN (:S1, :S2, :S3, :S4, :S5, :S6, :S7, :S8)
          AND ps_suppkey NOT IN (SELECT s_suppkey FROM supplier
                                 WHERE s_comment LIKE '%Customer%Complaints%')
        GROUP BY p_brand, p_type, p_size
        ORDER BY supplier_cnt DESC, p_brand, p_type, p_size
    """,

    # Correlated scalar subquery over an aggregate body -- Week 34's headline.
    "q17": """
        SELECT SUM(l_extendedprice) / 7.0 AS avg_yearly
        FROM lineitem
        JOIN part ON p_partkey = l_partkey
        WHERE p_brand = ':BRAND'
          AND p_container = ':CONTAINER'
          AND l_quantity < (SELECT 0.2 * AVG(l_quantity) FROM lineitem
                            WHERE l_partkey = p_partkey)
    """,

    # IN (subquery) whose body has a HAVING -- Week 32's lowering.
    "q18": """
        SELECT c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice,
               SUM(l_quantity) AS qty
        FROM customer
        JOIN orders ON c_custkey = o_custkey
        JOIN lineitem ON o_orderkey = l_orderkey
        WHERE o_orderkey IN (SELECT l_orderkey FROM lineitem
                             GROUP BY l_orderkey HAVING SUM(l_quantity) > :QUANTITY)
        GROUP BY c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice
        ORDER BY o_totalprice DESC, o_orderdate
        LIMIT 100
    """,

    # The OR chain over nullable columns Week 24's three-valued AND/OR exists for.
    "q19": """
        SELECT SUM(l_extendedprice * (1 - l_discount)) AS revenue
        FROM lineitem
        JOIN part ON p_partkey = l_partkey
        WHERE (p_brand = ':BRAND1'
               AND p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG')
               AND l_quantity >= :QTY1 AND l_quantity <= :QTY1 + 10
               AND p_size BETWEEN 1 AND 5
               AND l_shipmode IN ('AIR', 'REG AIR')
               AND l_shipinstruct = 'DELIVER IN PERSON')
           OR (p_brand = ':BRAND2'
               AND p_container IN ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK')
               AND l_quantity >= :QTY2 AND l_quantity <= :QTY2 + 10
               AND p_size BETWEEN 1 AND 10
               AND l_shipmode IN ('AIR', 'REG AIR')
               AND l_shipinstruct = 'DELIVER IN PERSON')
           OR (p_brand = ':BRAND3'
               AND p_container IN ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG')
               AND l_quantity >= :QTY3 AND l_quantity <= :QTY3 + 10
               AND p_size BETWEEN 1 AND 15
               AND l_shipmode IN ('AIR', 'REG AIR')
               AND l_shipinstruct = 'DELIVER IN PERSON')
    """,

    # Nested IN subqueries, the inner one correlated -- the shape Week 33's
    # nested tripwire is about.
    "q20": """
        SELECT s_name, s_address
        FROM supplier
        JOIN nation ON s_nationkey = n_nationkey
        WHERE s_suppkey IN (
                SELECT ps_suppkey FROM partsupp
                WHERE ps_partkey IN (SELECT p_partkey FROM part WHERE p_name LIKE ':COLOR%')
                  AND ps_availqty > 0)
          AND n_name = ':NATION'
        ORDER BY s_name
    """,

    # EXISTS and NOT EXISTS in one query -- semi-join AND anti-join (Week 33).
    "q21": """
        SELECT s_name, COUNT(*) AS numwait
        FROM supplier
        JOIN lineitem l1 ON s_suppkey = l1.l_suppkey
        JOIN orders ON o_orderkey = l1.l_orderkey
        JOIN nation ON s_nationkey = n_nationkey
        WHERE o_orderstatus = 'F'
          AND l1.l_receiptdate > l1.l_commitdate
          AND EXISTS (SELECT * FROM lineitem l2
                      WHERE l2.l_orderkey = l1.l_orderkey AND l2.l_suppkey != l1.l_suppkey)
          AND NOT EXISTS (SELECT * FROM lineitem l3
                          WHERE l3.l_orderkey = l1.l_orderkey
                            AND l3.l_suppkey != l1.l_suppkey
                            AND l3.l_receiptdate > l3.l_commitdate)
          AND n_name = ':NATION'
        GROUP BY s_name
        ORDER BY numwait DESC, s_name
        LIMIT 100
    """,

    # BOTH halves in one query, which is the point of Week 34's Q22 note: the
    # correlated component is a NOT EXISTS (Week 33 decorrelated it), and the
    # derived table custsale is Week 34's. The plan fingerprint is what settles
    # which half is which -- see run_tpch.py's --explain capture.
    "q22": """
        SELECT cntrycode, COUNT(*) AS numcust, SUM(c_acctbal) AS totacctbal
        FROM (
            SELECT SUBSTRING(c_phone, 1, 2) AS cntrycode, c_acctbal AS c_acctbal
            FROM customer
            WHERE SUBSTRING(c_phone, 1, 2) IN
                  (':CC1', ':CC2', ':CC3', ':CC4', ':CC5', ':CC6', ':CC7')
              AND c_acctbal > (SELECT AVG(c_acctbal) FROM customer
                               WHERE c_acctbal > 0.00
                                 AND SUBSTRING(c_phone, 1, 2) IN
                                     (':CC1', ':CC2', ':CC3', ':CC4', ':CC5', ':CC6', ':CC7'))
              AND NOT EXISTS (SELECT * FROM orders WHERE o_custkey = c_custkey)
        ) AS custsale
        GROUP BY cntrycode
        ORDER BY cntrycode
    """,
}

# The spec's validation parameters, adjusted only where a date had to be
# pre-computed because a placeholder cannot sit inside a date literal, or where
# the spec's own value does not DISCRIMINATE on this data -- see below.
#
# WHY A PARAMETER IS EVER CHANGED HERE. The spec's validation parameters were
# chosen against `dbgen` at SF=1. This data comes from python_tools/generate_tpch.py
# at SF=0.01, which reproduces the spec's value DOMAINS but not its distributions
# (PROVENANCE.txt). A parameter selective at SF=1 can therefore narrow to one row
# -- or to none -- here, and the mutation check then correctly reports the query
# as INERT / EMPTY / ALL_NULL: it matched SQLite while asserting nothing.
#
# That is a fault in the PARAMETER, not in the query, and writing the query off as
# vacuous UNDERSTATES the engine. Where a re-chosen parameter makes the query's
# characteristic feature selective again, it is re-chosen here, the smallest
# possible deviation from the spec value, and the deviation is recorded on the
# line. Where no parameter helps, the vacuity verdict stands (q18 -- see MUTATIONS).
VALIDATION_PARAMS = {
    "q1":  {"DELTA_DATE": "1998-09-02"},
    # SIZE: spec 15 -> 1. DEVIATION, measured, not preference. At SIZE=15 the
    # outer filter p_size = 15 AND p_type LIKE '%BRASS' AND r_name = 'EUROPE'
    # narrows to exactly ONE (part, supplier) pair on this data, so the
    # correlated MIN(ps_supplycost) subquery -- the entire point of Q2 -- has
    # nothing left to eliminate and deleting it changes no byte (INERT).
    # Swept all 50 sizes x 5 regions: 172 of 250 combinations DISCRIMINATE.
    # SIZE=1 keeps TYPE and REGION at their spec values and gives base 7 rows
    # -> mutant 8, so the subquery is now exercised.
    "q2":  {"SIZE": "1", "TYPE": "BRASS", "REGION": "EUROPE"},
    "q3":  {"SEGMENT": "BUILDING", "DATE": "1995-03-15"},
    "q4":  {"DATE": "1993-07-01", "DATE_PLUS_3M": "1993-10-01"},
    "q5":  {"REGION": "ASIA", "DATE": "1994-01-01", "DATE_PLUS_1Y": "1995-01-01"},
    "q6":  {"DATE": "1994-01-01", "DATE_PLUS_1Y": "1995-01-01",
            "DISCOUNT_LO": "0.05", "DISCOUNT_HI": "0.07", "QUANTITY": "24"},
    "q7":  {"NATION1": "FRANCE", "NATION2": "GERMANY"},
    "q8":  {"NATION": "BRAZIL", "REGION": "AMERICA",
            "TYPE": "ECONOMY ANODIZED STEEL"},
    "q9":  {"COLOR": "green"},
    "q10": {"DATE": "1993-10-01", "DATE_PLUS_3M": "1994-01-01"},
    "q11": {"NATION": "GERMANY", "FRACTION": "0.0001"},
    "q12": {"SHIPMODE1": "MAIL", "SHIPMODE2": "SHIP",
            "DATE": "1994-01-01", "DATE_PLUS_1Y": "1995-01-01"},
    "q13": {"WORD1": "special", "WORD2": "requests"},
    "q14": {"DATE": "1995-09-01", "DATE_PLUS_1M": "1995-10-01"},
    "q15": {"DATE": "1996-01-01", "DATE_PLUS_3M": "1996-04-01"},
    "q16": {"BRAND": "Brand#45", "TYPE": "MEDIUM POLISHED",
            "S1": "49", "S2": "14", "S3": "23", "S4": "45",
            "S5": "19", "S6": "3", "S7": "36", "S8": "9"},
    "q17": {"BRAND": "Brand#23", "CONTAINER": "MED BOX"},
    "q18": {"QUANTITY": "300"},
    # BRANDs: spec 12/23/34 -> 14/34/23. DEVIATION, measured. At the spec brands
    # NO row matches ANY of the three OR arms on this data, so the query is a SUM
    # over nothing and the check reports ALL_NULL before the mutant is even run --
    # the three-valued OR chain this query exists to exercise asserts nothing.
    # Measured per arm over all 25 generated brands: arm 1 (SM containers, size
    # 1-5) is non-empty for 4 brands, arm 2 (MED, 1-10) for 8, arm 3 (LG, 1-15)
    # for 14. Brand#14 / Brand#34 / Brand#23 is the smallest deviation that makes
    # ALL THREE arms contribute: only BRAND1 leaves the spec's value set, and
    # BRAND2/BRAND3 are the spec's own 23 and 34 with the arms swapped. Result:
    # base revenue 56323.29 (three arms) vs mutant 8473.82 (arm 1 alone), so
    # arms 2 and 3 carry 85% of the answer and neutering them is now visible.
    "q19": {"BRAND1": "Brand#14", "BRAND2": "Brand#34", "BRAND3": "Brand#23",
            "QTY1": "1", "QTY2": "10", "QTY3": "20"},
    "q20": {"COLOR": "forest", "NATION": "CANADA"},
    "q21": {"NATION": "SAUDI ARABIA"},
    "q22": {"CC1": "13", "CC2": "31", "CC3": "23", "CC4": "29",
            "CC5": "30", "CC6": "18", "CC7": "17"},
}

QUERY_IDS = [f"q{i}" for i in range(1, 23)]


# ---------------------------------------------------------------------------
# MUTATIONS — one per query, and the reason this file has them.
#
# A TPC-H harness lies in one specific way: a query MATCHES the oracle while the
# feature it exists to exercise contributed nothing. run_tpch.py's original
# vacuity detector only caught the whole-query case (zero rows on both sides),
# which is why q18 was named and q16 was not — q16 returned 305 rows whose
# anti-join filtered NOTHING, because the synthetic supplier comments contained
# no '%Customer%Complaints%'. Deleting the entire NOT IN gave a byte-identical
# answer, and that pass was counted in the headline figure for a week.
#
# So each query names the ONE predicate that carries its characteristic
# feature, and the check is mechanical: neuter that predicate, ask SQLite for
# both answers, and require them to DIFFER. A query whose mutant answers
# identically is INERT — it is not an answer, it is a coincidence, and it counts
# as unanswered.
#
# Where a query has a subquery/join feature (q2 q4 q11 q13 q15 q16 q17 q18 q20
# q21 q22) the mutation neuters exactly that. Where it does not (q1 q3 q5 q6 q7
# q8 q9 q10 q12 q14 q19) it neuters the spec-literal filter or CASE arm that
# selects the query's subject, because that is the part a generator with the
# wrong value domains would silently turn into a no-op.
#
# Each entry is (label, old_fragment, new_fragment) applied to the
# whitespace-COLLAPSED template BEFORE parameter substitution, so the fragments
# read exactly like the template text above. A fragment that does not occur
# EXACTLY ONCE is an error, not a skip: a mutation that silently fails to apply
# reports every query as discriminating, which is the failure this check exists
# to prevent.
# ---------------------------------------------------------------------------
MUTATIONS = {
    "q1": ("the l_shipdate cutoff",
           "WHERE l_shipdate <= ':DELTA_DATE'", "WHERE 1 = 1"),
    "q2": ("the correlated MIN(ps_supplycost) scalar subquery",
           "AND ps_supplycost = ( SELECT MIN(ps_supplycost) FROM partsupp "
           "JOIN supplier ON s_suppkey = ps_suppkey "
           "JOIN nation ON s_nationkey = n_nationkey "
           "JOIN region ON n_regionkey = r_regionkey "
           "WHERE p_partkey = ps_partkey AND r_name = ':REGION')", ""),
    "q3": ("the c_mktsegment filter",
           "WHERE c_mktsegment = ':SEGMENT'", "WHERE 1 = 1"),
    "q4": ("the correlated EXISTS semi-join",
           "AND EXISTS (SELECT * FROM lineitem WHERE l_orderkey = o_orderkey "
           "AND l_commitdate < l_receiptdate)", ""),
    "q5": ("the customer/supplier same-nation join condition",
           "JOIN supplier ON l_suppkey = s_suppkey AND c_nationkey = s_nationkey",
           "JOIN supplier ON l_suppkey = s_suppkey"),
    "q6": ("the l_discount BETWEEN band",
           "AND l_discount BETWEEN :DISCOUNT_LO AND :DISCOUNT_HI", ""),
    "q7": ("the nation-pair filter over the derived table",
           "WHERE (supp_nation = ':NATION1' AND cust_nation = ':NATION2') "
           "OR (supp_nation = ':NATION2' AND cust_nation = ':NATION1')",
           "WHERE 1 = 1"),
    "q8": ("the p_type filter inside the derived body",
           "AND p_type = ':TYPE'", ""),
    "q9": ("the p_name LIKE '%color%' filter",
           "WHERE p_name LIKE '%:COLOR%'", "WHERE 1 = 1"),
    "q10": ("the l_returnflag = 'R' filter",
            "AND l_returnflag = 'R'", ""),
    "q11": ("the HAVING with its uncorrelated scalar subquery",
            "HAVING SUM(ps_supplycost * ps_availqty) > ( "
            "SELECT SUM(ps_supplycost * ps_availqty) * :FRACTION FROM partsupp "
            "JOIN supplier ON ps_suppkey = s_suppkey "
            "JOIN nation ON s_nationkey = n_nationkey "
            "WHERE n_name = ':NATION')", ""),
    "q12": ("the commit/receipt/ship date ordering",
            "AND l_commitdate < l_receiptdate AND l_shipdate < l_commitdate", ""),
    "q13": ("the LEFT JOIN's outer-ness (customers with no orders)",
            "LEFT JOIN orders", "JOIN orders"),
    "q14": ("the PROMO CASE arm",
            "CASE WHEN p_type LIKE 'PROMO%' THEN l_extendedprice * (1 - l_discount) "
            "ELSE 0 END", "l_extendedprice * (1 - l_discount)"),
    "q15": ("the = (SELECT MAX(total_revenue)) equality",
            "WHERE revenue0.total_revenue = ( SELECT MAX(revenue1.total_revenue) "
            "FROM ( SELECT l_suppkey AS supplier_no, "
            "SUM(l_extendedprice * (1 - l_discount)) AS total_revenue FROM lineitem "
            "WHERE l_shipdate >= ':DATE' AND l_shipdate < ':DATE_PLUS_3M' "
            "GROUP BY l_suppkey ) AS revenue1)", ""),
    "q16": ("the NOT IN (suppliers with complaints) anti-join",
            "AND ps_suppkey NOT IN (SELECT s_suppkey FROM supplier "
            "WHERE s_comment LIKE '%Customer%Complaints%')", ""),
    "q17": ("the correlated 0.2 * AVG(l_quantity) scalar subquery",
            "AND l_quantity < (SELECT 0.2 * AVG(l_quantity) FROM lineitem "
            "WHERE l_partkey = p_partkey)", ""),
    "q18": ("the IN (subquery with HAVING) semi-join",
            "WHERE o_orderkey IN (SELECT l_orderkey FROM lineitem "
            "GROUP BY l_orderkey HAVING SUM(l_quantity) > :QUANTITY)",
            "WHERE 1 = 1"),
    "q19": ("the second and third OR arms",
            "OR (p_brand = ':BRAND2' AND p_container IN ('MED BAG', 'MED BOX', "
            "'MED PKG', 'MED PACK') AND l_quantity >= :QTY2 AND l_quantity <= "
            ":QTY2 + 10 AND p_size BETWEEN 1 AND 10 AND l_shipmode IN ('AIR', "
            "'REG AIR') AND l_shipinstruct = 'DELIVER IN PERSON') OR (p_brand = "
            "':BRAND3' AND p_container IN ('LG CASE', 'LG BOX', 'LG PACK', "
            "'LG PKG') AND l_quantity >= :QTY3 AND l_quantity <= :QTY3 + 10 AND "
            "p_size BETWEEN 1 AND 15 AND l_shipmode IN ('AIR', 'REG AIR') AND "
            "l_shipinstruct = 'DELIVER IN PERSON')", ""),
    "q20": ("the nested IN (subquery) semi-joins",
            "WHERE s_suppkey IN ( SELECT ps_suppkey FROM partsupp "
            "WHERE ps_partkey IN (SELECT p_partkey FROM part "
            "WHERE p_name LIKE ':COLOR%') AND ps_availqty > 0) AND", "WHERE"),
    "q21": ("the NOT EXISTS anti-join",
            "AND NOT EXISTS (SELECT * FROM lineitem l3 "
            "WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey != l1.l_suppkey "
            "AND l3.l_receiptdate > l3.l_commitdate)", ""),
    "q22": ("the NOT EXISTS anti-join (customers with no orders)",
            "AND NOT EXISTS (SELECT * FROM orders WHERE o_custkey = c_custkey)", ""),
}


def _substitute(qid, sql, params):
    """Substitute :NAME placeholders and collapse whitespace.

    Longest name first, so :DATE_PLUS_1Y is not eaten by :DATE. The
    unsubstituted-placeholder guard afterwards is the single most valuable check
    in this file: a template that renders with a leftover ':FOO' produces a
    perfectly plausible parse error that reads like a dialect gap.
    """
    for name in sorted(params, key=len, reverse=True):
        sql = sql.replace(":" + name, params[name])
    collapsed = " ".join(sql.split())
    leftovers = [tok for tok in collapsed.split()
                 if tok.startswith(":") or "(:" in tok or ",:" in tok]
    if leftovers:
        raise ValueError(f"{qid}: unsubstituted parameter(s): {leftovers}")
    return collapsed


def render(qid, params=None):
    """The query as run: template + validation parameters, whitespace collapsed."""
    return _substitute(qid, TEMPLATES[qid],
                       VALIDATION_PARAMS[qid] if params is None else params)


def render_mutant(qid, params=None):
    """(label, sql) for the query with its characteristic predicate neutered.

    Raises if the fragment does not occur exactly once in the collapsed
    template. That is deliberate: a mutation that silently fails to apply makes
    every query look like it discriminates.
    """
    label, old, new = MUTATIONS[qid]
    collapsed = " ".join(TEMPLATES[qid].split())
    n = collapsed.count(old)
    if n != 1:
        raise ValueError(
            f"{qid}: mutation fragment occurs {n} times, expected exactly 1: {old!r}")
    return label, _substitute(qid, collapsed.replace(old, new),
                              VALIDATION_PARAMS[qid] if params is None else params)


if __name__ == "__main__":
    missing = [q for q in QUERY_IDS if q not in TEMPLATES]
    if missing:
        raise SystemExit(f"missing templates: {missing}")
    missing_mut = [q for q in QUERY_IDS if q not in MUTATIONS]
    if missing_mut:
        raise SystemExit(f"missing mutations: {missing_mut}")
    for qid in QUERY_IDS:
        render(qid)          # raises on an unsubstituted parameter
        render_mutant(qid)   # raises on a fragment that does not apply
    print(f"ok: {len(QUERY_IDS)} templates render with their validation parameters, "
          f"and {len(MUTATIONS)} mutations apply")
