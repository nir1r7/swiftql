# python_tools/generate_data.py
import csv
import random
import argparse
import os

TEAMS = ["Ferrari", "McLaren", "Mercedes", "RedBull", "Alpine", 
         "AstonMartin", "Williams", "AlphaTauri"]
NATIONALITIES = ["British", "Dutch", "Spanish", "German", "French", "Italian"]

def generate_drivers(num_drivers=20):
    rows = []
    for i in range(1, num_drivers + 1):
        rows.append({
            "driver_id": i,
            "name": f"Driver_{i}",
            "nationality": random.choice(NATIONALITIES),
            "team": random.choice(TEAMS),
            "age": random.randint(20, 40)
        })
    return rows

def generate_laps(num_rows, drivers):
    rows = []
    for i in range(1, num_rows + 1):
        driver = random.choice(drivers)
        rows.append({
            "lap_id": i,
            "driver_id": driver["driver_id"],
            "team": driver["team"],
            "speed": round(random.uniform(280.0, 345.0), 2),
            "sector_1": round(random.uniform(25.0, 40.0), 2),
            "sector_2": round(random.uniform(30.0, 45.0), 2),
            "sector_3": round(random.uniform(20.0, 35.0), 2),
            "season": random.choice([2022, 2023, 2024, 2025]),
            "round": random.randint(1, 24)
        })
    return rows

def write_csv(filepath, rows, fieldnames):
    os.makedirs(os.path.dirname(filepath), exist_ok=True)
    with open(filepath, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)

LAPS_COLUMNS = [("lap_id", "INT"), ("driver_id", "INT"), ("team", "STRING"),
                ("speed", "DOUBLE"), ("sector_1", "DOUBLE"),
                ("sector_2", "DOUBLE"), ("sector_3", "DOUBLE"),
                ("season", "INT"), ("round", "INT")]
DRIVERS_COLUMNS = [("driver_id", "INT"), ("name", "STRING"),
                   ("nationality", "STRING"), ("team", "STRING"),
                   ("age", "INT")]


def write_catalog(out_dir):
    """Write a catalog.json beside the CSVs, making the directory self-contained.

    Week 35: `file` is RELATIVE on purpose — Catalog::Catalog resolves it against
    the catalog file's own directory, so a dataset directory can be selected with
    one --catalog argument from any working directory. That existing property is
    the whole scale-factor mechanism; a small fixture becomes a first-class
    artifact rather than "a second catalog to keep in sync" (Week 28's note).

    No "format" object: these are CSVs, and an absent format IS the CSV default.
    """
    import json
    spec = {"tables": [
        {"name": "laps", "file": "laps.csv",
         "columns": [{"name": n, "type": t} for n, t in LAPS_COLUMNS]},
        {"name": "drivers", "file": "drivers.csv",
         "columns": [{"name": n, "type": t} for n, t in DRIVERS_COLUMNS]},
    ]}
    with open(os.path.join(out_dir, "catalog.json"), "w") as f:
        json.dump(spec, f, indent=2)
        f.write("\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--rows", type=int, default=1000)
    parser.add_argument("--no-sort", action="store_true",
                        help="keep random season order (benchmark docs assume "
                             "season-sorted data so zone-map pruning can skip chunks)")
    # Week 35: `random` was never seeded, so two runs of this script produced
    # DIFFERENT data and every benchmark number in the README attached to a
    # dataset nobody could recreate. "Reproducible" is this week's checkpoint
    # word. Note the COMMITTED data/laps.csv and data/drivers.csv predate the
    # seed and are therefore not reproducible from it — they are left alone
    # deliberately, since the published benchmarks were measured on them.
    parser.add_argument("--seed", type=int, default=20250101)
    parser.add_argument("--out-dir", default="data",
                        help="dataset directory; a catalog.json is written "
                             "beside the CSVs so the directory is self-contained")
    parser.add_argument("--no-catalog", action="store_true",
                        help="skip the catalog (used when writing into data/, "
                             "whose catalog.json lives at the repo root)")
    args = parser.parse_args()

    random.seed(args.seed)

    drivers = generate_drivers()
    write_csv(os.path.join(args.out_dir, "drivers.csv"), drivers,
              ["driver_id", "name", "nationality", "team", "age"])

    laps = generate_laps(args.rows, drivers)
    if not args.no_sort:
        # season-sorted by default: every benchmark doc measures against
        # clustered seasons (zone maps can prove whole chunks irrelevant)
        laps.sort(key=lambda r: r["season"])
    write_csv(os.path.join(args.out_dir, "laps.csv"), laps,
              ["lap_id", "driver_id", "team", "speed",
               "sector_1", "sector_2", "sector_3", "season", "round"])

    if not args.no_catalog and os.path.abspath(args.out_dir) != os.path.abspath("data"):
        write_catalog(args.out_dir)

    order = "random season order" if args.no_sort else "sorted by season"
    print(f"Generated {args.rows} laps ({order}) and {len(drivers)} drivers "
          f"in {args.out_dir} (seed {args.seed})")