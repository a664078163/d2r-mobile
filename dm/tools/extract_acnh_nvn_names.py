#!/usr/bin/env python3
"""Recover the NVN bootstrap registration table from an expanded ACNH main.

ACNH's main constructor calls nvnBootstrapLoader once per registration and
stores each returned address through a global pointer. Some registrations go
to the main proc table and some to auxiliary globals. Recovering them from
the constructor is stronger evidence than assuming the slot follows names.txt.

The parser follows ARM64 ADRP/ADD/LDR/STR register values inside the
constructor. It intentionally emits a TSV instead of rewriting the shared
names.txt: names.txt is the union used by the host lookup, while this TSV is
the native ACNH table used as a regression oracle.
"""

import argparse
import pathlib
import re
import struct
import sys
from dataclasses import dataclass

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_MEM, ARM64_OP_REG


DEFAULT_TABLE_PAGES = (0x4ACF000, 0x4AD0000)


@dataclass(frozen=True)
class Entry:
    name: str
    slot: int
    name_pointer: int
    store_pc: int


def reg_name(ins, operand):
    return ins.reg_name(operand.reg) if operand.reg else None


def adrp_target(pc, ins):
    if ins.mnemonic != "adrp" or not ins.operands:
        return None
    op = ins.operands[1]
    return int(op.imm) if op.type == ARM64_OP_IMM else None


def add_immediate(ins):
    if ins.mnemonic not in {"add", "sub"} or len(ins.operands) < 3:
        return None
    dst, src, imm = ins.operands[:3]
    if dst.type != ARM64_OP_REG or src.type != ARM64_OP_REG or imm.type != ARM64_OP_IMM:
        return None
    value = int(imm.imm)
    if ins.mnemonic == "sub":
        value = -value
    return reg_name(ins, dst), reg_name(ins, src), value


def mov_alias(ins):
    if ins.mnemonic != "mov" or len(ins.operands) != 2:
        return None
    dst, src = ins.operands
    if dst.type != ARM64_OP_REG or src.type != ARM64_OP_REG:
        return None
    return reg_name(ins, dst), reg_name(ins, src)


def memory_operand(ins):
    if not ins.operands:
        return None
    for op in ins.operands:
        if op.type == ARM64_OP_MEM:
            base = ins.reg_name(op.mem.base) if op.mem.base else None
            return base, int(op.mem.disp)
    return None


def read_c_string(data, address, limit=256):
    if address < 0 or address >= len(data):
        return None
    end = data.find(b"\0", address, min(len(data), address + limit))
    if end < 0:
        return None
    raw = data[address:end]
    try:
        return raw.decode("ascii")
    except UnicodeDecodeError:
        return None


def parse(data, start, end, table_pages):
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    constants = {}
    slots = {}
    entries = []
    unclassified_stores = []

    table_lo = min(table_pages)
    table_hi = max(table_pages) + 0x1000
    # ACNH stores 22 registrations in auxiliary globals outside the main
    # proc-table pages. They are still bootstrap registrations and must be
    # counted; only the main-table subset is used as the name oracle.
    all_slots_lo = 0x4000000
    all_slots_hi = 0x6000000

    for ins in md.disasm(data[start:end], start):
        assigned = set()

        target = adrp_target(ins.address, ins)
        if target is not None:
            dst = reg_name(ins, ins.operands[0])
            assigned.add(dst)
            constants[dst] = target
            slots.pop(dst, None)

        add = add_immediate(ins)
        if add:
            dst, src, value = add
            assigned.add(dst)
            slots.pop(dst, None)
            if src in constants:
                constants[dst] = constants[src] + value
            else:
                constants.pop(dst, None)

        alias = mov_alias(ins)
        if alias:
            dst, src = alias
            assigned.add(dst)
            if src in constants:
                constants[dst] = constants[src]
            else:
                constants.pop(dst, None)
            if src in slots:
                slots[dst] = slots[src]
            else:
                slots.pop(dst, None)

        if ins.mnemonic.startswith("ldr") and len(ins.operands) >= 2:
            dst_op, mem_op = ins.operands[:2]
            if dst_op.type == ARM64_OP_REG and mem_op.type == ARM64_OP_MEM:
                dst = reg_name(ins, dst_op)
                base = ins.reg_name(mem_op.mem.base) if mem_op.mem.base else None
                disp = int(mem_op.mem.disp)
                assigned.add(dst)
                address = constants.get(base, -1) + disp
                if all_slots_lo <= address < all_slots_hi:
                    slots[dst] = address
                else:
                    slots.pop(dst, None)
                constants.pop(dst, None)

        if ins.mnemonic == "str" and len(ins.operands) >= 2:
            src_op, mem_op = ins.operands[:2]
            if src_op.type == ARM64_OP_REG and mem_op.type == ARM64_OP_MEM:
                src = reg_name(ins, src_op)
                base = ins.reg_name(mem_op.mem.base) if mem_op.mem.base else None
                disp = int(mem_op.mem.disp)
                slot = slots.get(base, -1) + disp
                if src == "x0" and slot >= 0:
                    # x1 is the argument to the immediately preceding
                    # bootstrap call. The store completes one registration.
                    # Resolve it at the store rather than accepting the first
                    # matching nvn string seen anywhere in the constructor.
                    name_pointer = constants.get("x1")
                    text = read_c_string(data, name_pointer) if name_pointer is not None else None
                    if text and text.startswith("nvn"):
                        entries.append(Entry(text, slot, name_pointer, ins.address))
                    else:
                        unclassified_stores.append((ins.address, slot, name_pointer, text))
        for name in list(slots):
            if name not in assigned and name in ("x8",):
                # ldr x8, [x8, #slot] both reads and overwrites x8; the slot is
                # retained for the following str x0, [x8].
                pass

    return entries, unclassified_stores


