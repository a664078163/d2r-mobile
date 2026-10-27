#!/usr/bin/env python3
"""Gate ACNH's real NVN call sites against the host implementation table.

The ACNH main program calls NVN through a cached three-level procedure table.
``scan_acnh_nvn_calls.py`` resolves those calls from the actual binary.  This
gate compares the resolved names with the ``NVN``/``NVN_NOPROTO`` registrations
and the explicitly documented ``g_nops`` list in ``hle_nvn.c``.

It deliberately does not treat a name merely present in ``names.txt`` or in a
prototype header as implemented.  A called name is covered only when the host
source registers an implementation or an explicit safe no-op.  This catches a
new ACNH binary call site before a real device can silently enter
``nvn_unimpl_entry``.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from collections import Counter

from scan_acnh_nvn_calls import (
    DEFAULT_TABLE_BASE,
    DEFAULT_TABLE_SIZE,
    DEFAULT_SLOT_HI,
    DEFAULT_SLOT_LO,
    parse_image_map,
    scan,
    load_names,
)


IMPLEMENTATION_RE = re.compile(r"\bNVN(?:_NOPROTO)?\(\s*(nvn[A-Za-z0-9_]+)")
NOP_BLOCK_RE = re.compile(r"g_nops\[\][^{]*\{(?P<body>.*?)\};", re.DOTALL)
NOP_NAME_RE = re.compile(r'"(nvn[A-Za-z0-9_]+)"')


def source_coverage(path: pathlib.Path) -> tuple[set[str], set[str], list[str]]:
    text = path.read_text(encoding="utf-8")
    implementation_matches = IMPLEMENTATION_RE.findall(text)
    implementations = set(implementation_matches)
    block = NOP_BLOCK_RE.search(text)
    if not block:
        raise ValueError(f"cannot find g_nops in {path}")
    noops = set(NOP_NAME_RE.findall(block.group("body")))
    # A registration name also appears in the generated prototype check and in
    # the function body.  Count only macro registrations; otherwise every
    # valid implementation is falsely reported as duplicated.
    implementation_counts = Counter(implementation_matches)
    duplicates = sorted(name for name, count in implementation_counts.items() if count > 1)
    return implementations, noops, duplicates


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=pathlib.Path)
    parser.add_argument("--names", required=True, type=pathlib.Path)
    parser.add_argument(
        "--source",
        type=pathlib.Path,
        default=pathlib.Path("dm/gfx/nvn/hle_nvn.c"),
    )
    parser.add_argument("--image-header", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--base", type=lambda value: int(value, 0), default=DEFAULT_TABLE_BASE)
    parser.add_argument("--size", type=lambda value: int(value, 0), default=DEFAULT_TABLE_SIZE)
    parser.add_argument("--start", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--end", type=lambda value: int(value, 0), default=0x3F06DF0)
    parser.add_argument("--slot-lo", type=lambda value: int(value, 0), default=DEFAULT_SLOT_LO)
    parser.add_argument("--slot-hi", type=lambda value: int(value, 0), default=DEFAULT_SLOT_HI)
    args = parser.parse_args()

    data = args.image.read_bytes()
    image = parse_image_map(data, args.image_header)
    sites = scan(data, image, args.base, args.size, args.end, args.start)
    names_by_slot = load_names(str(args.names))
    implementations, noops, duplicate_source_names = source_coverage(args.source)
    covered = implementations | noops

    calls_by_name: Counter[str] = Counter()
    unknown_slots: Counter[int] = Counter()
    for site in sites:
        name = names_by_slot.get(site.slot)
        if name is None:
            unknown_slots[site.slot] += 1
        else:
            calls_by_name[name] += 1

    missing = sorted(name for name in calls_by_name if name not in covered)
    errors: list[str] = []
    if unknown_slots:
        errors.append(
            "real call sites with no recovered name: "
            + ", ".join(f"{slot:#x} ({count})" for slot, count in sorted(unknown_slots.items()))
        )
    if duplicate_source_names:
        errors.append("duplicate NVN implementation registrations: " + ", ".join(duplicate_source_names))
    if missing:
        errors.append("real ACNH calls still resolve to nvn_unimpl_entry: " + ", ".join(missing))

    print(f"ACNH NVN call sites: {len(sites)}")
    print(f"ACNH NVN called slots: {len(calls_by_name)}")
    print(f"host registrations: {len(implementations)}")
    print(f"explicit safe no-ops: {len(noops)}")
    print(f"covered called slots: {len(calls_by_name) - len(missing)}")
    print(f"uncovered called slots: {len(missing)}")
    for name in missing:
        print(f"MISSING {name} calls={calls_by_name[name]}")

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print("ACNH-NVN-COVERAGE OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
