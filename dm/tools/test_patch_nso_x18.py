#!/usr/bin/env python3
"""Focused semantic regression for x18 rewrite write-back.

This test loads only the relevant function definitions from patch_nso.py.  The
production script performs the whole rewrite at import time, so importing it
directly would require a real NSO and would make this focused check slow.
"""

from __future__ import annotations

import ast
import re
import struct
import unittest
from itertools import combinations
from pathlib import Path

from capstone import (
    CS_AC_WRITE,
    CS_ARCH_ARM64,
    CS_MODE_ARM,
    CS_OP_MEM,
    CS_OP_REG,
    Cs,
)


ROOT = Path(__file__).resolve().parents[2]
PATCH_NSO = ROOT / "dm/tools/patch_nso.py"

DEFINITIONS = {
    "B",
    "LDP",
    "LDP_POST",
    "LDRX",
    "LDUR",
    "MRS_TPIDRRO",
    "NOP",
    "STP",
    "STP_PRE",
    "STRX",
    "STUR",
    "ADDI",
    "ADDR",
    "ADRP",
    "BLR",
    "BR",
    "SUBI",
    "dis",
    "emit",
    "emit_key_addr",
    "emit_slot_addr",
    "has18",
    "here",
    "keep_native_x18",
    "num",
    "pick",
    "regset",
    "regs_in",
    "site_plain",
    "site_window",
    "sp_delta",
    "subst",
    "writes18",
}

# Encodings are produced by LLVM 22's AArch64 assembler and decoded again with
# Capstone before use, so a typo cannot silently create a different instruction.
INSTRUCTIONS = {
    "add x0, x18, x15": 0x8B0F0240,
    "adds w18, w18, w16": 0x2B100252,
    "cmp x17, x18": 0xEB12023F,
    "cmp x18, x17": 0xEB11025F,
    "cmn x17, x18": 0xAB12023F,
    "cmn x18, x17": 0xAB11025F,
    "tst x17, x18": 0xEA12023F,
    "tst x18, x17": 0xEA11025F,
    "cmp w17, w18": 0x6B12023F,
    "cmp w18, #1": 0x7100065F,
    "cmn w18, #1": 0x3100065F,
    "tst w18, #3": 0x7200065F,
    "ccmp w18, w11, #0, eq": 0x7A4B0240,
    "ccmp w17, w18, #4, ne": 0x7A521224,
    "ldr x18, [x8, #0x18]": 0xF9400D12,
    "ldr w18, [x8, #0x18]": 0xB9401912,
    "str x18, [x0]": 0xF9000012,
    "str x18, [x0, #8]!": 0xF8008C12,
    "str x18, [x0], #8": 0xF8008412,
    "stp x18, x19, [sp]": 0xA9004FF2,
    "ldp x18, x19, [sp]": 0xA9404FF2,
    "mov x18, x15": 0xAA0F03F2,
    "csel x18, x15, x16, eq": 0x9A9001F2,
    "cinc w18, w15, ne": 0x1A8F05F2,
    "extr x18, x15, x16, #3": 0x93D00DF2,
    "stxr w15, x18, [x0]": 0xC80F7C12,
    "stlxr w15, x18, [x0]": 0xC80FFC12,
    "stxr w18, x17, [x16]": 0xC8127E11,
    "stlxr w18, x17, [x15]": 0xC812FDF1,
    "ldaxr x18, [x12]": 0xC85FFD92,
    "ldxr x18, [x17]": 0xC85F7E32,
    "cas x17, x18, [x0]": 0xC8B17C12,
    "cas x18, x17, [x0]": 0xC8B27C11,
    "swp x18, x17, [x0]": 0xF8328011,
    "swp x17, x18, [x0]": 0xF8318012,
    "sub x18, x23, x18": 0xCB1202F2,
}


