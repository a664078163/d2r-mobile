#!/usr/bin/env python3
"""Find real calls through ACNH's cached NVN procedure table.

ACNH calls every NVN function through three levels of indirection::

    adrp x8, table_page
    ldr  x8, [x8, #slot]   ; global storage address
    ldr  x8, [x8]          ; function pointer written by the bootstrap
    blr  x8

The expanded image is the same byte-for-byte memory image produced by
``nsoimg.load()``. Its first 16 bytes are part of the image at virtual
addresses ``0x0..0xf``; they are not a wrapper header. This helper builds an
explicit virtual-address to file-offset map and tracks ARM64 register values.
It reports a call only when the complete three-level chain resolves to a slot
in the procedure table.

This analysis helper is standalone and is not part of the host build.
"""

import argparse
import struct
import sys
from dataclasses import dataclass

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_MEM, ARM64_OP_REG


DEFAULT_TABLE_BASE = 0x4ACF000
DEFAULT_TABLE_SIZE = 0x10C0
DEFAULT_SLOT_LO = 0x4000000
DEFAULT_SLOT_HI = 0x6000000
DEFAULT_IMAGE_HEADER = 0


@dataclass(frozen=True)
class Segment:
    file_offset: int
    virtual_address: int
    size: int


@dataclass(frozen=True)
class ImageMap:
    segments: tuple[Segment, ...]
    image_header: int

    def file_offset(self, address):
        for segment in self.segments:
            if segment.virtual_address <= address < segment.virtual_address + segment.size:
                return segment.file_offset + address - segment.virtual_address
        return None

    def contains(self, address):
        return self.file_offset(address) is not None


@dataclass
class CallSite:
    slot: int
    storage: int
    load_pc: int
    deref_pc: int
    call_pc: int
    context_start: int
    context_end: int


def adrp_target(pc, word):
    if word & 0x9F000000 != 0x90000000:
        return None
    immhi = (word >> 5) & 0x7FFFF
    immlo = (word >> 29) & 3
    imm = (immhi << 2) | immlo
    if imm & (1 << 20):
        imm -= 1 << 21
    return (pc & ~0xFFF) + (imm << 12)


def reg_name(ins, operand):
    return ins.reg_name(operand.reg) if operand.reg else None


def is_writeback(ins, name):
    """Return whether the instruction overwrites ``name`` as an output."""
    mnemonic = ins.mnemonic
    if mnemonic in {
        "b", "bl", "blr", "br", "cbz", "cbnz", "tbz", "tbnz",
        "cmp", "cmn", "tst", "ccmp", "ccmn", "ret",
        "str", "stp", "stur", "sturb", "sturh", "sttr",
        "stxr", "stlxr", "stllr", "stlr", "prfm", "dmb", "dsb", "isb",
    }:
        return False
    if not ins.operands:
        return False
    first = ins.operands[0]
    return first.type == ARM64_OP_REG and reg_name(ins, first) == name


def add_immediate(ins):
    """Return (destination, source, immediate) for ADD/SUB reg+imm forms."""
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
    if ins.mnemonic not in {"mov", "orr"} or len(ins.operands) != 2:
        return None
    dst, src = ins.operands
    if dst.type != ARM64_OP_REG or src.type != ARM64_OP_REG:
        return None
    return reg_name(ins, dst), reg_name(ins, src)


def load_from(ins):
    """Return (destination register, base register, displacement) for LDR."""
    if not ins.mnemonic.startswith("ldr") or len(ins.operands) < 2:
        return None
    dst, mem = ins.operands[:2]
    if dst.type != ARM64_OP_REG or mem.type != ARM64_OP_MEM:
        return None
    base = ins.reg_name(mem.mem.base) if mem.mem.base else None
    return reg_name(ins, dst), base, int(mem.mem.disp)


