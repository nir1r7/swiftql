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

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--rows", type=int, default=1000)
    args = parser.parse_args()

    drivers = generate_drivers()
    write_csv("data/drivers.csv", drivers,
              ["driver_id", "name", "nationality", "team", "age"])

    laps = generate_laps(args.rows, drivers)
    write_csv("data/laps.csv", laps,
              ["lap_id", "driver_id", "team", "speed", 
               "sector_1", "sector_2", "sector_3", "season", "round"])

    print(f"Generated {args.rows} laps and {len(drivers)} drivers")