def _load_rewriter_helpers() -> dict:
    tree = ast.parse(PATCH_NSO.read_text(encoding="utf-8"), filename=str(PATCH_NSO))
    selected = [
        node
        for node in tree.body
        if isinstance(node, ast.FunctionDef) and node.name in DEFINITIONS
    ]
    found = {node.name for node in selected}
    missing = DEFINITIONS - found
    if missing - {"NOP"}:
        raise AssertionError(f"patch_nso.py no longer defines: {sorted(missing)}")

    module = ast.Module(body=selected, type_ignores=[])
    ast.fix_missing_locations(module)
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    namespace = {"ACNH_NATIVE_X18": set()}
    namespace.update({
        "Cs": Cs,
        "CS_ARCH_ARM64": CS_ARCH_ARM64,
        "CS_MODE_ARM": CS_MODE_ARM,
        "combinations": combinations,
        "md": md,
        "patch": bytearray(),
        "fix": [],
        "PATCH_OFF": 0x100000,
        "KEY_PAGE_OFF": 0x200000,
        "KEY_INDEX": {
            "K18": 0,
            "KTLS": 1,
            "KTLR": 2,
            "KGATE": 3,
            "KRES0": 4,
            "KRES1": 5,
            "KASPECT": 6,
            "KASPECTUI": 7,
        },
        "re": re,
        "struct": struct,
        "NOP": 0xD503201F,
    })
    exec(compile(module, str(PATCH_NSO), "exec"), namespace)
    return namespace


class RewriterHarness:
    def __init__(self) -> None:
        self.ns = _load_rewriter_helpers()

    def insn(self, text: str):
        word = INSTRUCTIONS[text]
        decoded = list(
            self.ns["md"].disasm(struct.pack("<I", word), 0x4000)
        )
        if len(decoded) != 1:
            raise AssertionError(f"failed to decode {text}: {word:#x}")
        ins = decoded[0]
        actual = f"{ins.mnemonic} {ins.op_str}"
        expected = (
            text.replace("#0x18", "#0x18")
            .replace(", ", ", ")
            .strip()
        )
        if actual != expected:
            raise AssertionError(f"encoding mismatch: {text!r} decoded as {actual!r}")
        return ins

    def reset_patch(self) -> None:
        self.ns["patch"].clear()
        self.ns["fix"].clear()

    def keep_native_x18(self, module: str, build_id: str, offset: int) -> bool:
        return self.ns["keep_native_x18"](module, build_id, offset)

    def decode_patch(self):
        raw = bytes(self.ns["patch"])
        decoded = list(self.ns["md"].disasm(raw, self.ns["PATCH_OFF"]))
        if sum(ins.size for ins in decoded) != len(raw):
            raise AssertionError("patch contains an undecodable word")
        return decoded

    def run_plain(self, text: str):
        self.reset_patch()
        decoded = self.ns["site_plain"](self.insn(text))
        return decoded, self.decode_patch()

    def run_window(self, texts: tuple[str, ...]):
        self.reset_patch()
        segment = []
        for index, text in enumerate(texts):
            ins = self.insn(text)
            segment.append(
                next(
                    self.ns["md"].disasm(
                        ins.bytes, 0x4000 + 4 * index
                    )
                )
            )
        decoded = self.ns["site_window"](segment)
        segment_end = 0x4000 + 4 * len(segment)
        return decoded, segment_end, self.decode_patch()

    @staticmethod
    def scratch_registers(decoded) -> tuple[int, int]:
        first = decoded[0]
        if first.mnemonic != "stp":
            raise AssertionError(f"unexpected trampoline prologue: {first.mnemonic}")
        return (
            first.reg_name(first.operands[0].reg),
            first.reg_name(first.operands[1].reg),
        )

    @classmethod
    def k18_stores(cls, decoded):
        scratch, slot = cls.scratch_registers(decoded)
        scratch_number = int(scratch.removeprefix("x"))
        slot_number = int(slot.removeprefix("x"))
        stores = []
        for ins in decoded:
            if ins.mnemonic != "str" or len(ins.operands) != 2:
                continue
            dst, mem = ins.operands
            if (
                dst.type == CS_OP_REG
                and decoded[0].reg_name(dst.reg) == scratch
                and mem.type == CS_OP_MEM
                and decoded[0].reg_name(mem.mem.base) == slot
                and mem.mem.index == 0
            ):
                stores.append(ins)
        return stores


