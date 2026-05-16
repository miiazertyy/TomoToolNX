#!/usr/bin/env python3
"""Strip PII from a Tomodachi Life save and emit seed files for bin2s.

Reads the three .sav files from `100savefile/` (or a path you pass), nulls /
overwrites every field that contains player-identifying text, and writes the
results into `data/seed_{Mii,Map,Player}.bin` so the next `make` bundles them
as the Island Generator's default seed.

Specifically it touches:

  Player.sav
    - Player.Name                              → zeroed (player nickname)
    - Player.IslandName                        → zeroed
    - Player.NameRegionLanguageID, etc.        → left as-is; runtime patch
                                                 already rewrites these to
                                                 the booting console's region
    - UGC name arrays (Food, Cloth, Goods,
      Interior, Exterior, MapObject, MapFloor) → every per-slot name zeroed

  Mii.sav
    - Mii.Name.Name array (70 slots)           → renamed "Resident N"
    - Mii.Name.FirstPerson array (if present)  → zeroed
    - Mii.Name.HowToCallName array (if present)→ zeroed

  Map.sav
    - copied through unchanged — no PII lives here

The script does NOT touch:
  - Mii face / personality / belongings / housing / relationships
  - Map tiles / actor placements / building unlocks
  - Money or wish unlocks (you said you want 100% baseline)
  - PhotoStudio/ or Ugc/ files (those don't get bundled — only the 3 .sav
    blobs go into data/ for bin2s)

Usage:
  python tools/strip_seed_pii.py
  python tools/strip_seed_pii.py --input <path> --output <path>

The script is idempotent — run it again with a different `--input` to refresh
the seeds.
"""
from __future__ import annotations

import argparse
import os
import shutil
import struct
import sys
from pathlib import Path


# ─── Murmur3 x86 32-bit (seed 0) ─────────────────────────────────────────────
# Identical implementation to save_editor.cpp's SaveEditor::Hash so we can
# resolve any field by name at script time rather than hand-copying hex.

def _rotl32(x: int, r: int) -> int:
    x &= 0xFFFFFFFF
    return ((x << r) | (x >> (32 - r))) & 0xFFFFFFFF


def mhash(name: str) -> int:
    data = name.encode("utf-8")
    n = len(data)
    nblocks = n // 4
    h1 = 0
    c1 = 0xCC9E2D51
    c2 = 0x1B873593
    for i in range(nblocks):
        (k1,) = struct.unpack_from("<I", data, i * 4)
        k1 = (k1 * c1) & 0xFFFFFFFF
        k1 = _rotl32(k1, 15)
        k1 = (k1 * c2) & 0xFFFFFFFF
        h1 ^= k1
        h1 = _rotl32(h1, 13)
        h1 = (h1 * 5 + 0xE6546B64) & 0xFFFFFFFF
    tail = data[nblocks * 4:]
    k1 = 0
    if len(tail) == 3:
        k1 ^= tail[2] << 16
    if len(tail) >= 2:
        k1 ^= tail[1] << 8
    if len(tail) >= 1:
        k1 ^= tail[0]
        k1 = (k1 * c1) & 0xFFFFFFFF
        k1 = _rotl32(k1, 15)
        k1 = (k1 * c2) & 0xFFFFFFFF
        h1 ^= k1
    h1 ^= n
    h1 ^= (h1 >> 16)
    h1 = (h1 * 0x85EBCA6B) & 0xFFFFFFFF
    h1 ^= (h1 >> 13)
    h1 = (h1 * 0xC2B2AE35) & 0xFFFFFFFF
    h1 ^= (h1 >> 16)
    return h1 & 0xFFFFFFFF


# ─── Minimal .sav parser ─────────────────────────────────────────────────────
# Header is 0x20 bytes:
#   [0x00..0x04]  magic 04 03 02 01
#   [0x04..0x08]  version (u32 LE)
#   [0x08..0x0C]  saveDataOffset (u32 LE) — start of the heap
#   [0x0C..0x20]  padding (zeros)
# Index runs from 0x20 to saveDataOffset, in rows of 8 bytes:
#   [hash:4][slot:4]
# A row with hash == 0 is a TYPE sentinel; `slot` is the DataType id and
# applies to every subsequent row until the next sentinel.
# Heap runs from saveDataOffset to EOF. For non-inline types, `slot` is the
# byte offset of the entry's payload inside the heap.

