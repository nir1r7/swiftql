#!/usr/bin/env python3
"""
generate_tpch.py — TPC-H-shaped data at a chosen scale factor, plus the
self-contained catalog.json that describes it.

    python3 python_tools/generate_tpch.py --scale 0.01 --out-dir data/tpch/sf0.01

WHAT THIS IS, AND WHAT IT IS NOT — read before quoting any result produced from
this data.

This is NOT the official `dbgen`. It is not available in this environment, and
vendoring it is out of scope (separate codebase, different licence). The
consequence is precise and it constrains what Week 36 may claim:

  * The SCHEMA, the column order, the referential integrity and the VALUE
    DOMAINS are the spec's. That is what makes the 22 queries non-vacuous —
    see below.
  * The value DISTRIBUTIONS are not the spec's, so the published TPC-H SF1
    ANSWER SET DOES NOT APPLY. The only oracle for this data is SQLite over the
    same files, which is the oracle this project has used since Phase 1.

Every run report must state which generator produced its data. A report that
says "matches reference results" while meaning "matches SQLite on synthetic
data" is the overclaim this file exists to prevent.

WHY THE VALUE DOMAINS ARE THE LOAD-BEARING PART. TPC-H query predicates name
literal values: `r_name = 'ASIA'`, `p_type like '%BRASS'`, `c_mktsegment =
'BUILDING'`, `l_shipmode in ('MAIL','SHIP')`, `p_name like '%green%'`,
`o_comment not like '%special%requests%'`. A generator that emits plausible-
looking random strings makes every one of those predicates match nothing, both
engines return zero rows, and the harness reports a PASS that tested nothing.
Vacuous passes are the specific way a TPC-H harness lies, so the domains below
are transcribed from the spec and the harness asserts non-emptiness separately
(run_tpch.py --require-rows).

Deterministic: --seed defaults to a fixed value and every table is generated
from it, so two runs produce byte-identical files.
"""

import argparse
import json
import os
import random

from tpch_schema import (TPCH_TABLES, TPCH_LOAD_ORDER, TBL_FORMAT,
                         swiftql_columns)

# ---------------------------------------------------------------------------
# Spec value domains. These are what make the 22 queries non-vacuous.
# ---------------------------------------------------------------------------

REGIONS = ["AFRICA", "AMERICA", "ASIA", "EUROPE", "MIDDLE EAST"]

# (name, regionkey) in spec key order.
NATIONS = [
    ("ALGERIA", 0), ("ARGENTINA", 1), ("BRAZIL", 1), ("CANADA", 1),
    ("EGYPT", 4), ("ETHIOPIA", 0), ("FRANCE", 3), ("GERMANY", 3),
    ("INDIA", 2), ("INDONESIA", 2), ("IRAN", 4), ("IRAQ", 4),
    ("JAPAN", 2), ("JORDAN", 4), ("KENYA", 0), ("MOROCCO", 0),
    ("MOZAMBIQUE", 0), ("PERU", 1), ("CHINA", 2), ("ROMANIA", 3),
    ("SAUDI ARABIA", 4), ("VIETNAM", 2), ("RUSSIA", 3),
    ("UNITED KINGDOM", 3), ("UNITED STATES", 1),
]

P_TYPE_S1 = ["STANDARD", "SMALL", "MEDIUM", "LARGE", "ECONOMY", "PROMO"]
P_TYPE_S2 = ["ANODIZED", "BURNISHED", "PLATED", "POLISHED", "BRUSHED"]
P_TYPE_S3 = ["TIN", "NICKEL", "BRASS", "STEEL", "COPPER"]

P_CONT_S1 = ["SM", "LG", "MED", "JUMBO", "WRAP"]
P_CONT_S2 = ["CASE", "BOX", "BAG", "JAR", "PKG", "PACK", "CAN", "DRUM"]

