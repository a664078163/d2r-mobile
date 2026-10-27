#!/usr/bin/env python3
"""Extract NVN shader programs from ACNH's Yaz0/SARC/agl/BFSHA containers.

The output is the same BTCD record layout consumed by dm/gfx/tools/dmsh_build.py:
the BTCD marker, 0x30 bytes of alignment, the code area, then the NVN control
section.  The 0x38-byte prefix keeps the shared builder's code offset unchanged.
ACNH uses two containers:

* agl SHARCFB (magic BAHS): records point into a code pool and each record
  points to its control section.  The record may reuse an earlier control.
* BFSHA (magic FSHA): the archive embeds one or more complete BNSH resources.
  Each BNSH variation has independent control and code offsets.

This module contains container extraction only.  Translation, reflection,
MSL generation, metallib compilation and runtime lookup stay on the shared
dmsh chain.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from dataclasses import dataclass
import struct
import tempfile
from pathlib import Path


CONTROL_SIZE = 0x880
BTCD = b"BTCDBTCD"
GPU_CODE = b"\x78\x56\x34\x12"
CONTROL = b"\x34\x12\x76\x98"
STAGES = {"vs": 0, "ps": 1, "cs": 5}


class FormatError(ValueError):
    """An input container violates its declared bounds or invariants."""


@dataclass(frozen=True)
class Shader:
    key: str
    stage: str
    control: bytes
    code: bytes
    source: str

    def record(self) -> bytes:
        """Return the exact record shape understood by the existing builder."""
        return BTCD + bytes(0x30) + self.code + self.control


def split_record(record: bytes) -> tuple[bytes, bytes]:
    """Split a serialized ACNH record into its code and trailing control.

    Most records contain the NVN control marker, but one legal degenerate
    vertex record has a one-byte code area and therefore no marker in the
    serialized bytes.  The control length field is the reliable boundary for
    both forms.
    """
    if len(record) < 0x38 or record[:8] != BTCD:
        raise FormatError("invalid ACNH BTCD record")
    matches: list[tuple[bytes, bytes]] = []
    for control_size in range(0x7CC, CONTROL_SIZE + 1):
        control_at = len(record) - control_size
        if control_at < 0x38 or control_at + 0x724 > len(record):
            continue
        declared = struct.unpack_from("<I", record, control_at + 0x20)[0]
        canonical = declared if 0x7CC <= declared < CONTROL_SIZE else CONTROL_SIZE
        if canonical != control_size:
            continue
        code = record[0x38:control_at]
        control = record[control_at:control_at + control_size]
        c1, code_length = struct.unpack_from("<II", control, 0x700)
        if code_length and 0 <= c1 <= code_length <= len(code):
            matches.append((code, control))
    if len(matches) != 1:
        raise FormatError("ACNH BTCD record has no unique trailing control")
    return matches[0]


def is_translatable(shader: Shader) -> bool:
    """Return whether the code area contains a real Maxwell program.

    Vertex and pixel records carry the standard 0x30-byte program header;
    compute records use the raw code layout.  ACNH contains one table
    sentinel with a one-byte vertex payload, which is valid to preserve while
    parsing but cannot be passed to the Maxwell translator.
    """
    return bool(shader.code) if shader.stage == "cs" else len(shader.code) >= 0x30 and shader.code[:4] == GPU_CODE


@dataclass(frozen=True)
class Member:
    name: str
    offset: int
    data: bytes


def _checked(data: bytes, offset: int, size: int, what: str) -> bytes:
    if offset < 0 or size < 0 or offset + size > len(data):
        raise FormatError(f"{what} outside file: offset={offset:#x} size={size:#x} file={len(data):#x}")
    return data[offset:offset + size]


def yaz0(data: bytes) -> bytes:
    """Decode standard Yaz0 with overlapping back-reference support."""
    if data[:4] != b"Yaz0":
        raise FormatError("expected Yaz0")
    if len(data) < 16:
        raise FormatError("truncated Yaz0 header")
    size = struct.unpack_from(">I", data, 4)[0]
    if size > 512 << 20:
        raise FormatError("unbounded Yaz0 output")
    out = bytearray(size)
    o = i = 0
    i = 16
    try:
        while o < size:
            flags = data[i]
            i += 1
            for bit in (0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01):
                if o >= size:
                    break
                if flags & bit:
                    out[o] = data[i]
                    i += 1
                    o += 1
                    continue
                first = data[i]
                distance = (((first & 0x0F) << 8) | data[i + 1]) + 1
                length = first >> 4
                i += 2
                if length:
                    length += 2
                else:
                    length = data[i] + 0x12
                    i += 1
                if distance > o:
                    raise FormatError("Yaz0 back reference before output start")
                length = min(length, size - o)
                for _ in range(length):
                    out[o] = out[o - distance]
                    o += 1
    except IndexError as exc:
        raise FormatError("truncated Yaz0 stream") from exc
    if i != len(data) and any(data[i:]):
        raise FormatError(f"Yaz0 compressed stream consumption mismatch: {i:#x} != {len(data):#x}")
    return bytes(out)


def sarc(data: bytes) -> list[Member]:
    """Parse a SARC and validate every name, range and the declared file size."""
    if len(data) < 0x20 or data[:4] != b"SARC":
        raise FormatError("expected SARC")
    header = struct.unpack_from("<H", data, 4)[0]
    bom = data[6:8]
    if bom == b"\xff\xfe":
        endian = "<"
    elif bom == b"\xfe\xff":
        endian = ">"
    else:
        raise FormatError("invalid SARC byte-order mark")
    declared = struct.unpack_from(endian + "I", data, 8)[0]
    start = struct.unpack_from(endian + "I", data, 12)[0]
    if declared != len(data):
        raise FormatError(f"SARC size mismatch: {declared:#x} != {len(data):#x}")
    if header < 0x14 or start < header or start > len(data):
        raise FormatError("invalid SARC header/data offset")
    if data[header:header + 4] != b"SFAT":
        raise FormatError("missing SARC SFAT")
    count = struct.unpack_from(endian + "H", data, header + 6)[0]
    nodes = header + 12
    names = nodes + count * 16
    _checked(data, nodes, count * 16, "SARC nodes")
    if data[names:names + 4] != b"SFNT":
        raise FormatError("missing SARC SFNT")
    names += 8
    result: list[Member] = []
    seen: set[str] = set()
    for index in range(count):
        _, attrs, lo, hi = struct.unpack_from(endian + "4I", data, nodes + index * 16)
        if attrs >> 24 != 1 or hi < lo or start + lo > len(data) or start + hi > len(data):
            raise FormatError(f"invalid SARC node {index}")
        at = names + (attrs & 0xFFFFFF) * 4
        if not names <= at < start:
            raise FormatError(f"SARC name outside string table: node {index}")
        end = data.find(b"\0", at, start)
        if end < 0:
            raise FormatError(f"unterminated SARC name: node {index}")
        try:
            name = data[at:end].decode("utf-8")
        except UnicodeDecodeError as exc:
            raise FormatError(f"non-UTF-8 SARC name: node {index}") from exc
        if name.startswith("/"):
            name = name[1:]
        parts = name.split("/")
        if not name or name.startswith("/") or "\\" in name or any(p in ("", ".", "..") for p in parts):
            raise FormatError(f"unsafe SARC name: {name!r}")
        if name in seen:
            raise FormatError(f"duplicate SARC name: {name}")
        seen.add(name)
        result.append(Member(name, start + lo, _checked(data, start + lo, hi - lo, f"SARC member {name}")))
    return result


def _control_key_len(control: bytes) -> int:
    """The runtime hashes the declared control length, capped at 0x880."""
    if len(control) < 0x24:
        return 0
    size = struct.unpack_from("<I", control, 0x20)[0]
    return size if 0x7CC <= size < CONTROL_SIZE else CONTROL_SIZE


def _stage(control: bytes) -> str:
    if len(control) < 0x718:
        raise FormatError("control section shorter than stage field")
    value = struct.unpack_from("<I", control, 0x714)[0]
    for stage, number in STAGES.items():
        if value == number:
            return stage
    raise FormatError(f"unsupported NVN stage {value}")


def _shader(control: bytes, code: bytes, source: str) -> Shader:
    if len(control) < 0x7CC:
        raise FormatError("control section shorter than 0x7cc")
    length = _control_key_len(control)
    if length > len(control):
        raise FormatError("declared control length outside section")
    control = control[:length]
    c1, code_length = struct.unpack_from("<II", control, 0x700)
    if not code_length or c1 > code_length or code_length > len(code):
        raise FormatError(f"invalid code bounds: c1={c1:#x} code={code_length:#x} available={len(code):#x}")
    code = code[:code_length]
    stage = _stage(control)
    key = hashlib.sha256(control + code).hexdigest()[:16]
    return Shader(key=key, stage=stage, control=control, code=code, source=source)


def sharcb(data: bytes, source: str = "sharcb") -> list[Shader]:
    """Parse agl SHARCFB records and validate their code-pool and control bounds.

    BAHS records are not a fixed-size array.  A record starts eight bytes before
    its u32 control-size field; the signed field at +0x0C points to the shared
    control section.  Locate the declared control sizes and derive every record
    start from its field instead of walking 0x80-byte slots.
    """
    if len(data) < 0x20 or data[:4] != b"BAHS":
        raise FormatError("expected BAHS (agl SHARCFB)")
    start, pool_size, pool = struct.unpack_from("<III", data, 20)
    if not 0x20 <= start <= pool or pool + pool_size > len(data):
        raise FormatError("BAHS records/code pool outside file")
    result: list[Shader] = []
    seen: dict[str, Shader] = {}
    controls: dict[int, bytes] = {}
    # ACNH uses the 0x880-byte NVN control layout.  The 0x88C/0x848/0x7D1
    # variants accepted by other agl titles are deliberately not guessed here;
    # ACNH's stage/key contract is validated from the actual section.
    size_field = struct.pack("<I", CONTROL_SIZE)
    at = start
    while (field := data.find(size_field, at, pool)) >= 0:
        at = field + 4
        record = field - 8
        if record < start or record % 4:
            continue
        code_offset = struct.unpack_from("<Q", data, record)[0]
        relative = struct.unpack_from("<i", data, record + 0x0C)[0]
        control_at = record + relative
        if code_offset >= pool_size or not start <= control_at <= pool - CONTROL_SIZE:
            continue
        code_available = data[pool + code_offset:pool + pool_size]
        if not code_available:
            continue
        try:
            shader = _shader(data[control_at:control_at + CONTROL_SIZE], code_available,
                             f"{source}#{record:#x}")
        except FormatError:
            continue
        if controls.setdefault(control_at, shader.control) != shader.control:
            raise FormatError(f"BAHS control at {control_at:#x} has inconsistent lengths")
        prior = seen.get(shader.key)
        if prior is not None:
            if (prior.stage, prior.control, prior.code) != (shader.stage, shader.control, shader.code):
                raise FormatError(f"BAHS key collision {shader.key}: {prior.source} / {shader.source}")
            continue
        seen[shader.key] = shader
        result.append(shader)
    if not result:
        raise FormatError("BAHS contains no valid shader records")
    return result


def bnsh(data: bytes, source: str = "bnsh") -> list[Shader]:
    """Parse every variation of a complete Switch BNSH resource."""
    if len(data) < 0xC0 or data[:4] != b"BNSH":
        raise FormatError("expected BNSH")
    if data[12:14] != b"\xff\xfe":
        raise FormatError("unsupported BNSH byte order")
    declared = struct.unpack_from("<I", data, 0x1C)[0]
    if declared != len(data):
        raise FormatError(f"BNSH size mismatch: {declared:#x} != {len(data):#x}")
    if data[0x60:0x64] != b"grsc":
        raise FormatError("missing BNSH grsc header")
    count, variations = struct.unpack_from("<IQ", data, 0x7C)
    _checked(data, variations, count * 64, "BNSH variation table")
    result: list[Shader] = []
    seen: dict[str, Shader] = {}
    for index in range(count):
        program = struct.unpack_from("<Q", data, variations + index * 64 + 16)[0]
        _checked(data, program, 0x38, f"BNSH program {index}")
        for stage, relative in (("vs", 8), ("ps", 40), ("cs", 48)):
            header = struct.unpack_from("<Q", data, program + relative)[0]
            if not header:
                continue
            _checked(data, header, 0x38, f"BNSH {stage} header")
            control_at, code_at, code_size, control_size = struct.unpack_from("<QQII", data, header + 8)
            if control_size < 0x7CC or code_size == 0:
                continue
            control = _checked(data, control_at, control_size, f"BNSH {stage} control")
            code = _checked(data, code_at, code_size, f"BNSH {stage} code")
            shader = _shader(control, code, f"{source}#{index}:{stage}")
            if shader.stage != stage:
                raise FormatError(f"BNSH stage mismatch: header={stage} control={shader.stage}")
            prior = seen.get(shader.key)
            if prior is not None:
                if (prior.stage, prior.control, prior.code) != (shader.stage, shader.control, shader.code):
                    raise FormatError(f"BNSH key collision {shader.key}: {prior.source} / {shader.source}")
                continue
            seen[shader.key] = shader
            result.append(shader)
    return result


def bfsha(data: bytes, source: str = "bfsha") -> list[Shader]:
    """Extract all embedded, size-bounded BNSH resources from a Switch BFSHA."""
    if len(data) < 0x20 or data[:8] != b"FSHA    ":
        raise FormatError("expected Switch BFSHA")
    result: list[Shader] = []
    at = 0
    while (at := data.find(b"BNSH", at)) >= 0:
        if at + 0x20 > len(data):
            break
        size = struct.unpack_from("<I", data, at + 0x1C)[0]
        if 0xC0 <= size <= len(data) - at:
            try:
                result.extend(bnsh(data[at:at + size], f"{source}@BNSH:{at:#x}"))
            except FormatError:
                pass
        at += 4
    if not result:
        raise FormatError("BFSHA contains no valid BNSH programs")
    return result


def _decode_member(member: Member) -> bytes:
    data = member.data
    return yaz0(data) if data[:4] == b"Yaz0" else data


def package(data: bytes, source: str = "package") -> list[Shader]:
    """Extract from a Yaz0/SARC package, a bare shader archive, or a nested one."""
    if data[:4] == b"Yaz0":
        data = yaz0(data)
    if data[:4] == b"SARC":
        members = sarc(data)
        result: list[Shader] = []
        for member in members:
            child = _decode_member(member)
            name = f"{source}/{member.name}"
            if child[:4] in (b"BAHS", b"FSHA", b"BNSH", b"SARC", b"Yaz0"):
                result.extend(package(child, name))
            else:
                try:
                    result.extend(bfsha(child, name))
                except FormatError:
                    pass
        if not result:
            raise FormatError("SARC contains no supported shader archive")
        return result
    if data[:4] == b"BAHS":
        return sharcb(data, source)
    if data[:8] == b"FSHA    ":
        return bfsha(data, source)
    if data[:4] == b"BNSH":
        return bnsh(data, source)
    found: list[Shader] = []
    at = 0
    while (at := data.find(b"BNSH", at)) >= 0:
        size = struct.unpack_from("<I", data, at + 0x1C)[0] if at + 0x20 <= len(data) else 0
        if 0xC0 <= size <= len(data) - at:
            try:
                found.extend(bnsh(data[at:at + size], f"{source}@BNSH:{at:#x}"))
            except FormatError:
                pass
        at += 4
    if found:
        return found
    raise FormatError("unsupported shader container")


def scan(root: Path) -> list[Shader]:
    """Recursively scan a RomFS shader tree without retaining game data in the repository."""
    result: list[Shader] = []
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        data = path.read_bytes()
        try:
            result.extend(package(data, str(path.relative_to(root))))
        except FormatError:
            continue
    return result


def inventory(shaders: list[Shader]) -> tuple[dict[str, Shader], int]:
    """Deduplicate exact repeats and reject one key mapped to different bytes."""
    unique: dict[str, Shader] = {}
    duplicate = 0
    for shader in shaders:
        prior = unique.get(shader.key)
        if prior is not None:
            if (prior.stage, prior.control, prior.code) != (shader.stage, shader.control, shader.code):
                raise FormatError(f"shader key collision {shader.key}: {prior.source} / {shader.source}")
            duplicate += 1
            continue
        unique[shader.key] = shader
    return unique, duplicate


def write(shaders: list[Shader], out: Path) -> tuple[int, int]:
    """Write records plus map, c1 and source manifests.

    c1_full.tsv is produced later by c1_guard.py from generated MSL; this
    extractor must not invent a runtime c1 guard list from code offsets.
    """
    out.mkdir(parents=True, exist_ok=True)
    unique, duplicate = inventory(shaders)
    skipped = [(key, shader) for key, shader in sorted(unique.items()) if not is_translatable(shader)]
    rows, c1_rows, source_rows = [], [], []
    for key, shader in sorted(unique.items()):
        if not is_translatable(shader):
            continue
        (out / (key + ".bin")).write_bytes(shader.record())
        stage_name = {"vs": "顶点", "ps": "像素", "cs": "计算"}[shader.stage]
        rows.append(f"acnh\t{shader.source}\t{stage_name}\t{key}\t{shader.source}\t-\t-\t-\t-\t-\t-\t-\t-\n")
        c1_offset, code_size = struct.unpack_from("<II", shader.control, 0x700)
        if not 0 <= c1_offset <= code_size <= len(shader.code):
            raise FormatError(f"{key}: invalid c1 boundary")
        if c1_offset < code_size:
            c1_rows.append(f"{key}\t{c1_offset:#x}\t{code_size:#x}\n")
        source_rows.append(
            f"{key}\t{shader.stage}\t{len(shader.control):#x}\t{len(shader.code):#x}\t"
            f"{c1_offset:#x}\t{code_size:#x}\t{shader.source}\n"
        )
    (out / "shader_map.tsv").write_text(
        "# kind: ACNH sha256(control + code)[:16] -> stage\n"
        "package\tindex\tstage\tdm_ctl_sha16\tpc_dxbc_sha16\tmetallib\tpc_outputs\t"
        "dm_outputs\tdm_ubo\tdm_tex\tdm_samp\tdm_ssbo\tsubset_ok\n"
        + "".join(rows)
    )
    (out / "shader_c1.tsv").write_text("# key\tc1_offset\ttotal_code_size\n" + "".join(c1_rows))
    (out / "shader_sources.tsv").write_text(
        "# key\tstage\tcontrol_size\tcode_size\tc1_offset\tcode_total\tsource\n" + "".join(source_rows)
    )
    (out / "shader_skipped.tsv").write_text(
        "# key\tstage\tcode_size\treason\tsource\n" +
        "".join(f"{key}\t{shader.stage}\t{len(shader.code):#x}\tnot-a-Maxwell-program\t{shader.source}\n"
                for key, shader in skipped)
    )
    return len(unique) - len(skipped), duplicate


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=Path)
    parser.add_argument("--out", type=Path, help="write one <key>.bin record per shader")
    parser.add_argument("--json", type=Path, help="write shader inventory as JSON")
    args = parser.parse_args(argv)
    result: list[Shader] = []
    for path in args.paths:
        if path.is_dir():
            result.extend(scan(path))
        else:
            result.extend(package(path.read_bytes(), str(path)))
    unique, duplicate = inventory(result)
    if args.out:
        write(result, args.out)
    if args.json:
        args.json.write_text(json.dumps([
            {"key": s.key, "stage": s.stage, "source": s.source,
             "control_size": len(s.control), "code_size": len(s.code)}
            for s in unique.values() if is_translatable(s)
        ], indent=1))
    by_stage: dict[str, int] = {}
    for shader in unique.values():
        if not is_translatable(shader):
            continue
        by_stage[shader.stage] = by_stage.get(shader.stage, 0) + 1
    print(f"shaders {sum(is_translatable(s) for s in unique.values())}; skipped {sum(not is_translatable(s) for s in unique.values())}; duplicates {duplicate}; stages {by_stage}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
