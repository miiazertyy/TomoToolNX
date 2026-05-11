#!/usr/bin/env python3
"""Generate include/wishes_data.h from LtdSaveEditorTemplate/static/wishes.json."""
import json, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC  = os.path.join(ROOT, "LtdSaveEditorTemplate", "static", "wishes.json")
DST  = os.path.join(ROOT, "include", "wishes_data.h")

with open(SRC, encoding="utf-8") as f:
    data = json.load(f)

data.sort(key=lambda w: (w.get("c", ""), w.get("s", 0), w.get("n", "")))

cats = sorted({w.get("c", "Unknown") for w in data})
cat_map = {c: i for i, c in enumerate(cats)}

def esc(s: str) -> str:
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ord(ch) < 32:
            out.append("?")
        else:
            out.append(ch)
    return "".join(out)

lines = []
lines.append("// Auto-generated from LtdSaveEditorTemplate/static/wishes.json")
lines.append(f"// Do not edit by hand. {len(data)} wishes total.")
lines.append("#pragma once")
lines.append("#include <cstdint>")
lines.append("")
lines.append("namespace WishesData {")
lines.append("")
lines.append("struct WishEntry {")
lines.append("    uint32_t    hash;")
lines.append("    const char* name;       // English (USen) display name, falls back to internal name")
lines.append("    uint8_t     categoryIdx;")
lines.append("};")
lines.append("")
lines.append("static const char* const CATEGORIES[] = {")
for c in cats:
    lines.append('    "%s",' % esc(c))
lines.append("};")
lines.append("static const int CATEGORY_COUNT = %d;" % len(cats))
lines.append("")
lines.append("static const WishEntry WISHES[] = {")
for w in data:
    h = int(w.get("h", 0)) & 0xFFFFFFFF
    n = (w.get("l") or {}).get("USen") or w.get("n", "")
    c = cat_map[w.get("c", "Unknown")]
    lines.append('    { 0x%08xu, "%s", %d },' % (h, esc(n), c))
lines.append("};")
lines.append("static const int WISH_COUNT = %d;" % len(data))
lines.append("")
lines.append("} // namespace WishesData")

out = "\n".join(lines) + "\n"
os.makedirs(os.path.dirname(DST), exist_ok=True)
with open(DST, "w", encoding="utf-8") as f:
    f.write(out)
print("Wrote %s (%d bytes, %d wishes, %d categories)" % (DST, len(out), len(data), len(cats)))
