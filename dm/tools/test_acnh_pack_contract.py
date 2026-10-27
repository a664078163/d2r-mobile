#!/usr/bin/env python3
"""Focused tests for the DMACNHPK/DMACNHSET resource contract."""

from __future__ import annotations

import argparse
import json
import os
import tempfile
import unittest
from pathlib import Path

import acnh_pack


def build_args(root: Path, output: Path, *, name: str = "pack",
               esize: int = acnh_pack.MAX_ENTRY,
               layer: str | None = None) -> argparse.Namespace:
    return argparse.Namespace(
        romfs=str(root),
        output=str(output),
        name=name,
        target_size="4M",
        start=1,
        limit=0,
        progress=False,
        esize=esize,
        set=layer,
    )


class AcnhPackContractTests(unittest.TestCase):
    def test_python_accepts_a_64_mib_entry(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td) / "RomFS"
            out = Path(td) / "packs"
            root.mkdir()
            with (root / "large.bin").open("wb") as fh:
                fh.truncate(64 << 20)

            acnh_pack.build(build_args(root, out, esize=64 << 20))
            info = acnh_pack.inspect(out / "pack-000.dmp")

            self.assertEqual(info["bytes"], 64 << 20)

    def test_esize_limits_build_and_verify(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td) / "RomFS"
            out = Path(td) / "packs"
            root.mkdir()
            (root / "too-large.bin").write_bytes(os.urandom(2 << 20))

            with self.assertRaises(SystemExit):
                acnh_pack.build(build_args(root, out, esize=1 << 20))

            acnh_pack.build(build_args(root, out, name="update", layer="update"))
            manifest_path = out / "update.set.json"
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest["entrySizeLimit"] = 1 << 20
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaises(SystemExit):
                acnh_pack.verify_set(manifest_path)

    def test_v2_manifest_uses_canonical_base_name_and_fields(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td) / "RomFS"
            out = Path(td) / "packs"
            root.mkdir()
            (root / "small.bin").write_bytes(b"resource")

            acnh_pack.build(build_args(root, out, name="base", layer="base"))

            manifest_path = out / "base.set.json"
            self.assertTrue(manifest_path.is_file())
            self.assertFalse((out / "romfs.set.json").exists())
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            self.assertEqual(manifest["layer"], "base")
            self.assertEqual(manifest["fileCount"], 1)
            self.assertIn("originalBytes", manifest)
            self.assertEqual(
                set(manifest["packs"][0]),
                {"name", "sha256", "packBytes", "fileCount",
                 "pairIndex", "pairCount"},
            )
            verified = acnh_pack.verify_set(manifest_path)
            self.assertEqual(verified["fileCount"], 1)
            self.assertEqual(verified["originalBytes"], 8)
            self.assertEqual(set(verified["packs"][0]), set(manifest["packs"][0]))


if __name__ == "__main__":
    unittest.main()