# Q9 greps p_name for '%green%'; the spec's colour list is what supplies it.
COLORS = [
    "almond", "antique", "aquamarine", "azure", "beige", "bisque", "black",
    "blanched", "blue", "blush", "brown", "burlywood", "burnished", "chartreuse",
    "chiffon", "chocolate", "coral", "cornflower", "cornsilk", "cream", "cyan",
    "dark", "deep", "dim", "dodger", "drab", "firebrick", "floral", "forest",
    "frosted", "gainsboro", "ghost", "goldenrod", "green", "grey", "honeydew",
    "hot", "indian", "ivory", "khaki", "lace", "lavender", "lawn", "lemon",
    "light", "lime", "linen", "magenta", "maroon", "medium", "metallic",
    "midnight", "mint", "misty", "moccasin", "navajo", "navy", "olive", "orange",
    "orchid", "pale", "papaya", "peach", "peru", "pink", "plum", "powder",
    "puff", "purple", "red", "rose", "rosy", "royal", "saddle", "salmon",
    "sandy", "seashell", "sienna", "sky", "slate", "smoke", "snow", "spring",
    "steel", "tan", "thistle", "tomato", "turquoise", "violet", "wheat", "white",
    "yellow",
]

MKTSEGMENTS = ["AUTOMOBILE", "BUILDING", "FURNITURE", "MACHINERY", "HOUSEHOLD"]
ORDER_PRIORITIES = ["1-URGENT", "2-HIGH", "3-MEDIUM", "4-NOT SPECIFIED", "5-LOW"]
SHIP_INSTRUCTS = ["DELIVER IN PERSON", "COLLECT COD", "NONE", "TAKE BACK RETURN"]
SHIP_MODES = ["REG AIR", "AIR", "RAIL", "SHIP", "TRUCK", "MAIL", "FOB"]

# Comment vocabulary. 'special' and 'requests' are here because Q13 filters
# `o_comment not like '%special%requests%'` and a corpus without them makes that
# predicate a no-op — the query would still return rows, but it would not be
# testing the thing it exists to test.
COMMENT_WORDS = [
    "special", "requests", "packages", "accounts", "deposits", "pending",
    "unusual", "express", "furiously", "carefully", "slyly", "final", "ironic",
    "regular", "even", "bold", "quickly", "blithely", "silent", "theodolites",
    "instructions", "foxes", "dependencies", "asymptotes", "excuses", "waters",
    "dolphins", "frays", "warthogs", "epitaphs",
]

# Order dates span 1992-01-01 .. 1998-08-02 in the spec; several query
# parameters (Q1's 1998-12-01, Q3's 1995-03-15, Q6's 1994, Q7/Q8's 1995-1996)
# are only meaningful inside that window.
ORDERDATE_START = "1992-01-01"
ORDERDATE_DAYS = 2406           # through 1998-08-02


def _days_to_iso(base_ordinal, n):
    import datetime
    return (datetime.date.fromordinal(base_ordinal + n)).isoformat()


def _comment(rng, min_words=3, max_words=12):
    return " ".join(rng.choice(COMMENT_WORDS)
                    for _ in range(rng.randint(min_words, max_words)))


def _phone(rng, nationkey):
    # Spec form: (10 + nationkey)-NNN-NNN-NNNN. Q22 slices the country code with
    # SUBSTRING(c_phone, 1, 2), so the first two characters must be the code.
    return "{:02d}-{:03d}-{:03d}-{:04d}".format(
        10 + nationkey, rng.randint(100, 999), rng.randint(100, 999),
        rng.randint(1000, 9999))


# ---------------------------------------------------------------------------
# Table generators. Each returns a list of field-value lists in schema order.
# ---------------------------------------------------------------------------

def gen_region(rng):
    return [[i, name, _comment(rng)] for i, name in enumerate(REGIONS)]


def gen_nation(rng):
    return [[i, name, rk, _comment(rng)] for i, (name, rk) in enumerate(NATIONS)]


def gen_part(rng, n):
    rows = []
    for k in range(1, n + 1):
        mfgr = rng.randint(1, 5)
        name = " ".join(rng.sample(COLORS, 5))
        rows.append([
            k,
            name,
            f"Manufacturer#{mfgr}",
            f"Brand#{mfgr}{rng.randint(1, 5)}",
            f"{rng.choice(P_TYPE_S1)} {rng.choice(P_TYPE_S2)} {rng.choice(P_TYPE_S3)}",
            rng.randint(1, 50),
            f"{rng.choice(P_CONT_S1)} {rng.choice(P_CONT_S2)}",
            # Spec formula, so prices span the same ~900..2100 band the queries
            # were parameterised against.
            round(90000 + ((k / 10) % 20001) + 100 * (k % 1000), 2) / 100.0,
            _comment(rng, 2, 8),
        ])
    return rows