def parse_image_map(data, image_header):
    """Return an explicit virtual-address to file-offset map.

    An expanded memory image is byte-identical to ``nsoimg.load()`` and maps
    virtual address N to file offset N. A non-zero ``image_header`` is only
    for an externally wrapped memory image; raw NSO input still follows the
    NSO segment table.
    """
    if len(data) < 0x10:
        raise ValueError("image is shorter than its first virtual-address word")
    if struct.unpack_from("<I", data, 0)[0] == 1:
        if image_header > len(data):
            raise ValueError("image header extends past the end of the image")
        image_size = len(data) - image_header
        return ImageMap((Segment(image_header, 0, image_size),), image_header)

    if len(data) < image_header + 0x60:
        raise ValueError("image is shorter than its metadata header")

    if data[image_header:image_header + 4] != b"NSO0":
        raise ValueError("image is neither a memory image with the expected header nor an NSO")
    segments = []
    for index in range(3):
        file_offset, virtual_address, size = struct.unpack_from(
            "<III", data, image_header + 0x10 + index * 0x10)
        segments.append(Segment(file_offset + image_header, virtual_address, size))
    for previous, current in zip(segments, segments[1:]):
        if previous.virtual_address + previous.size > current.virtual_address:
            raise ValueError("NSO segments overlap")
    return ImageMap(tuple(segments), image_header)


def validate_slot_storage(data, image, base, size, slot_lo, slot_hi):
    """Reject slots whose value is not a pointer into the storage globals.

    The procedure table does not contain function addresses. Every non-zero
    slot must point at a global that holds the address returned by the NVN
    bootstrap. String or code addresses are invalid evidence.
    """
    errors = []
    valid = 0
    for slot in range(base, base + size, 8):
        offset = image.file_offset(slot)
        if offset is None or offset + 8 > len(data):
            errors.append(f"slot {slot:#x} is outside the image")
            continue
        storage = struct.unpack_from("<Q", data, offset)[0]
        if storage == 0:
            continue
        if not (slot_lo <= storage < slot_hi):
            # Both table pages contain non-pointer words: page zero has an
            # ELF-style header value and the second page has unrelated data.
            # They are outside the registered slot range, not evidence.
            continue
        valid += 1
        if image.file_offset(storage) is None:
            errors.append(f"slot {slot:#x} -> {storage:#x}, outside the image")
    if not valid:
        errors.append("no valid procedure-table storage pointers")
    return errors