DT_BOOL = 0
DT_BOOL_ARRAY = 1
DT_INT = 2
DT_INT_ARRAY = 3
DT_FLOAT = 4
DT_FLOAT_ARRAY = 5
DT_ENUM = 6
DT_ENUM_ARRAY = 7
DT_VEC2 = 8
DT_VEC2_ARRAY = 9
DT_VEC3 = 10
DT_VEC3_ARRAY = 11
DT_STR16 = 12
DT_STR16_ARRAY = 13
DT_STR32 = 14
DT_STR32_ARRAY = 15
DT_STR64 = 16
DT_STR64_ARRAY = 17
DT_BIN = 18
DT_BIN_ARRAY = 19
DT_UINT = 20
DT_UINT_ARRAY = 21
DT_INT64 = 22
DT_INT64_ARRAY = 23
DT_UINT64 = 24
DT_UINT64_ARRAY = 25
DT_WSTR16 = 26
DT_WSTR16_ARRAY = 27
DT_WSTR32 = 28
DT_WSTR32_ARRAY = 29
DT_WSTR64 = 30
DT_WSTR64_ARRAY = 31

# Per-element sizes for the heap layouts we care about.
ELEM_SIZE = {
    DT_WSTR16: 32, DT_WSTR16_ARRAY: 32,
    DT_WSTR32: 64, DT_WSTR32_ARRAY: 64,
    DT_WSTR64: 128, DT_WSTR64_ARRAY: 128,
}


def parse_index(data: bytes):
    """Return (entries, save_data_off, version).

    entries: dict[hash] = (dtype, slot)
    """
    if len(data) < 0x20 or data[:4] != b"\x04\x03\x02\x01":
        raise ValueError("bad magic")
    (version,) = struct.unpack_from("<I", data, 4)
    (save_off,) = struct.unpack_from("<I", data, 8)
    if save_off < 0x20 or save_off > len(data):
        raise ValueError("bad saveDataOffset")

    entries: dict[int, tuple[int, int]] = {}
    cur_type = DT_BOOL
    pos = 0x20
    while pos + 8 <= save_off:
        (h, slot) = struct.unpack_from("<II", data, pos)
        pos += 8
        if h == 0:
            cur_type = slot
            continue
        entries[h] = (cur_type, slot)
    return entries, save_off, version


def write_wstr(buf: bytearray, off: int, max_chars: int, text: str) -> None:
    """Zero-fill a fixed-size UTF-16LE region, then write `text` into it.

    `max_chars` is the field's capacity in UTF-16 code units (including the
    terminating null). Anything past `max_chars - 1` is truncated.
    """
    region_bytes = max_chars * 2
    buf[off:off + region_bytes] = b"\x00" * region_bytes
    if not text:
        return
    encoded = text.encode("utf-16-le")[: (max_chars - 1) * 2]
    buf[off:off + len(encoded)] = encoded


# ─── Operations ──────────────────────────────────────────────────────────────


def strip_player(path_in: Path, path_out: Path) -> dict:
    """Scrub Player.sav. Returns a stats dict for printing."""
    raw = path_in.read_bytes()
    entries, save_off, version = parse_index(raw)
    buf = bytearray(raw)
    stats: dict[str, int] = {}

    def zero_scalar_wstr(name: str, *expected_dts: int):
        """Accept any of the WStr* scalar widths — the schema's `Name` and
        `IslandName` are WStr32 on Player root but WStr64 elsewhere; we just
        match whichever width the file actually uses."""
        h = mhash(name)
        if h not in entries:
            return
        dt, slot = entries[h]
        if expected_dts and dt not in expected_dts:
            print(f"[warn] {name}: type {dt} not in expected {expected_dts}")
            return
        size = ELEM_SIZE.get(dt)
        if not size:
            return
        # The fixed-size scalars live at heap[slot..slot+size] verbatim.
        buf[slot:slot + size] = b"\x00" * size
        stats[name] = 1

    def zero_wstr_array_by_hash(label: str, target_hash: int,
                                 label_template: str | None = None):
        """Clear a WStr*Array identified by its raw hash. The save editor's
        schema hashes don't always map to a clean dotted path (some are
        derived from internal node IDs), so we let the caller pass the hash
        directly when needed."""
        if target_hash not in entries:
            print(f"[info] {label}: hash 0x{target_hash:08x} not present")
            return
        dt, slot = entries[target_hash]
        elem_size = ELEM_SIZE.get(dt)
        if not elem_size:
            print(f"[warn] {label}: unsupported array type {dt}")
            return
        base = slot
        if base + 4 > len(buf):
            return
        (count,) = struct.unpack_from("<I", buf, base)
        cleared = 0
        for i in range(count):
            off = base + 4 + i * elem_size
            if off + elem_size > len(buf):
                break
            if label_template:
                write_wstr(buf, off, elem_size // 2,
                           label_template.format(i + 1))
            else:
                buf[off:off + elem_size] = b"\x00" * elem_size
            cleared += 1
        stats[label] = cleared

    def zero_wstr_array(name: str, label_template: str | None = None):
        zero_wstr_array_by_hash(name, mhash(name), label_template)

    # Player root: ltd-save-editor's schema lists Player.Name and
    # Player.IslandName as WStr32 (64-byte fixed buffer). Accept WStr64 too
    # for forward-compat in case a future game patch widens them.
    zero_scalar_wstr("Player.Name",       DT_WSTR32, DT_WSTR64)
    zero_scalar_wstr("Player.IslandName", DT_WSTR32, DT_WSTR64)

    # UGC name arrays — raw hashes from mii_manager.cpp's UGC_KIND_DATA. The
    # ltd-save-editor schema lists them as inner `Name` fields whose hashes
    # are derived from the schema's internal structure rather than from a
    # plain dotted path, so we use the known hex values directly.
    ugc_kinds = [
        ("UgcFood.Name",      0x408494F5),
        ("UgcCloth.Name",     0x40710642),
        ("UgcGoods.Name",     0x2F793EB1),
        ("UgcInterior.Name",  0x3DE2C5DD),
        ("UgcExterior.Name",  0x27C875D6),
        ("UgcMapObject.Name", 0x56F99338),
        ("UgcMapFloor.Name",  0x918875A9),
    ]
    for label, h in ugc_kinds:
        zero_wstr_array_by_hash(label, h)

    path_out.write_bytes(bytes(buf))
    return stats


