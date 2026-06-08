#!/usr/bin/env python3
"""Intentionally monolithic invoice reporter.

The behavior is covered by tests. The refactor target is to split parsing,
calculation, and formatting into small modules while preserving CLI behavior.
"""
import argparse
import csv
from decimal import Decimal, ROUND_HALF_UP
from pathlib import Path


def main(argv=None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("csv_path")
    parser.add_argument("--tax", default="0.20")
    args = parser.parse_args(argv)

    rows = []
    with open(args.csv_path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append(row)

    subtotal = Decimal("0")
    lines = []
    for row in rows:
        qty = Decimal(row["qty"])
        price = Decimal(row["unit_price"])
        total = (qty * price).quantize(Decimal("0.01"), rounding=ROUND_HALF_UP)
        subtotal += total
        lines.append(f"{row['sku']}: {qty} × {price} = {total}")

    tax_rate = Decimal(args.tax)
    tax = (subtotal * tax_rate).quantize(Decimal("0.01"), rounding=ROUND_HALF_UP)
    grand_total = (subtotal + tax).quantize(Decimal("0.01"), rounding=ROUND_HALF_UP)

    print("INVOICE REPORT")
    print("==============")
    for line in lines:
        print(line)
    print(f"Subtotal: {subtotal.quantize(Decimal('0.01'))}")
    print(f"Tax: {tax}")
    print(f"Total: {grand_total}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