def scan(data, image, base, size, end, start=0):
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    constants = {}
    storage_regs = {}
    function_regs = {}
    sites = []
    image_end = image.segments[-1].virtual_address + image.segments[-1].size
    last = min(end, image_end)

    # A full-image Capstone scan with detail enabled is prohibitively slow.
    # First scan instruction words directly for ADRP targets in the proc table,
    # then decode only a bounded window around each candidate. Keep exact
    # instruction ordering by resuming at every word inside overlapping windows.
    candidates = set()
    for pc in range(start, last - 3, 4):
        offset = image.file_offset(pc)
        if offset is None or offset + 4 > len(data):
            continue
        target = adrp_target(pc, struct.unpack_from("<I", data, offset)[0])
        if target is not None and base <= target < base + size:
            for q in range(max(start, pc - 0x80), min(last, pc + 0x100), 4):
                candidates.add(q)

    pc = start
    while pc < last - 3:
        offset = image.file_offset(pc)
        if offset is None or offset + 4 > len(data):
            pc += 4
            continue
        word = struct.unpack_from("<I", data, offset)[0]
        if pc not in candidates:
            constants.clear()
            storage_regs.clear()
            function_regs.clear()
            pc += 4
            continue
        # Decode one instruction with detail only at candidate windows.
        window = list(md.disasm(data[offset:offset + 4], pc))
        if not window:
            constants.clear()
            storage_regs.clear()
            function_regs.clear()
            pc += 4
            continue
        ins = window[0]

        assigned = set()
        target = adrp_target(pc, word)
        if target is not None:
            dst = reg_name(ins, ins.operands[0])
            assigned.add(dst)
            storage_regs.pop(dst, None)
            function_regs.pop(dst, None)
            if base <= target < base + size:
                constants[dst] = target
            else:
                constants.pop(dst, None)

        add = add_immediate(ins)
        if add:
            dst, src, value = add
            assigned.add(dst)
            storage_regs.pop(dst, None)
            function_regs.pop(dst, None)
            if src in constants:
                constants[dst] = (constants[src] + value) & ((1 << 64) - 1)
            else:
                constants.pop(dst, None)

        alias = mov_alias(ins)
        if alias:
            dst, src = alias
            assigned.add(dst)
            storage_regs.pop(dst, None)
            function_regs.pop(dst, None)
            if src in constants:
                constants[dst] = constants[src]
            elif src in storage_regs:
                storage_regs[dst] = storage_regs[src]
            elif src in function_regs:
                function_regs[dst] = function_regs[src]
            else:
                constants.pop(dst, None)

        load = load_from(ins)
        if load:
            dst, src, disp = load
            # The first load reads the storage-pointer global selected by the
            # procedure-table slot. The second load with zero displacement
            # dereferences that global and obtains the function pointer. A
            # register can retain its old ADRP constant after the first LDR;
            # the newer storage state must take precedence over it.
            source_storage = storage_regs.get(src) if disp == 0 else None
            assigned.add(dst)
            # Do not discard the ADRP base until this instruction's result has
            # been classified; the two loads commonly reuse the same register.
            storage_regs.pop(dst, None)
            function_regs.pop(dst, None)
            if source_storage:
                slot, storage, load_pc = source_storage
                function_regs[dst] = (slot, storage, load_pc, pc)
            else:
                address = constants.get(src, 0) + disp
                if base <= address < base + size:
                    slot_offset = image.file_offset(address)
                    storage = struct.unpack_from("<Q", data, slot_offset)[0] if slot_offset is not None else 0
                    if storage:
                        storage_regs[dst] = (address, storage, pc)
            constants.pop(dst, None)

        if ins.mnemonic in {"blr", "br"} and ins.operands:
            op = ins.operands[0]
            name = reg_name(ins, op) if op.type == ARM64_OP_REG else None
            if name in function_regs:
                slot, storage, load_pc, deref_pc = function_regs[name]
                sites.append(CallSite(slot, storage, load_pc, deref_pc, pc,
                                      max(0, pc - 0x40), min(last, pc + 0x40)))
                del function_regs[name]
            if ins.mnemonic == "blr":
                # A normal call can clobber every caller-saved register.
                storage_regs.clear()
                function_regs.clear()
                constants.clear()

        for name in list(storage_regs):
            if name not in assigned and is_writeback(ins, name):
                del storage_regs[name]
        for name in list(function_regs):
            if name not in assigned and is_writeback(ins, name):
                del function_regs[name]
        for name in list(constants):
            if name not in assigned and name not in storage_regs and name not in function_regs and is_writeback(ins, name):
                del constants[name]

        if ins.mnemonic in {"b", "ret", "br"}:
            storage_regs.clear()
            function_regs.clear()
            constants.clear()
        pc += 4

    return sites


def load_names(path):
    names = {}
    if not path:
        return names
    # The generated evidence TSV has a human-readable UTF-8 header.  Keep the
    # loader tolerant of that header (and of future metadata-only rows) while
    # still requiring the two fields that define a slot mapping.
    with open(path, encoding="utf-8") as source:
        for line in source:
            if line.startswith("#") or not line.strip():
                continue
            fields = line.rstrip("\n").split("\t")
            if len(fields) < 2 or fields[0] == "name":
                continue
            name, slot = fields[:2]
            # nvn_slots.py emits bare hexadecimal addresses, while extracted
            # evidence TSVs may use 0x-prefixed values.
            base = 0 if slot.lower().startswith("0x") else 16
            names[int(slot, base)] = name
    return names


