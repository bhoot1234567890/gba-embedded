#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path


def load_rows(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            raise ValueError(f"{path} does not contain a CSV header")
        return list(reader)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare emulator CPU traces and report first mismatch."
    )
    parser.add_argument("--expected", required=True, type=Path, help="reference trace CSV")
    parser.add_argument("--actual", required=True, type=Path, help="trace CSV from this emulator")
    parser.add_argument(
        "--ignore-field",
        action="append",
        default=[],
        help="field name to ignore (repeatable)",
    )
    args = parser.parse_args()

    expected_rows = load_rows(args.expected)
    actual_rows = load_rows(args.actual)
    ignored = set(args.ignore_field)

    if not expected_rows:
        print("Expected trace is empty", file=sys.stderr)
        return 1
    if not actual_rows:
        print("Actual trace is empty", file=sys.stderr)
        return 1

    fields = [name for name in expected_rows[0].keys() if name not in ignored]
    if not fields:
        print("No comparable fields left after ignore list", file=sys.stderr)
        return 1

    if len(expected_rows) != len(actual_rows):
        print(
            f"Row count mismatch: expected={len(expected_rows)} actual={len(actual_rows)}",
            file=sys.stderr,
        )
        return 1

    for row_index, (expected, actual) in enumerate(zip(expected_rows, actual_rows)):
        for field in fields:
            actual_value = actual.get(field)
            expected_value = expected.get(field)
            if actual_value != expected_value:
                print(
                    f"Mismatch at row {row_index}, field '{field}': "
                    f"expected={expected_value} actual={actual_value}",
                    file=sys.stderr,
                )
                return 1

    print(f"Traces match for {len(actual_rows)} rows across {len(fields)} fields.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
