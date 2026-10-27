#!/usr/bin/env python3
"""Focused contract tests for the ACNH container reader."""

from __future__ import annotations

import hashlib
import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "dm/tools/acnh_shader.py"
spec = importlib.util.spec_from_file_location("acnh_shader", MODULE)
acnh = importlib.util.module_from_spec(spec)
assert spec.loader
sys.modules[spec.name] = acnh
spec.loader.exec_module(acnh)


def control(stage: int, c1: int, total: int, identity: int = 0) -> bytes:
    value = bytearray(0x880)
    struct.pack_into("<I", value, 0x20, len(value))
    struct.pack_into("<I", value, 0x714, stage)
    struct.pack_into("<II", value, 0x700, c1, total)
    value[0x600:0x608] = identity.to_bytes(8, "little")
    return bytes(value)


def yaz0_literal(payload: bytes) -> bytes:
    out = bytearray(b"Yaz0" + struct.pack(">I", len(payload)) + bytes(8))
    for at in range(0, len(payload), 8):
        chunk = payload[at:at + 8]
        out.append(0xFF)
        out.extend(chunk)
    return bytes(out)


class Yaz0Tests(unittest.TestCase):
    def test_literal_and_padding(self):
        payload = b"animal-crossing"
        encoded = yaz0_literal(payload) + b"\0\0\0"
        self.assertEqual(acnh.yaz0(encoded), payload)

    def test_overlapping_back_reference(self):
        # One literal 'A', then two copies: distance 1, then distance 2.
        stream = bytearray(b"Yaz0" + struct.pack(">I", 10) + bytes(8))
        stream.extend((0x80, ord("A"), 0xF0, 0x00))
        self.assertEqual(acnh.yaz0(bytes(stream)), b"AAAAAAAAAA")

    def test_truncated_stream(self):
        with self.assertRaises(acnh.FormatError):
            acnh.yaz0(b"Yaz0" + struct.pack(">I", 10) + bytes(8) + b"\x80")


def sarc_member(name: str, payload: bytes) -> bytes:
    header_size = 0x20
    node = header_size
    names = node + 12 + 16
    strings = names + 8
    name_at = strings
    data_at = (strings + len(name) + 1 + 15) & ~15
    total = data_at + len(payload)
    out = bytearray(total)
    out[:4] = b"SARC"
    struct.pack_into("<H", out, 4, header_size)
    out[6:8] = b"\xff\xfe"
    struct.pack_into("<I", out, 8, total)
    struct.pack_into("<I", out, 12, data_at)
    out[node:node + 4] = b"SFAT"
    struct.pack_into("<H", out, node + 4, 0x40)
    struct.pack_into("<H", out, node + 6, 1)
    struct.pack_into("<4I", out, node + 12, 0, 0x01000000, 0, len(payload))
    out[names:names + 4] = b"SFNT"
    struct.pack_into("<I", out, names + 4, 0x01000000)
    out[name_at:name_at + len(name)] = name.encode()
    out[data_at:data_at + len(payload)] = payload
    return bytes(out)

class SarcTests(unittest.TestCase):
    def test_member_name_and_data_bounds(self):
        archive = sarc_member("Shader/test.bin", b"abc")
        members = acnh.sarc(archive)
        self.assertEqual([(m.name, m.data) for m in members], [("Shader/test.bin", b"abc")])

    def test_truncated_declared_size(self):
        archive = bytearray(sarc_member("x", b"abc"))
        struct.pack_into("<I", archive, 8, len(archive) + 1)
        with self.assertRaises(acnh.FormatError):
            acnh.sarc(bytes(archive))

    def test_parent_path_rejected(self):
        archive = sarc_member("../escape", b"abc")
        with self.assertRaises(acnh.FormatError):
            acnh.sarc(archive)


def bahs(records: list[tuple[int, int]], code: bytes, controls: list[bytes],
         *, record_offsets: list[int] | None = None) -> bytes:
    start = 0x100
    record_offsets = record_offsets or [index * 0x80 for index in range(len(records))]
    table_size = max(record_offsets) + 0x80
    table = bytearray(table_size)
    control_start = start + table_size
    controls_at: dict[int, int] = {}
    control_blob = bytearray()
    for number, ((_, control_index), record_offset) in enumerate(zip(records, record_offsets)):
        if control_index not in controls_at:
            controls_at[control_index] = control_start + len(control_blob)
            control_blob.extend(controls[control_index])
        entry = bytearray(0x80)
        struct.pack_into("<Q", entry, 0, records[number][0])
        struct.pack_into("<I", entry, 8, len(controls[control_index]))
        struct.pack_into("<i", entry, 0x0C, controls_at[control_index] - (start + record_offset))
        table[record_offset:record_offset + 0x80] = entry
    pool = (control_start + len(control_blob) + 0x10 + len(code) + 15) & ~15
    out = bytearray(pool + len(code))
    out[:4] = b"BAHS"
    struct.pack_into("<III", out, 20, start, len(code), pool)
    out[start:start + len(table)] = table
    out[control_start:control_start + len(control_blob)] = control_blob
    out[pool:] = code
    return bytes(out)


