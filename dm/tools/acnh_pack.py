#!/usr/bin/env python3
"""Build and inspect sequential ACNH resource packs.

Large already-compressed assets are stored verbatim.  Small files use raw
DEFLATE only when compression saves at least 6%.  The iOS importer verifies
every file's size and SHA-256 before publishing the tree.

Usage:
  acnh_pack.py build <RomFS> <output-dir> --name update [--target-size 2G]
                         [--start 1] [--limit 20000]
  acnh_pack.py inspect <pack>...
  acnh_pack.py list <pack>
  acnh_pack.py extract <pack>... --destination <dir>
  acnh_pack.py verify-set <set.json>
  acnh_pack.py selftest <output-dir>
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import sys
import tempfile
import zlib
from pathlib import Path
from typing import NoReturn

MAGIC = b"DMACNHPK"
VERSION = 1
HEADER = struct.Struct("<8sIIQQ")
ENTRY = struct.Struct("<HBBQQ32s")
STORE = 0
DEFLATE = 1
MAX_PATH = 512
MIN_ENTRY = 1 << 20
MAX_ENTRY = 64 << 20
MAX_PAIRS = 1000
COMPRESS_LIMIT = 2 << 20
SAFE_PACK_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*\.dmp$")


def die(message: str) -> NoReturn:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def sha256_path(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_size(value: str) -> int:
    if value.endswith("G"):
        return int(value[:-1]) << 30
    if value.endswith("M"):
        return int(value[:-1]) << 20
    if value.endswith("K"):
        return int(value[:-1]) << 10
    return int(value)


def choose_esize(value: str) -> int:
    """Parse the device-side entry limit while keeping CLI errors concise."""
    try:
        size = parse_size(value)
    except (TypeError, ValueError):
        raise argparse.ArgumentTypeError(f"无效的设备条目上限：{value}")
    if size < MIN_ENTRY or size > MAX_ENTRY:
        raise argparse.ArgumentTypeError(
            f"设备条目上限必须在 {MIN_ENTRY >> 20} MiB 到 {MAX_ENTRY >> 20} MiB 之间"
        )
    return size


def validate_entry_limit(value: int) -> int:
    if type(value) is not int or not MIN_ENTRY <= value <= MAX_ENTRY:
        die(f"设备条目上限必须在 {MIN_ENTRY >> 20} MiB 到 {MAX_ENTRY >> 20} MiB 之间")
    return value


def source_files(root: Path) -> list[Path]:
    root = root.resolve()
    if not root.is_dir():
        die(f"RomFS 不是目录：{root}")
    files = sorted(
        p for p in root.rglob("*")
        if p.is_file() and not p.is_symlink() and p.name != ".DS_Store"
    )
    if not files:
        die(f"RomFS 没有文件：{root}")
    return files


def entry_bytes(relative: str, original_size: int, stored_size: int,
                method: int, digest: bytes,
                entry_limit: int = MAX_ENTRY) -> bytes:
    path = relative.encode("utf-8")
    validate_relative(path, relative)
    validate_entry_limit(entry_limit)
    if original_size > entry_limit:
        die(f"单文件超过 {entry_limit >> 20} MiB 设备解压上限：{relative}（{original_size} 字节）")
    if method not in (STORE, DEFLATE):
        die(f"不支持的条目方法：{relative}")
    if method == STORE and stored_size != original_size:
        die(f"原样条目大小不一致：{relative}")
    if stored_size > original_size:
        die(f"条目存储大小大于原始大小：{relative}")
    return ENTRY.pack(len(path), method, 0, original_size, stored_size,
                      digest) + path


def validate_relative(path: bytes, display: str | None = None) -> str:
    label = display if display is not None else path.decode("utf-8", "replace")
    if not path or len(path) > MAX_PATH:
        die(f"路径过长或为空：{label}")
    if b"\0" in path or path.startswith((b"/", b"\\")):
        die(f"资源包路径不安全：{label}")
    try:
        relative = path.decode("utf-8")
    except UnicodeDecodeError:
        die(f"资源包路径不是有效 UTF-8：{label!r}")
    if relative == ".dm-pack-complete":
        die("资源包不能提供完成标记")
    if any(part in ("", ".", "..") for part in relative.split("/")):
        die(f"资源包路径不安全：{relative}")
    if "\\" in relative:
        die(f"资源包路径包含反斜杠：{relative}")
    return relative


def header_bytes(count: int, payload: int, pair_index: int = 0,
                 pair_count: int = 0) -> bytes:
    if (not pair_index and pair_count) or pair_index > pair_count or \
            pair_count > MAX_PAIRS:
        die("批次字段不正确")
    flags = pair_index | (pair_count << 16)
    return HEADER.pack(MAGIC, VERSION, flags, count, payload)


def build(args: argparse.Namespace) -> None:
    root = Path(args.romfs).resolve()
    all_files = source_files(root)
    start = getattr(args, "start", 1)
    limit = getattr(args, "limit", 0)
    if start < 1 or start > len(all_files):
        die(f"起始序号越界：{start}（共 {len(all_files)} 个文件）")
    if limit < 0:
        die(f"文件数量不能为负数：{limit}")
    end = len(all_files) if not limit else min(len(all_files), start - 1 + limit)
    files = all_files[start - 1:end]
    if not files:
        die("选中的文件范围为空")
    out_dir = Path(args.output).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    target = parse_size(args.target_size)
    if target < 4 << 20 or target > 2 << 30:
        die("每包目标大小必须在 4 MiB 到 2 GiB 之间")
    prefix = args.name
    if not SAFE_PACK_NAME.fullmatch(prefix + ".dmp"):
        die("资源包名称只能包含 ASCII 字母、数字、点、下划线和连字符")
    entry_limit = validate_entry_limit(getattr(args, "esize", MAX_ENTRY))
    layer = getattr(args, "set", None)
    if layer == "romfs":
        layer = "base"
    stale_manifest = out_dir / f"{prefix}.manifest.json"
    stale_set = out_dir / f"{layer}.set.json" if layer else None
    stale_packs = set(out_dir.glob(f"{prefix}-*.dmp"))
    generated: set[Path] = set()

    manifest = {
        "format": MAGIC.decode(),
        "version": VERSION,
        "rootName": root.name or "RomFS",
        "source": str(root),
        "sourceFiles": len(all_files),
        "start": start,
        "end": end,
        "files": len(files),
        "originalBytes": 0,
        "packs": [],
        "entrySizeLimit": entry_limit,
        "set": layer,
    }

    current = tempfile.NamedTemporaryFile(
        prefix=f".{prefix}-", suffix=".part", dir=out_dir, delete=False
    )
    count = 0
    payload = 0
    total = 0
    pack_no = 0

    def finish_pack() -> None:
        nonlocal current, count, payload, pack_no
        current.seek(0)
        current.write(header_bytes(count, payload))
        current.flush()
        os.fsync(current.fileno())
        current.close()
        dst = out_dir / f"{prefix}-{pack_no:03d}.dmp"
        Path(current.name).replace(dst)
        generated.add(dst)
        manifest["packs"].append({
            "name": dst.name,
            "files": count,
            "bytes": dst.stat().st_size,
            "sha256": sha256_path(dst),
        })
        current = tempfile.NamedTemporaryFile(
            prefix=f".{prefix}-", suffix=".part", dir=out_dir, delete=False
        )
        current.write(header_bytes(0, 0))
        count = 0
        payload = 0
        pack_no += 1

    current.write(header_bytes(0, 0))
    try:
        for index, path in enumerate(files, 1):
            relative = path.relative_to(root).as_posix()
            size = path.stat().st_size
            digest = bytes.fromhex(sha256_path(path))
            method = STORE
            stored_size = size
            block = None
            if 0 < size <= COMPRESS_LIMIT:
                raw = path.read_bytes()
                # Apple's COMPRESSION_ZLIB decoder consumes raw RFC 1951
                # DEFLATE data, without a zlib/RFC 1950 wrapper.
                compressor = zlib.compressobj(6, zlib.DEFLATED, -15)
                candidate = compressor.compress(raw) + compressor.flush()
                if len(candidate) < len(raw) * 0.94:
                    method = DEFLATE
                    stored_size = len(candidate)
                    block = candidate
            head = entry_bytes(relative, size, stored_size, method, digest,
                               entry_limit)
            estimate = len(head) + stored_size
            if count and current.tell() + estimate > target:
                finish_pack()
            current.write(head)
            if method == STORE:
                with path.open("rb") as src:
                    for chunk in iter(lambda: src.read(1 << 20), b""):
                        current.write(chunk)
            else:
                current.write(block)
            count += 1
            payload += stored_size
            total += size
            if args.progress and index % 1000 == 0:
                print(f"{index}/{len(files)} files, {total / 1e9:.2f} GB",
                      file=sys.stderr)
        finish_pack()
        pair_count = len(manifest["packs"])
        if pair_count > MAX_PAIRS:
            die("资源包批次超过 1000 个")
        for index, item in enumerate(manifest["packs"], 1):
            pack_path = out_dir / item["name"]
            with pack_path.open("r+b") as fh:
                fh.seek(12)
                fh.write(struct.pack("<I", index | (pair_count << 16)))
                fh.flush()
                os.fsync(fh.fileno())
            item["sha256"] = sha256_path(pack_path)
            item["pairIndex"] = index
            item["pairCount"] = pair_count
        manifest["originalBytes"] = total
        (out_dir / f"{prefix}.manifest.json").write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        if layer:
            layer_manifest = {
                "format": "DMACNHSET",
                "version": 2,
                "layer": layer,
                "entrySizeLimit": entry_limit,
                "fileCount": manifest["files"],
                "originalBytes": manifest["originalBytes"],
                "packs": [
                    {
                        "name": item["name"],
                        "sha256": item["sha256"],
                        "packBytes": item["bytes"],
                        "fileCount": item["files"],
                        "pairIndex": item["pairIndex"],
                        "pairCount": item["pairCount"],
                    }
                    for item in manifest["packs"]
                ],
            }
            (out_dir / f"{layer}.set.json").write_text(
                json.dumps(layer_manifest, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
        # Commit the new set only after every pack and manifest has been
        # fsynced.  Old packs from a shorter previous build are removed after
        # commit so a scan can never combine them with the new generation.
        for stale in sorted(stale_packs - generated):
            stale.unlink(missing_ok=True)
    finally:
        try:
            current.close()
        except Exception:
            pass
        try:
            Path(current.name).unlink(missing_ok=True)
        except Exception:
            pass
        for part in out_dir.glob(f".{prefix}-*.part"):
            part.unlink(missing_ok=True)
        if not manifest["packs"]:
            for generated_pack in generated:
                generated_pack.unlink(missing_ok=True)
            stale_manifest.unlink(missing_ok=True)
            if stale_set:
                stale_set.unlink(missing_ok=True)
    print(f"wrote {len(files)} files in {len(manifest['packs'])} packs, "
          f"{total} bytes -> {out_dir}")


def entries(pack: Path, entry_limit: int = MAX_ENTRY):
    with pack.open("rb") as fh:
        info = inspect_stream(fh, pack, entry_limit)
        fh.seek(HEADER.size)
        for _ in range(info["entries"]):
            raw = fh.read(ENTRY.size)
            path_len, method, _, size, stored, digest = ENTRY.unpack(raw)
            relative = validate_relative(fh.read(path_len))
            data = fh.read(stored)
            if len(data) != stored:
                die(f"资源包条目被截断：{relative}")
            yield relative, method, size, digest, data


def inspect_stream(fh, pack: Path | None = None,
                   entry_limit: int = MAX_ENTRY) -> dict:
    validate_entry_limit(entry_limit)
    name = str(pack) if pack else "<stream>"
    size = os.fstat(fh.fileno()).st_size
    raw = fh.read(HEADER.size)
    if len(raw) != HEADER.size:
        die(f"资源包头被截断：{name}")
    magic, version, flags, count, payload = HEADER.unpack(raw)
    if magic != MAGIC or version != VERSION:
        die(f"资源包格式不正确：{name}")
    pair_index = flags & 0xffff
    pair_count = flags >> 16
    if (bool(pair_index) != bool(pair_count) or
            pair_index > pair_count or pair_count > MAX_PAIRS):
        die(f"资源包批次字段不正确：{name}")
    if not count or count > 10_000_000 or payload > 1 << 52:
        die(f"资源包头字段越界：{name}")
    files = 0
    total = 0
    stored = 0
    paths: set[str] = set()
    current = HEADER.size
    for _ in range(count):
        raw = fh.read(ENTRY.size)
        if len(raw) != ENTRY.size:
            die(f"资源包条目头被截断：{name}")
        path_len, method, _, original, compressed, _ = ENTRY.unpack(raw)
        if (not path_len or path_len > MAX_PATH or
                method not in (STORE, DEFLATE) or
                original > entry_limit or compressed > original or
                (method == STORE and compressed != original)):
            die(f"资源包条目字段越界：{name}")
        next_offset = current + ENTRY.size + path_len + compressed
        if next_offset > size:
            die(f"资源包条目越过文件末尾：{name}")
        path_bytes = fh.read(path_len)
        if len(path_bytes) != path_len:
            die(f"资源包路径被截断：{name}")
        relative = validate_relative(path_bytes)
        folded = relative.casefold()
        if folded in paths:
            die(f"资源包包含冲突路径：{relative}")
        paths.add(folded)
        fh.seek(compressed, os.SEEK_CUR)
        files += 1
        total += original
        stored += compressed
        current = next_offset
    if current != size:
        die(f"资源包长度与头声明不一致：{name}")
    if stored != payload:
        die(f"资源包 payload 总量与头声明不一致：{name}")
    return {"entries": count, "files": files, "bytes": total,
            "stored": stored, "packSize": size, "pairIndex": pair_index,
            "pairCount": pair_count, "path": name}


def inspect(pack: Path, entry_limit: int = MAX_ENTRY) -> dict:
    with pack.open("rb") as fh:
        return inspect_stream(fh, pack, entry_limit)


def list_pack(pack: Path) -> None:
    for relative, method, size, digest, _ in entries(pack):
        print(f"{method} {size:>12} {digest.hex()} {relative}")


def extract(pack: Path, dest: Path, entry_limit: int = MAX_ENTRY) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    for relative, method, size, expected, data in entries(pack, entry_limit):
        if method == DEFLATE:
            data = zlib.decompress(data, -15)
        elif method != STORE:
            die(f"不支持的条目方法：{relative}")
        if len(data) != size or hashlib.sha256(data).digest() != expected:
            die(f"校验失败：{relative}")
        target = dest.joinpath(*relative.split("/"))
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)


def verify_set(path: Path) -> dict:
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        die(f"资源集清单无法读取：{exc}")
    if (not isinstance(manifest, dict) or
            manifest.get("format") != "DMACNHSET" or
            manifest.get("version") != 2 or
            manifest.get("layer") not in ("base", "update") or
            type(manifest.get("entrySizeLimit")) is not int or
            not MIN_ENTRY <= manifest["entrySizeLimit"] <= MAX_ENTRY or
            type(manifest.get("fileCount")) is not int or
            manifest["fileCount"] <= 0 or
            type(manifest.get("originalBytes")) is not int or
            manifest["originalBytes"] < 0 or
            not isinstance(manifest.get("packs"), list) or
            not manifest["packs"]):
        die(f"资源集清单格式不正确：{path}")
    packs = manifest["packs"]
    if len(packs) > MAX_PAIRS:
        die(f"资源集批次数超过上限：{path}")
    seen_names: set[str] = set()
    seen_indexes: set[int] = set()
    total_files = 0
    total_bytes = 0
    checked = []
    for index, item in enumerate(packs, 1):
        if (not isinstance(item, dict) or
                not isinstance(item.get("name"), str) or
                not SAFE_PACK_NAME.fullmatch(item["name"]) or
                item["name"] in seen_names or
                item.get("pairIndex") != index or
                item.get("pairCount") != len(packs) or
                type(item.get("fileCount")) is not int or item["fileCount"] <= 0 or
                type(item.get("packBytes")) is not int or item["packBytes"] <= 0 or
                not isinstance(item.get("sha256"), str) or
                not re.fullmatch(r"[0-9a-f]{64}", item["sha256"])):
            die(f"资源集第 {index} 个包元数据不正确：{path}")
        pack_path = path.parent / item["name"]
        if not pack_path.is_file():
            die(f"资源集缺少资源包：{pack_path}")
        actual_size = pack_path.stat().st_size
        if actual_size != item["packBytes"]:
            die(f"资源包大小不一致：{pack_path}")
        if sha256_path(pack_path) != item["sha256"]:
            die(f"资源包 SHA-256 不一致：{pack_path}")
        info = inspect(pack_path, manifest["entrySizeLimit"])
        if (info["pairIndex"] != index or
                info["pairCount"] != len(packs) or
                info["files"] != item["fileCount"] or
                info["packSize"] != item["packBytes"]):
            die(f"资源集与包头不一致：{pack_path}")
        seen_names.add(item["name"])
        seen_indexes.add(index)
        total_files += info["files"]
        total_bytes += info["bytes"]
        checked.append({
            "name": item["name"],
            "sha256": item["sha256"],
            "packBytes": info["packSize"],
            "fileCount": info["files"],
            "pairIndex": info["pairIndex"],
            "pairCount": info["pairCount"],
        })
    if (seen_indexes != set(range(1, len(packs) + 1)) or
            total_files != manifest["fileCount"] or
            total_bytes != manifest["originalBytes"]):
        die(f"资源集汇总不一致：{path}")
    return {"path": str(path), "layer": manifest["layer"],
            "packs": checked, "fileCount": total_files,
            "originalBytes": total_bytes}


def selftest(out_dir: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="acnh-pack-test-") as td:
        root = Path(td) / "RomFS"
        (root / "a").mkdir(parents=True)
        (root / "a" / "small.bin").write_bytes(b"abc" * 1000)
        (root / "a" / "zero").write_bytes(b"")
        (root / "random.bin").write_bytes(os.urandom(8192))
        args = argparse.Namespace(
            romfs=str(root), output=str(out_dir), name="selftest",
            target_size="4M", progress=False
        )
        build(args)
        packs = sorted(Path(out_dir).glob("selftest-*.dmp"))
        if not packs:
            die("自检没有生成资源包")
        with tempfile.TemporaryDirectory(prefix="acnh-pack-out-") as od:
            for pack in packs:
                extract(pack, Path(od))
            for path in root.rglob("*"):
                if path.is_file():
                    got = Path(od) / path.relative_to(root)
                    if got.read_bytes() != path.read_bytes():
                        die(f"自检内容不一致：{path}")
        from subprocess import run
        ios_root = Path(__file__).resolve().parents[1] / "ios"
        binary = out_dir / "acnh_pack_selftest"
        run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-I", str(ios_root / "Sources"),
             str(ios_root / "tests" / "acnh_pack_selftest.c"),
             str(ios_root / "Sources" / "dm_pack.c"),
             "-framework", "Foundation", "-lcompression", "-o", str(binary)],
            check=True)
        run([str(binary), str(packs[0])], check=True)
    print("selftest: PASS")


def main() -> None:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="command", required=True)
    p = sub.add_parser("build")
    p.add_argument("romfs")
    p.add_argument("output")
    p.add_argument("--name", default="acnh")
    p.add_argument("--target-size", default="2G")
    p.add_argument("--start", type=int, default=1,
                   help="1-based source-file index for a partial build")
    p.add_argument("--limit", type=int, default=0,
                   help="maximum files in this build; 0 means through the end")
    p.add_argument("--progress", action="store_true")
    p.add_argument("--esize", type=choose_esize, default=MAX_ENTRY,
                   help="maximum original bytes for one entry; default 64M")
    p.add_argument("--set", choices=("update", "base", "romfs"),
                   help="write a layer manifest for the device importer")
    p = sub.add_parser("inspect")
    p.add_argument("pack")
    p = sub.add_parser("list")
    p.add_argument("pack")
    p = sub.add_parser("extract")
    p.add_argument("packs", nargs="+")
    p.add_argument("--destination", required=True)
    p = sub.add_parser("verify-set")
    p.add_argument("set")
    p = sub.add_parser("selftest")
    p.add_argument("output")
    a = ap.parse_args()
    if a.command == "build":
        build(a)
    elif a.command == "inspect":
        print(json.dumps(inspect(Path(a.pack)), ensure_ascii=False, indent=2))
    elif a.command == "list":
        list_pack(Path(a.pack))
    elif a.command == "extract":
        for pack in a.packs:
            extract(Path(pack), Path(a.destination))
    elif a.command == "verify-set":
        print(json.dumps(verify_set(Path(a.set)), ensure_ascii=False, indent=2))
    elif a.command == "selftest":
        selftest(Path(a.output))


if __name__ == "__main__":
    main()