def load_modules(path):
    modules = {}
    if not path:
        return modules
    with open(path, encoding="utf-8") as source:
        for line in source:
            if line.startswith("#") or not line.strip():
                continue
            fields = line.split("\t")
            if len(fields) >= 2:
                slot = fields[1]
                base = 0 if slot.lower().startswith("0x") else 16
                modules[int(slot, base)] = fields[0]
    return modules


def self_check(image, sites, required):
    errors = []
    by_pc = {(site.deref_pc, site.call_pc): site for site in sites}
    for slot, deref_pc, call_pc in required:
        site = by_pc.get((deref_pc, call_pc))
        if site is None:
            errors.append(f"missing call {deref_pc:#x} -> {call_pc:#x}")
        elif site.slot != slot:
            errors.append(f"call {call_pc:#x} resolves slot {site.slot:#x}, expected {slot:#x}")
    for site in sites:
        if not (DEFAULT_SLOT_LO <= site.storage < DEFAULT_SLOT_HI):
            errors.append(f"call {site.call_pc:#x} has invalid storage {site.storage:#x}")
        if image.file_offset(site.storage) is None:
            errors.append(f"call {site.call_pc:#x} storage {site.storage:#x} is outside the image")
    return errors


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--image-header", type=lambda x: int(x, 0), default=DEFAULT_IMAGE_HEADER)
    ap.add_argument("--base", type=lambda x: int(x, 0), default=DEFAULT_TABLE_BASE)
    ap.add_argument("--size", type=lambda x: int(x, 0), default=DEFAULT_TABLE_SIZE)
    ap.add_argument("--end", type=lambda x: int(x, 0), default=0x3F06DF0)
    ap.add_argument("--start", type=lambda x: int(x, 0), default=0)
    ap.add_argument("--names", help="TSV emitted by extract_acnh_nvn_names.py")
    ap.add_argument("--modules", help="TSV emitted by extract_acnh_nvn_names.py")
    ap.add_argument("--slot-lo", type=lambda x: int(x, 0), default=DEFAULT_SLOT_LO)
    ap.add_argument("--slot-hi", type=lambda x: int(x, 0), default=DEFAULT_SLOT_HI)
    ap.add_argument("--require", action="append", default=[],
                    metavar="SLOT:DEREF_PC:CALL_PC")
    ap.add_argument("--strict", action="store_true")
    args = ap.parse_args()
    with open(args.image, "rb") as f:
        data = f.read()

    image = parse_image_map(data, args.image_header)
    errors = validate_slot_storage(data, image, args.base, args.size, args.slot_lo, args.slot_hi)
    required = []
    for value in args.require:
        slot, deref_pc, call_pc = (int(part, 0) for part in value.split(":"))
        required.append((slot, deref_pc, call_pc))

    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    sites = scan(data, image, args.base, args.size, args.end, args.start)
    names = load_names(args.names)
    modules = load_modules(args.modules)
    errors.extend(self_check(image, sites, required))
    if errors and args.strict:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1

    grouped = {}
    for site in sites:
        grouped.setdefault(site.slot, []).append(site)

    for slot in sorted(grouped):
        label = names.get(slot, "")
        print(f"\n=== table+{slot - args.base:#x} ({slot:#x}) {label}, calls={len(grouped[slot])} ===")
        for number, site in enumerate(grouped[slot], 1):
            print(f"-- call {number}: storage {site.storage:#x}, load {site.load_pc:#x}, "
                  f"deref {site.deref_pc:#x}, call {site.call_pc:#x} --")
            context_offset = image.file_offset(site.context_start)
            context_end = image.file_offset(site.context_end - 4) + 4
            for ins in md.disasm(data[context_offset:context_end], site.context_start):
                marker = ">>" if ins.address in (site.load_pc, site.call_pc) else "  "
                print(f"{marker} {ins.address:08x}: {ins.mnemonic:8} {ins.op_str}")

    print(f"\nreal proc-table call sites: {len(sites)}; slots: {len(grouped)}")
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    main()
