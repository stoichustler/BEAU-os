#!/usr/bin/env python3
"""Extract AI_SCHED_V1 shell records into a versioned CSV dataset."""

import argparse
import csv
import re
from pathlib import Path

from schema import SCHEMA_VERSION

RECORD = re.compile(r"^AI_SCHED_V1 (?P<body>.+)$")


def parse_record(line):
    match = RECORD.match(line.strip())
    if match is None:
        return None
    values = {"schema": SCHEMA_VERSION}
    for field in match.group("body").split():
        if "=" not in field:
            continue
        key, value = field.split("=", 1)
        if key == "active_mask":
            values[key] = int(value, 0)
        else:
            values[key] = int(value, 10)
    required = {"ticks", "pcpu", "period_us", "admission_ppm", "runqueue",
                "active_mask", "pool_budget_us", "cs", "resched"}
    if not required.issubset(values):
        return None
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    rows = []
    previous = {}
    for line in args.input.read_text(encoding="utf-8", errors="replace").splitlines():
        record = parse_record(line)
        if record is None:
            continue
        old = previous.get(record["pcpu"])
        if old is None or record["ticks"] <= old["ticks"]:
            record["cs_delta"] = 0
            record["resched_delta"] = 0
        else:
            record["cs_delta"] = record["cs"] - old["cs"]
            record["resched_delta"] = record["resched"] - old["resched"]
        previous[record["pcpu"]] = record
        rows.append(record)

    fields = ("schema", "ticks", "pcpu", "period_us", "admission_ppm", "runqueue",
              "active_mask", "pool_budget_us", "cs_delta", "resched_delta")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


if __name__ == "__main__":
    main()