def strip_mii(path_in: Path, path_out: Path) -> dict:
    """Scrub Mii.sav: rename all 70 slots to "Resident N", zero other name
    fields (FirstPerson, HowToCallName) so no bundler pronunciation leaks."""
    raw = path_in.read_bytes()
    entries, save_off, version = parse_index(raw)
    buf = bytearray(raw)
    stats: dict[str, int] = {}

    def rewrite_wstr_array(name: str, template: str | None):
        h = mhash(name)
        if h not in entries:
            print(f"[info] {name}: hash not present (skipping)")
            return
        dt, slot = entries[h]
        elem_size = ELEM_SIZE.get(dt)
        if not elem_size:
            print(f"[warn] {name}: unsupported array type {dt}")
            return
        base = slot
        (count,) = struct.unpack_from("<I", buf, base)
        cleared = 0
        for i in range(count):
            off = base + 4 + i * elem_size
            if off + elem_size > len(buf):
                break
            if template is None:
                buf[off:off + elem_size] = b"\x00" * elem_size
            else:
                write_wstr(buf, off, elem_size // 2, template.format(i + 1))
            cleared += 1
        stats[name] = cleared

    rewrite_wstr_array("Mii.Name.Name",            "Resident {}")
    rewrite_wstr_array("Mii.Name.FirstPerson",     None)
    rewrite_wstr_array("Mii.Name.HowToCallName",   None)

    path_out.write_bytes(bytes(buf))
    return stats


def copy_map(path_in: Path, path_out: Path) -> dict:
    """Map.sav has no PII — just copy through."""
    shutil.copyfile(path_in, path_out)
    return {"passthrough": 1}


# ─── Driver ──────────────────────────────────────────────────────────────────


def main(argv=None):
    repo = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    # `wipbetter100savefile/` was the curated 100% snapshot during the bundle
    # work; it now points at the same content as `100savefile/`. Prefer the
    # latter when it actually has Player.sav, otherwise fall back to the
    # wip folder. The earlier `is_dir()` check was wrong: an empty
    # wipbetter folder still passes is_dir() and would block the fallback.
    candidates = [repo / "100savefile", repo / "wipbetter100savefile"]
    default_input = candidates[0]
    for c in candidates:
        if (c / "Player.sav").is_file():
            default_input = c
            break
    parser.add_argument("--input",  default=str(default_input),
                        help="folder containing Mii.sav, Map.sav, Player.sav")
    parser.add_argument("--output", default=str(repo / "data"),
                        help="folder to write seed_{Mii,Map,Player}.bin into")
    args = parser.parse_args(argv)

    in_dir = Path(args.input)
    out_dir = Path(args.output)
    if not in_dir.is_dir():
        print(f"input dir does not exist: {in_dir}", file=sys.stderr)
        return 1
    out_dir.mkdir(parents=True, exist_ok=True)

    sources = {
        "Player.sav": ("seed_Player.bin", strip_player),
        "Mii.sav":    ("seed_Mii.bin",    strip_mii),
        "Map.sav":    ("seed_Map.bin",    copy_map),
    }

    for src_name, (out_name, fn) in sources.items():
        src = in_dir / src_name
        dst = out_dir / out_name
        if not src.exists():
            print(f"[skip] {src_name} not found at {src}", file=sys.stderr)
            continue
        try:
            stats = fn(src, dst)
        except Exception as e:
            print(f"[fail] {src_name}: {e}", file=sys.stderr)
            return 2
        size = dst.stat().st_size
        print(f"[ok] {src_name} -> {dst.name} ({size:,} bytes)")
        for k, v in stats.items():
            print(f"       {k}: {v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