class BahsTests(unittest.TestCase):
    def test_shared_control_different_code_has_different_key(self):
        ctl = control(1, 0x100, 0x100, 7)
        first = bytes.fromhex("78563412") + bytes(0xFC)
        second = bytes.fromhex("78563412") + bytes(0xFC)
        second = second[:4] + b"\x55" + second[5:]
        data = bahs([(0, 0), (0x100, 0)], first + second, [ctl, ctl])
        shaders = acnh.sharcb(data)
        self.assertEqual(len(shaders), 2)
        self.assertEqual({s.control for s in shaders}, {ctl})
        self.assertEqual(len({s.key for s in shaders}), 2)

    def test_degenerate_c1_is_empty_but_code_is_kept(self):
        ctl = control(0, 0, 1, 9)
        record = b"\x78"
        data = bahs([(0, 0)], record, [ctl])
        shader = acnh.sharcb(data)[0]
        self.assertEqual(shader.code, b"\x78")
        self.assertEqual(shader.control[0x700:0x708], struct.pack("<II", 0, 1))

    def test_same_control_reused_by_non_fixed_size_records(self):
        ctl = control(1, 0x100, 0x100, 11)
        first = bytes.fromhex("78563412") + bytes(0xFC)
        second = bytes.fromhex("78563412") + b"\x33" + bytes(0xFB)
        # The second control-size field is at +0x88, not at a multiple of
        # 0x80 from the record table start.
        data = bahs([(0, 0), (0x100, 0)], first + second, [ctl], record_offsets=[0, 0x88])
        shaders = acnh.sharcb(data)
        self.assertEqual(len(shaders), 2)
        self.assertEqual({s.key for s in shaders}, {
            hashlib.sha256(ctl + first).hexdigest()[:16],
            hashlib.sha256(ctl + second).hexdigest()[:16],
        })

    def test_same_key_different_bytes_is_rejected(self):
        ctl = control(1, 0, 1)
        same = b"\x78"
        with self.assertRaises(acnh.FormatError):
            acnh.inventory([
                acnh.Shader("0123456789abcdef", "ps", ctl, same, "a"),
                acnh.Shader("0123456789abcdef", "ps", ctl, b"\x79", "b"),
            ])


class RecordTests(unittest.TestCase):
    def test_translation_filter_rejects_non_program_sentinel(self):
        self.assertFalse(acnh.is_translatable(acnh.Shader(
            "", "vs", control(0, 0, 1), b"\x78", "sentinel"
        )))
        self.assertTrue(acnh.is_translatable(acnh.Shader(
            "", "vs", control(0, 0x100, 0x100), bytes.fromhex("78563412") + bytes(0xFC), "program"
        )))

    def test_split_record_accepts_degenerate_code_without_magic_marker(self):
        shader = acnh.Shader(
            "0123456789abcdef", "vs", control(0, 0, 1), b"\x78", "test"
        )
        code, ctl = acnh.split_record(shader.record())
        self.assertEqual(code, b"\x78")
        self.assertEqual(ctl, shader.control)

    def test_builder_records_can_split_the_output(self):
        shader = acnh.Shader(
            "0123456789abcdef", "ps", control(1, 0x100, 0x100), b"\x78" * 0x100, "test"
        )
        record = shader.record()
        self.assertEqual(record[:8], b"BTCDBTCD")
        self.assertEqual(record[0x38:0x38 + len(shader.code)], shader.code)
        sys_path = ROOT / "dm/gfx/tools/dmsh_build.py"
        self.assertTrue(sys_path.exists())
        self.assertEqual(len(record), 0x38 + len(shader.code) + len(shader.control))

    def test_key_is_control_plus_full_code(self):
        shader = acnh.Shader(
            "", "ps", control(1, 0x100, 0x100), b"\x78" * 0x100, "test"
        )
        key = hashlib.sha256(shader.control + shader.code).hexdigest()[:16]
        self.assertEqual(acnh._shader(shader.control, shader.code, "test").key, key)

    def test_manifest_files(self):
        shaders = [acnh._shader(control(1, 0x100, 0x100), bytes.fromhex("78563412") + bytes(0xFC), "test")]
        with tempfile.TemporaryDirectory() as td:
            count, duplicate = acnh.write(shaders, Path(td))
            self.assertEqual((count, duplicate), (1, 0))
            self.assertTrue((Path(td) / (shaders[0].key + ".bin")).exists())
            self.assertIn("\t像素\t", (Path(td) / "shader_map.tsv").read_text())
            self.assertFalse((Path(td) / "c1_full.tsv").exists())
            self.assertEqual((Path(td) / "shader_skipped.tsv").read_text().count("\n"), 1)


if __name__ == "__main__":
    unittest.main()
