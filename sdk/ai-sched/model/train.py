#!/usr/bin/env python3
"""Train and export a bounded decision-tree reward model from reviewed CSV data."""

import argparse
import csv
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "emlearn"))
sys.path.insert(0, str(ROOT))

import emlearn
from sklearn.tree import DecisionTreeRegressor

from schema import FEATURE_COLUMNS, REQUIRED_COLUMNS, validate_columns


def load_rows(path):
    with path.open(newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        validate_columns(reader.fieldnames or ())
        rows = list(reader)
    if len(rows) < 2:
        raise ValueError("dataset needs at least two rows")
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--max-depth", type=int, default=4)
    args = parser.parse_args()
    if args.max_depth < 1 or args.max_depth > 4:
        parser.error("--max-depth must be 1..4")

    rows = load_rows(args.input)
    features = [[float(row[name]) for name in FEATURE_COLUMNS] for row in rows]
    rewards = [float(row["reward"]) for row in rows]
    model = DecisionTreeRegressor(max_depth=args.max_depth, random_state=1)
    model.fit(features, rewards)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    cmodel = emlearn.convert(model, method="inline")
    cmodel.save(file=str(args.output), name="ai_sched_model")
    manifest = {
        "schema": "AI_SCHED_V1",
        "features": FEATURE_COLUMNS,
        "rows": len(rows),
        "max_depth": args.max_depth,
        "dataset_sha256": hashlib.sha256(args.input.read_bytes()).hexdigest(),
        "model": args.output.name,
    }
    args.output.with_suffix(".json").write_text(json.dumps(manifest, indent=2) + "\n",
                                                encoding="utf-8")


if __name__ == "__main__":
    main()
