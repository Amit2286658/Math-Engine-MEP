#!/usr/bin/env python3
"""
Generates patches/currency_pairs_N.math — the `into` matrix rows for the 42
baseline currencies that are NOT in the hand-maintained patches/currency.math
base file (which carries 13 currencies).

Requires the SYM_MAX_UNITS=128 engine (unit registry bumped from 64): the 42
groups here cannot fit the old 47-owned-slot registry (34 stdlib + 5 base).
On engines with the old registry these files fail to compile and the app's
patch loader skips them gracefully per-file.

Layout contract (keep in sync with MathPatchLoader):
  - one `[module]` per file, `<[currency]` dependency for topological ordering;
  - per source currency: spellings (upper code, lower code, quoted symbol),
    bare constants, then ALL 55 destination rows (`into` overload per row);
  - identity rows use `with_unit(x, y)`; cross rows call the host extern
    `@currency_convert("SRC", "DST", x)` (the extern understands both cases
    and symbol aliases).
"""
import json
import hashlib
from pathlib import Path

PATCHES = Path(__file__).resolve().parent.parent / "patches"

# ---------------------------------------------------------------- currency table
# (code_upper, group_name, symbol_or_None)
# The group name is the lowercase code EXCEPT for TRY (reserved keyword `try`),
# which uses `lira`; its lower-case code is never spelled.
BASE13 = {
    "USD", "EUR", "GBP", "INR", "JPY", "CNY", "AUD",
    "CAD", "CHF", "SGD", "HKD", "KRW", "NZD",
}

# (upper, group, symbol)
TABLE = [
    ("SEK", "sek", None),
    ("NOK", "nok", None),
    ("DKK", "dkk", None),
    ("MXN", "mxn", None),
    ("BRL", "brl", "R$"),
    ("ZAR", "zar", None),
    ("RUB", "rub", "₽"),
    ("TRY", "lira", "₺"),   # `try` is a reserved keyword — group renamed
    ("AED", "aed", None),
    ("SAR", "sar", None),
    ("THB", "thb", "฿"),
    ("MYR", "myr", None),
    ("IDR", "idr", None),
    ("PHP", "php", "₱"),
    ("PLN", "pln", None),
    ("TWD", "twd", None),
    ("VND", "vnd", "₫"),
    ("EGP", "egp", None),
    ("PKR", "pkr", None),
    ("BDT", "bdt", None),
    ("LKR", "lkr", None),
    ("NPR", "npr", None),
    ("NGN", "ngn", "₦"),
    ("KES", "kes", None),
    ("GHS", "ghs", None),
    ("ARS", "ars", None),
    ("CLP", "clp", None),
    ("COP", "cop", None),
    ("PEN", "pen", None),
    ("ILS", "ils", "₪"),
    ("QAR", "qar", None),
    ("KWD", "kwd", None),
    ("BHD", "bhd", None),
    ("OMR", "omr", None),
    ("CZK", "czk", None),
    ("HUF", "huf", None),
    ("RON", "ron", None),
    ("BGN", "bgn", None),
    ("ISK", "isk", None),
    ("UAH", "uah", "₴"),
    ("KZT", "kzt", "₸"),
    ("MAD", "mad", None),
]

# All 55 destinations (base 13 in their base-file display order first).
BASE_ORDER = [
    ("USD", "usd", "$"), ("EUR", "eur", "€"), ("GBP", "gbp", "£"),
    ("INR", "inr", None), ("JPY", "jpy", "¥"), ("CNY", "cny", None),
    ("AUD", "aud", None), ("CAD", "cad", None), ("CHF", "chf", None),
    ("SGD", "sgd", None), ("HKD", "hkd", None), ("KRW", "krw", None),
    ("NZD", "nzd", None),
]
DEST = BASE_ORDER + TABLE  # 55 entries, group names unique

ROWS_PER_FILE = 7


def spellings_block(upper, group, symbol):
    lines = [""]
    lines.append(f"(x)    {upper:<4} :95 -> real, {group} = x")
    if group != "lira":  # lowercase identifier spelling (skip reserved `try`)
        lines.append(f"(x)    {group:<4} :95 -> real, {group} = x")
    if symbol:
        lines.append(f'(x)    "{symbol}":95 -> real, {group} = x')
    lines.append("")
    lines.append(f'"{upper}" : real, {group} = 1')
    if group != "lira":
        lines.append(f'"{group}" : real, {group} = 1')
    if symbol:
        lines.append(f'"{symbol}" : real, {group} = 1')
    return lines


def matrix_block(src_upper, src_group, dest_list):
    lines = [""]
    for du, dgr, dsym in dest_list:
        if du == src_upper:
            lines.append(f'(x: real, {src_group}) "into" (y: real, {src_group}) :>55 = with_unit(x, y)')
        else:
            lines.append(
                f'(x: real, {src_group}) "into" (y: real, {dgr}) :>55 = '
                f'with_unit(@currency_convert("{src_upper}", "{du}", x), y)'
            )
    return lines


def inbound_block(base_order, dst_upper, dst_group):
    """Rows where a base-13 currency is the SOURCE and [dst] the target.
    Lives in the same file as the dst currency's spellings so every dst has a
    single file that owns all conversions arriving at it."""
    lines = [""]
    lines.append(f"# Base-13 -> {dst_upper}")
    for bu, bgr, _bsym in base_order:
        if bu == dst_upper:
            continue
        lines.append(
            f'(x: real, {bgr}) "into" (y: real, {dst_group}) :>55 = '
            f'with_unit(@currency_convert("{bu}", "{dst_upper}", x), y)'
        )
    return lines


def main() -> None:
    assert len(DEST) == 55, "baseline table drifted (want exactly 55 currencies)"
    groups = [g for _, g, _ in DEST]
    assert len(set(groups)) == 55, "currency group names must be unique"

    files = [TABLE[i:i + ROWS_PER_FILE] for i in range(0, len(TABLE), ROWS_PER_FILE)]
    for idx, chunk in enumerate(files, start=1):
        srcs = ", ".join(upper for upper, _, _ in chunk)
        out = []
        out.append("# ==============================================================================")
        out.append(f"# patches/currency_pairs_{idx}.math — currency `into` matrix [{idx}/len(files)]")
        out.append(f"# Covers: {srcs}")
        out.append("# Generated by tools/gen_currency_pairs.py — DO NOT EDIT BY HAND.")
        out.append("# Requires the SYM_MAX_UNITS=128 unit registry (older engines skip this")
        out.append("# file gracefully via the patch loader).")
        out.append("# ==============================================================================")
        out.append("")
        out.append(f"[currency_pairs_{idx}]")
        out.append("<[currency]")
        for upper, group, symbol in chunk:
            out.append(f"# ------------------------------ {upper} ------------------------------")
            out += spellings_block(upper, group, symbol)
            out.append(f"# {upper}: target all 55 baseline currencies")
            out += matrix_block(upper, group, DEST)
            out += inbound_block(BASE_ORDER, upper, group)
            out.append("")
        text = "\n".join(out).rstrip() + "\n"

        path = PATCHES / f"currency_pairs_{idx}.math"
        path.write_text(text, encoding="utf-8")
        size = path.stat().st_size
        assert size < 65000, f"{path.name} too large for the uint16 stage limit: {size}"
        print(f"[pairs] {path.name}: {size} bytes, {len(chunk)} currencies")

    # Manifest entries for new files (the topological manifest itself is rebuilt
    # by tools/generate_patch_manifest.py over the whole patches/ directory).
    print(f"[pairs] generated {len(files)} files; {len(TABLE)} currencies with full 55-row matrices")


if __name__ == "__main__":
    main()