class CapstoneEvidenceTests(unittest.TestCase):
    def test_compare_and_test_destinations_are_misreported_as_written(self) -> None:
        harness = RewriterHarness()
        for text in (
            "cmp x17, x18",
            "cmp x18, x17",
            "cmn x17, x18",
            "cmn x18, x17",
            "tst x17, x18",
            "tst x18, x17",
        ):
            with self.subTest(text=text):
                ins = harness.insn(text)
                _, writes = ins.regs_access()
                write_names = {ins.reg_name(reg) for reg in writes}
                compare_destination = ins.operands[0]

                # Evidence: both the summary access and the operand access claim
                # that CMP's first source is written.  A fix must exclude compare
                # instructions explicitly rather than trusting either report.
                self.assertTrue(compare_destination.access & CS_AC_WRITE)
                self.assertTrue(
                    {ins.reg_name(compare_destination.reg), "nzcv"} <= write_names
                )


class Writes18ContractTests(unittest.TestCase):
    def test_only_real_x18_destinations_are_writes(self) -> None:
        harness = RewriterHarness()
        expected = {
            "add x0, x18, x15": False,
            "adds w18, w18, w16": True,
            "cmp x17, x18": False,
            "cmp x18, x17": False,
            "cmn x17, x18": False,
            "cmn x18, x17": False,
            "tst x17, x18": False,
            "tst x18, x17": False,
            "cmp w17, w18": False,
            "cmp w18, #1": False,
            "cmn w18, #1": False,
            "tst w18, #3": False,
            "ccmp w18, w11, #0, eq": False,
            "ccmp w17, w18, #4, ne": False,
            "ldr x18, [x8, #0x18]": True,
            "ldr w18, [x8, #0x18]": True,
            "str x18, [x0]": False,
            "str x18, [x0, #8]!": False,
            "str x18, [x0], #8": False,
            "stp x18, x19, [sp]": False,
            "ldp x18, x19, [sp]": True,
            "mov x18, x15": True,
            "csel x18, x15, x16, eq": True,
            "cinc w18, w15, ne": True,
            "extr x18, x15, x16, #3": True,
            "stxr w15, x18, [x0]": False,
            "stlxr w15, x18, [x0]": False,
            "stxr w18, x17, [x16]": True,
            "stlxr w18, x17, [x15]": True,
            "ldaxr x18, [x12]": True,
            "ldxr x18, [x17]": True,
            "sub x18, x23, x18": True,
        }
        for text, want in expected.items():
            with self.subTest(text=text):
                self.assertEqual(harness.ns["writes18"](harness.insn(text)), want)


class NativeX18ExceptionTests(unittest.TestCase):
    def test_acnh_allocator_chain_has_no_native_x18_gap(self) -> None:
        """The allocator's contiguous x18 dataflow must stay entirely in K18.

        A native exception for only 0x37c7c40 writes Apple's reserved x18,
        while the next instruction still reads the guest K18 slot.  Testing
        each instruction in isolation misses that broken edge.
        """
        harness = RewriterHarness()
        chain = (
            (0x37C7C34, "add x18, x8, x17"),
            (0x37C7C38, "add x18, x18, #0x20"),
            (0x37C7C3C, "add x0, x18, x15"),
            (0x37C7C40, "sub x18, x23, x18"),
            (0x37C7C48, "add x18, x18, x0"),
            (0x37C7C4C, "cmp x14, x18"),
        )
        for offset, text in chain:
            with self.subTest(offset=hex(offset), instruction=text):
                self.assertFalse(
                    harness.keep_native_x18(
                        "main",
                        "ff1d1c05670db6021c85b624a710b963",
                        offset,
                    )
                )

        # 0x37c7c40 is the critical producer for 0x37c7c48.  It must be
        # rewritten and write its result back to K18.
        _, decoded = harness.run_plain("sub x18, x23, x18")
        self.assertTrue(harness.k18_stores(decoded))


