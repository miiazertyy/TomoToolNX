#!/usr/bin/env python3
"""check_save.py - validate Tomodachi Life .sav files against the schema.

Reads the JSON-like enum schemas in ltd-save-editor-2.5.3/src/lib/sav/schema/
and reports any scalar enum or enum-array field whose value is NOT one of
the allowed Murmur3 hashes from enumOptions.ts. Use it after running the
generator to catch "save loads in editor but game refuses it" bugs without
shipping a debug build.

    python tools/check_save.py path/to/save/folder

The folder should contain Map.sav, Mii.sav, Player.sav. Any of the three may
be missing - the script just skips it.
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path


# ── Murmur3 x86 32-bit (seed 0) - identical to SaveEditor::Hash ──────────────
def m3(name: str) -> int:
    data = name.encode("utf-8")
    n = len(data)
    nb = n // 4
    h1 = 0
    c1 = 0xCC9E2D51
    c2 = 0x1B873593

    def rotl(x: int, r: int) -> int:
        return ((x << r) | (x >> (32 - r))) & 0xFFFFFFFF

    for i in range(nb):
        k = int.from_bytes(data[i * 4 : i * 4 + 4], "little")
        k = (k * c1) & 0xFFFFFFFF
        k = rotl(k, 15)
        k = (k * c2) & 0xFFFFFFFF
        h1 ^= k
        h1 = rotl(h1, 13)
        h1 = (h1 * 5 + 0xE6546B64) & 0xFFFFFFFF
    tail = data[nb * 4 :]
    k = 0
    if len(tail) >= 3:
        k ^= tail[2] << 16
    if len(tail) >= 2:
        k ^= tail[1] << 8
    if len(tail) >= 1:
        k ^= tail[0]
        k = (k * c1) & 0xFFFFFFFF
        k = rotl(k, 15)
        k = (k * c2) & 0xFFFFFFFF
        h1 ^= k
    h1 ^= n
    h1 ^= h1 >> 16
    h1 = (h1 * 0x85EBCA6B) & 0xFFFFFFFF
    h1 ^= h1 >> 13
    h1 = (h1 * 0xC2B2AE35) & 0xFFFFFFFF
    h1 ^= h1 >> 16
    return h1


# ── Schema loaders ───────────────────────────────────────────────────────────
def load_enum_options(schema_dir: Path) -> dict[str, list[str]]:
    """enum NAME -> list of identifier strings (in order)."""
    src = (schema_dir / "enumOptions.ts").read_text(encoding="utf-8")
    out: dict[str, list[str]] = {}
    for m in re.finditer(
        r"export const (\w+) = \[\s*([^]]+)\] as const;", src
    ):
        name = m.group(1)
        items = re.findall(r"'([^']*)'", m.group(2))
        out[name] = items
    return out


def load_enum_fields(
    schema_dir: Path, enums: dict[str, list[str]]
) -> tuple[dict[int, tuple[str, set[int]]], dict[int, tuple[str, set[int]]]]:
    """
    Walk every map.ts / mii.ts / player.ts and pull out every field with an
    `options: ENUM_NAME`. Returns (scalar_enum_fields, array_enum_fields)
    keyed by Murmur3 hash, valued (enum_name, set_of_allowed_hashes).
    """
    scalar: dict[int, tuple[str, set[int]]] = {}
    array: dict[int, tuple[str, set[int]]] = {}
    for sf in ("map.ts", "mii.ts", "player.ts"):
        p = schema_dir / sf
        if not p.exists():
            continue
        text = p.read_text(encoding="utf-8")
        for m in re.finditer(
            r"hash:\s*(0x[0-9a-fA-F]+),\s*type:\s*DataType\.(\w+),\s*options:\s*(\w+)",
            text,
        ):
            h = int(m.group(1), 16)
            typ = m.group(2)
            opts = m.group(3)
            if opts not in enums:
                continue
            allowed = {m3(x) for x in enums[opts]}
            if typ == "Enum":
                scalar[h] = (opts, allowed)
            elif typ.endswith("Array"):
                array[h] = (opts, allowed)
    return scalar, array


# ── Save walker ──────────────────────────────────────────────────────────────
def walk_entries(buf: bytes):
    """Yield (hash, type_id, slot_or_offset) for each entry in the index."""
    if len(buf) < 0x20:
        return
    if buf[:4] != b"\x04\x03\x02\x01":
        raise ValueError("bad magic")
    save_data_off = struct.unpack_from("<I", buf, 8)[0]
    cur_type = 0
    i = 0x20
    while i + 8 <= save_data_off:
        h = struct.unpack_from("<I", buf, i)[0]
        s = struct.unpack_from("<I", buf, i + 4)[0]
        if h == 0:
            cur_type = s
        else:
            yield (h, cur_type, s)
        i += 8


def check_save(
    path: Path,
    scalar_fields: dict[int, tuple[str, set[int]]],
    array_fields: dict[int, tuple[str, set[int]]],
) -> list[tuple[int, str, str, int, int]]:
    """Returns [(hash, opts_name, kind, bad_value, idx_in_array)]."""
    bad: list[tuple[int, str, str, int, int]] = []
    buf = path.read_bytes()
    for h, t, s in walk_entries(buf):
        # DT_Enum (scalar inline) = 6 in save_editor.h
        if t == 6 and h in scalar_fields:
            name, allowed = scalar_fields[h]
            if s not in allowed:
                bad.append((h, name, "Enum", s, 0))
        # DT_EnumArray = 7 — heap payload [count:u32][values:u32 * count]
        if t == 7 and h in array_fields and s != 0:
            name, allowed = array_fields[h]
            if s + 4 > len(buf):
                continue
            cnt = struct.unpack_from("<I", buf, s)[0]
            need = 4 + cnt * 4
            if s + need > len(buf):
                continue
            vals = struct.unpack_from(f"<{cnt}I", buf, s + 4)
            for i, v in enumerate(vals):
                # 0 is treated as "unset" - the seed leaves untouched
                # array slots zeroed and the game accepts that.
                if v != 0 and v not in allowed:
                    bad.append((h, name, "EnumArray", v, i))
    return bad


# ── CLI ──────────────────────────────────────────────────────────────────────
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "save_folder",
        type=Path,
        help="Folder containing Map.sav / Mii.sav / Player.sav",
    )
    ap.add_argument(
        "--schema",
        type=Path,
        default=Path("ltd-save-editor-2.5.3/src/lib/sav/schema"),
        help="Path to ltd-save-editor's schema dir (default: ./ltd-save-editor-2.5.3/...)",
    )
    args = ap.parse_args()

    if not args.schema.exists():
        print(f"schema dir not found: {args.schema}", file=sys.stderr)
        return 2

    enums = load_enum_options(args.schema)
    scalar, array = load_enum_fields(args.schema, enums)
    print(
        f"schema loaded: {len(enums)} enum types, "
        f"{len(scalar)} scalar enum fields, {len(array)} enum-array fields"
    )

    any_bad = False
    # Loaded saves keyed by filename so cross-file checks (Mii->Map house
    # references) can look up the other file.
    loaded: dict[str, bytes] = {}
    for name in ("Map.sav", "Mii.sav", "Player.sav"):
        p = args.save_folder / name
        if not p.exists():
            print(f"{name}: (missing - skipped)")
            continue
        loaded[name] = p.read_bytes()
        bad = check_save(p, scalar, array)
        if not bad:
            print(f"{name}: enum OK ({p.stat().st_size} bytes)")
        else:
            any_bad = True
            print(f"{name}: {len(bad)} invalid enum values")
            grouped: dict[tuple[int, str, str], list[tuple[int, int]]] = {}
            for h, opts, kind, v, idx in bad:
                grouped.setdefault((h, opts, kind), []).append((v, idx))
            for (h, opts, kind), occ in grouped.items():
                print(
                    f"  field 0x{h:08x} ({kind} of {opts}): "
                    f"{len(occ)} bad values, first = 0x{occ[0][0]:08x} at idx {occ[0][1]}"
                )

    # ── Semantic checks (run only if all three files are present) ────────
    if {"Map.sav", "Mii.sav", "Player.sav"} <= loaded.keys():
        map_buf = loaded["Map.sav"]
        mii_buf = loaded["Mii.sav"]

        # Hashes used below
        H = {
            "ActorKey":       m3("MapObject.ActorKey"),
            "GridPosX":       m3("MapObject.Location.GridPosX"),
            "GridPosY":       m3("MapObject.Location.GridPosY"),
            "HouseMapId":     m3("House.MapId"),
            "MiiHouseMapId":  m3("Mii.Location.HouseMapId"),
            "MiiRoomIndex":   m3("Mii.Location.RoomIndex"),
            "MiiName":        m3("Mii.Name.Name"),
        }

        def find_offset_and_count(buf: bytes, target_hash: int) -> tuple[int, int] | None:
            for h, t, s in walk_entries(buf):
                if h == target_hash and s != 0:
                    cnt = struct.unpack_from("<I", buf, s)[0]
                    return s, cnt
            return None

        def read_int_array(buf: bytes, target_hash: int) -> list[int]:
            info = find_offset_and_count(buf, target_hash)
            if not info:
                return []
            off, cnt = info
            return list(struct.unpack_from(f"<{cnt}i", buf, off + 4))

        def read_uint_array(buf: bytes, target_hash: int) -> list[int]:
            info = find_offset_and_count(buf, target_hash)
            if not info:
                return []
            off, cnt = info
            return list(struct.unpack_from(f"<{cnt}I", buf, off + 4))

        MAP_W, MAP_H = 120, 80
        keys = read_uint_array(map_buf, H["ActorKey"])
        xs = read_int_array(map_buf, H["GridPosX"])
        ys = read_int_array(map_buf, H["GridPosY"])
        # Position out-of-bounds
        if keys and xs and ys:
            oob = 0
            for i in range(len(keys)):
                if keys[i] == 0:
                    continue
                if i >= len(xs) or i >= len(ys):
                    continue
                x, y = xs[i], ys[i]
                if x < 0 or x >= MAP_W or y < 0 or y >= MAP_H:
                    oob += 1
            if oob:
                any_bad = True
                print(f"Map.sav: {oob} actor(s) at out-of-bounds positions")
            else:
                print(f"Map.sav: actor positions in bounds ({sum(1 for k in keys if k)} placed)")

        # Mii -> house cross-reference
        houses = set(v for v in read_int_array(map_buf, H["HouseMapId"]) if v > 0)
        mii_houses = read_int_array(mii_buf, H["MiiHouseMapId"])
        mii_rooms = read_int_array(mii_buf, H["MiiRoomIndex"])
        # Find valid Mii slots (non-empty names)
        info = find_offset_and_count(mii_buf, H["MiiName"])
        valid_slots: list[int] = []
        if info:
            off, cnt = info
            # WString32 entries are 32 wchar (64 bytes). First byte of name in
            # UTF-16LE is non-zero for a real name.
            for i in range(cnt):
                w0 = struct.unpack_from("<H", mii_buf, off + 4 + i * 64)[0]
                if w0 != 0:
                    valid_slots.append(i)
        if mii_houses and houses:
            dangling = 0
            for s in valid_slots:
                if s < len(mii_houses):
                    h = mii_houses[s]
                    if h > 0 and h not in houses:
                        dangling += 1
            if dangling:
                any_bad = True
                print(f"Mii.sav: {dangling} Mii(s) reference a HouseMapId that doesn't exist in Map.sav")
            else:
                print(f"Mii.sav: housing cross-refs OK ({len(valid_slots)} valid Miis, {len(houses)} houses)")
        # Room-index sanity
        if mii_rooms:
            bad_rooms = 0
            for s in valid_slots:
                if s < len(mii_rooms):
                    r = mii_rooms[s]
                    if r < 0 or r > 16:  # game caps somewhere; >16 is implausible
                        bad_rooms += 1
            if bad_rooms:
                any_bad = True
                print(f"Mii.sav: {bad_rooms} Mii(s) with implausible RoomIndex")

    return 1 if any_bad else 0


if __name__ == "__main__":
    sys.exit(main())