def duplicates(entries, attr):
    seen = {}
    for entry in entries:
        value = getattr(entry, attr)
        seen.setdefault(value, []).append(entry)
    return {key: values for key, values in seen.items() if len(values) > 1}


def write_tsv(path, entries):
    with path.open("w", encoding="ascii") as out:
        out.write("# name\tslot\tname_pointer\tstore_pc\n")
        for entry in entries:
            out.write(f"{entry.name}\t{entry.slot:#x}\t{entry.name_pointer:#x}\t{entry.store_pc:#x}\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=pathlib.Path)
    parser.add_argument("--start", type=lambda x: int(x, 0), default=0x2491D0)
    parser.add_argument("--end", type=lambda x: int(x, 0), default=0x24CC54)
    parser.add_argument("--table-page", action="append", type=lambda x: int(x, 0))
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--expect", type=int)
    parser.add_argument("--strict", action="store_true")
    args = parser.parse_args()

    data = args.image.read_bytes()
    table_pages = tuple(args.table_page or DEFAULT_TABLE_PAGES)
    entries, unknown = parse(data, args.start, args.end, table_pages)

    duplicate_names = duplicates(entries, "name")
    duplicate_slots = duplicates(entries, "slot")
    print(f"registrations: {len(entries)}")
    print(f"unique names: {len({e.name for e in entries})}")
    main_entries = [e for e in entries if min(table_pages) <= e.slot < max(table_pages) + 0x1000]
    print(f"unique slots: {len({e.slot for e in entries})}")
    print(f"main table registrations: {len(main_entries)}")
    print(f"main table unique names: {len({e.name for e in main_entries})}")
    print(f"table range: {min((e.slot for e in entries), default=0):#x}..{max((e.slot for e in entries), default=0):#x}")
    print(f"unclassified stores: {len(unknown)}")

    for name, values in sorted(duplicate_names.items()):
        print(f"duplicate name {name}: " + ", ".join(f"{v.slot:#x}@{v.store_pc:#x}" for v in values))
    for slot, values in sorted(duplicate_slots.items()):
        print(f"duplicate slot {slot:#x}: " + ", ".join(f"{v.name}@{v.store_pc:#x}" for v in values))
    for store_pc, slot, pointer, text in unknown:
        print(f"unclassified store {store_pc:#x}: slot={slot:#x} ptr={pointer} text={text!r}")

    if args.output:
        write_tsv(args.output, entries)

    if args.strict:
        errors = []
        if args.expect is not None and len(entries) != args.expect:
            errors.append(f"registration count {len(entries)} != {args.expect}")
        if len(main_entries) != 512:
            errors.append(f"main table registration count {len(main_entries)} != 512")
        if len({e.name for e in main_entries}) != 511:
            errors.append(f"main table unique name count {len({e.name for e in main_entries})} != 511")
        if duplicate_slots:
            errors.append("duplicate slots remain")
        if unknown:
            errors.append("unclassified stores remain")
        if errors:
            print("STRICT FAILED: " + "; ".join(errors), file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