class SitePlainContractTests(unittest.TestCase):
    def test_plain_rewrite_only_writes_k18_for_real_x18_destinations(self) -> None:
        harness = RewriterHarness()
        expected = {
            "add x0, x18, x15": False,
            "str x18, [x0]": False,
            "cmp x17, x18": False,
            "cmp x18, x17": False,
            "cmn x17, x18": False,
            "cmn x18, x17": False,
            "tst x17, x18": False,
            "tst x18, x17": False,
            "adds w18, w18, w16": True,
            "ldr x18, [x8, #0x18]": True,
            "ldr w18, [x8, #0x18]": True,
            "str x18, [x0, #8]!": False,
            "str x18, [x0], #8": False,
            "stxr w15, x18, [x0]": False,
            "stlxr w15, x18, [x0]": False,
        }
        for text, want in expected.items():
            with self.subTest(text=text):
                _, decoded = harness.run_plain(text)
                self.assertEqual(bool(harness.k18_stores(decoded)), want)

    def test_plain_substitution_preserves_register_roles(self) -> None:
        harness = RewriterHarness()
        for text in (
            "add x0, x18, x15",
            "str x18, [x0]",
            "cmp x17, x18",
            "cmp x18, x17",
            "cmn x17, x18",
            "cmn x18, x17",
            "tst x17, x18",
            "tst x18, x17",
            "cmp w17, w18",
            "cmp w18, #1",
            "cmn w18, #1",
            "tst w18, #3",
            "ccmp w18, w11, #0, eq",
            "ccmp w17, w18, #4, ne",
            "ldr w18, [x8, #0x18]",
            "str x18, [x0, #8]!",
            "str x18, [x0], #8",
            "stxr w15, x18, [x0]",
            "stlxr w15, x18, [x0]",
        ):
            with self.subTest(text=text):
                start, decoded = harness.run_plain(text)
                scratch_name, _ = harness.scratch_registers(decoded)
                scratch = int(scratch_name.removeprefix("x"))
                original = harness.insn(text)
                want = re.sub(
                    r"\b([xw])18\b", lambda match: f"{match.group(1)}{scratch}", text
                )
                self.assertEqual(
                    bool(
                        any(
                            f"{ins.mnemonic} {ins.op_str}" == want for ins in decoded
                        )
                    ),
                    True,
                    f"missing substituted instruction {want!r}",
                )
                self.assertEqual(
                    f"{original.mnemonic} {original.op_str}",
                    text,
                )
                self.assertEqual(
                    harness.ns["PATCH_OFF"],
                    start,
                )
                self.assertTrue(
                    any(
                        ins.address == start
                        for ins in decoded
                    )
                )


class SiteWindowContractTests(unittest.TestCase):
    def test_window_substitution_preserves_register_roles(self) -> None:
        harness = RewriterHarness()
        for text in (
            "add x0, x18, x15",
            "str x18, [x0]",
            "cmp x17, x18",
            "cmp x18, x17",
            "cmn x17, x18",
            "cmn x18, x17",
            "tst x17, x18",
            "tst x18, x17",
            "adds w18, w18, w16",
            "ldr x18, [x8, #0x18]",
        ):
            with self.subTest(text=text):
                _, _, decoded = harness.run_window((text,))
                scratch_name, _ = harness.scratch_registers(decoded)
                scratch = int(scratch_name.removeprefix("x"))
                want = re.sub(
                    r"\b([xw])18\b",
                    lambda match: f"{match.group(1)}{scratch}",
                    text,
                )
                actual = [
                    f"{ins.mnemonic} {ins.op_str}"
                    for ins in decoded
                    if ins.address < harness.ns["PATCH_OFF"] + 0x20
                ]
                self.assertIn(want, actual)

    def test_window_rewrite_only_writes_k18_for_real_x18_destinations(self) -> None:
        harness = RewriterHarness()
        cases = {
            (
                "add x0, x18, x15",
                "str x18, [x0]",
            ): False,
            (
                "cmp x17, x18",
                "str x18, [x0]",
            ): False,
            (
                "cmp x18, x17",
                "str x18, [x0]",
            ): False,
            (
                "adds w18, w18, w16",
                "str x18, [x0]",
            ): True,
            (
                "ldr x18, [x8, #0x18]",
                "str x18, [x0]",
            ): True,
            (
                "ldr w18, [x8, #0x18]",
                "str x18, [x0]",
            ): True,
            (
                "stxr w15, x18, [x0]",
                "str x18, [x0]",
            ): False,
            (
                "str x18, [x0]",
                "add x0, x18, x15",
            ): False,
        }
        for texts, want in cases.items():
            with self.subTest(instructions=texts):
                (start, lo, hi), segment_end, decoded = harness.run_window(texts)
                self.assertEqual(bool(harness.k18_stores(decoded)), want)
                self.assertEqual(harness.ns["PATCH_OFF"], start)
                self.assertEqual(0x4000, lo)
                self.assertEqual(segment_end, hi)


if __name__ == "__main__":
    unittest.main(verbosity=2)