def gen_supplier(rng, n):
    rows = []
    for k in range(1, n + 1):
        nk = rng.randint(0, 24)
        rows.append([
            k,
            "Supplier#{:09d}".format(k),
            _comment(rng, 2, 5),                       # address-shaped filler
            nk,
            _phone(rng, nk),
            round(rng.uniform(-999.99, 9999.99), 2),
            _comment(rng, 3, 10),
        ])
    return rows


def gen_partsupp(rng, n_parts, n_suppliers):
    """Four suppliers per part, spread deterministically.

    Q9/Q17/Q20 join lineitem to partsupp on BOTH keys, so a lineitem line must
    reference a pair that exists here. gen_lineitem draws from this table for
    exactly that reason.
    """
    rows = []
    for pk in range(1, n_parts + 1):
        for i in range(4):
            sk = ((pk + i * (n_suppliers // 4 + (pk - 1) // n_suppliers))
                  % n_suppliers) + 1
            rows.append([pk, sk, rng.randint(1, 9999),
                         round(rng.uniform(1.0, 1000.0), 2),
                         _comment(rng, 3, 10)])
    return rows


def gen_customer(rng, n):
    rows = []
    for k in range(1, n + 1):
        nk = rng.randint(0, 24)
        rows.append([
            k,
            "Customer#{:09d}".format(k),
            _comment(rng, 2, 5),
            nk,
            _phone(rng, nk),
            round(rng.uniform(-999.99, 9999.99), 2),
            rng.choice(MKTSEGMENTS),
            _comment(rng, 3, 10),
        ])
    return rows


def gen_orders_and_lineitem(rng, n_orders, customers_n, partsupp_rows, parts):
    import datetime
    base = datetime.date.fromisoformat(ORDERDATE_START).toordinal()
    retailprice = {p[0]: p[7] for p in parts}

    orders, lineitem = [], []
    for ok in range(1, n_orders + 1):
        od_off = rng.randint(0, ORDERDATE_DAYS)
        orderdate = _days_to_iso(base, od_off)
        nlines = rng.randint(1, 7)

        total = 0.0
        for ln in range(1, nlines + 1):
            ps = partsupp_rows[rng.randrange(len(partsupp_rows))]
            pk, sk = ps[0], ps[1]
            qty = float(rng.randint(1, 50))
            price = round(qty * retailprice[pk], 2)
            disc = round(rng.randint(0, 10) / 100.0, 2)
            tax = round(rng.randint(0, 8) / 100.0, 2)
            ship_off = od_off + rng.randint(1, 121)
            commit_off = od_off + rng.randint(30, 90)
            recv_off = ship_off + rng.randint(1, 30)
            # returnflag/linestatus correlate with shipdate in the spec, and Q1
            # groups by exactly this pair — an uncorrelated draw would still
            # produce groups, but not the spec's three.
            shipped = ship_off < ORDERDATE_DAYS - 30
            lineitem.append([
                ok, pk, sk, ln, qty, price, disc, tax,
                rng.choice(["A", "R"]) if shipped else "N",
                "F" if shipped else "O",
                _days_to_iso(base, ship_off),
                _days_to_iso(base, commit_off),
                _days_to_iso(base, recv_off),
                rng.choice(SHIP_INSTRUCTS),
                rng.choice(SHIP_MODES),
                _comment(rng, 3, 10),
            ])
            total += price * (1 - disc) * (1 + tax)

        orders.append([
            ok,
            rng.randint(1, customers_n),
            rng.choice(["F", "O", "P"]),
            round(total, 2),
            orderdate,
            rng.choice(ORDER_PRIORITIES),
            "Clerk#{:09d}".format(rng.randint(1, 1000)),
            0,
            _comment(rng, 3, 12),
        ])
    return orders, lineitem


# ---------------------------------------------------------------------------
# Writers
# ---------------------------------------------------------------------------

def write_tbl(path, rows):
    """One line per row, '|'-TERMINATED — the .tbl convention the Week 35 loader
    learned to read (FileFormat::tbl)."""
    with open(path, "w") as f:
        for r in rows:
            f.write("|".join(_fmt(v) for v in r))
            f.write("|\n")


def _fmt(v):
    if isinstance(v, float):
        # %.2f: every DECIMAL(15,2) column in TPC-H has two places, and writing
        # more would make the file disagree with what the column can represent.
        return f"{v:.2f}"
    return str(v)


def write_catalog(out_dir, tables):
    """The per-scale-factor catalog.

    "file" is RELATIVE on purpose: Catalog::Catalog resolves it against the
    catalog file's own directory, which is what makes a scale-factor directory
    self-contained and selectable with a single --catalog argument from any cwd.
    """
    spec = {"tables": [
        {
            "name": t,
            "file": f"{t}.tbl",
            "format": TBL_FORMAT,
            "columns": [{"name": c, "type": ty} for c, ty in swiftql_columns(t)],
        }
        for t in tables
    ]}
    with open(os.path.join(out_dir, "catalog.json"), "w") as f:
        json.dump(spec, f, indent=2)
        f.write("\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scale", type=float, default=0.01,
                    help="scale factor; nation and region do NOT scale")
    ap.add_argument("--out-dir", default=None,
                    help="default: data/tpch/sf<scale>")
    ap.add_argument("--seed", type=int, default=20250101,
                    help="fixed by default: an unseeded generator makes every "
                         "published number attach to data nobody can recreate")
    args = ap.parse_args()

    out_dir = args.out_dir or f"data/tpch/sf{args.scale:g}"
    os.makedirs(out_dir, exist_ok=True)
    rng = random.Random(args.seed)

    n_part = max(1, int(round(200_000 * args.scale)))
    n_supp = max(1, int(round(10_000 * args.scale)))
    n_cust = max(1, int(round(150_000 * args.scale)))
    n_ord = max(1, int(round(1_500_000 * args.scale)))

    region = gen_region(rng)
    nation = gen_nation(rng)
    part = gen_part(rng, n_part)
    supplier = gen_supplier(rng, n_supp)
    partsupp = gen_partsupp(rng, n_part, n_supp)
    customer = gen_customer(rng, n_cust)
    orders, lineitem = gen_orders_and_lineitem(rng, n_ord, n_cust, partsupp, part)

    tables = {"region": region, "nation": nation, "part": part,
              "supplier": supplier, "partsupp": partsupp,
              "customer": customer, "orders": orders, "lineitem": lineitem}

    for t in TPCH_LOAD_ORDER:
        rows = tables[t]
        # Width check against the one schema definition, here rather than at
        # load time: a generator that emits the wrong arity should fail where
        # the arity is decided.
        want = len(TPCH_TABLES[t])
        for r in rows:
            if len(r) != want:
                raise SystemExit(f"{t}: emitted {len(r)} fields, schema has {want}")
        write_tbl(os.path.join(out_dir, f"{t}.tbl"), rows)

    write_catalog(out_dir, TPCH_LOAD_ORDER)

    # Provenance, written beside the data. Week 36's checkpoint sentence depends
    # on which generator ran, so it must not live only in someone's memory.
    with open(os.path.join(out_dir, "PROVENANCE.txt"), "w") as f:
        f.write(
            "generator: python_tools/generate_tpch.py (NOT the official dbgen)\n"
            f"scale:     {args.scale}\n"
            f"seed:      {args.seed}\n"
            "\n"
            "The schema, column order, referential integrity and value domains\n"
            "are the TPC-H spec's. The value DISTRIBUTIONS are not, so the\n"
            "published TPC-H answer set DOES NOT APPLY to this data. The only\n"
            "valid oracle here is SQLite over these same files.\n")

    print(f"wrote {out_dir} at scale {args.scale} (seed {args.seed})")
    for t in TPCH_LOAD_ORDER:
        print(f"  {t:<10} {len(tables[t]):>9,} rows")


if __name__ == "__main__":
    import sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    main()
