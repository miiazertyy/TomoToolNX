#!/usr/bin/env python3
"""Check the translations.

Console (romfs/lang/*.txt):
  * keys the code asks for with Lang::T("...") that en.txt does not have
  * keys en.txt has that another language is missing (they fall back to English)
WebUI (data/webui.html):
  * keys the page asks for (data-i18n attributes and t('...') calls) that the
    English table does not have (the page would show the raw key)
  * per language, how many English keys it has not translated yet

Run from the repository root:  python tools/check_i18n.py
Exit code 1 when a key is used but missing from English (that is a bug);
untranslated keys in other languages are only listed.
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANG_DIR = os.path.join(ROOT, "romfs", "lang")
SRC_DIR = os.path.join(ROOT, "source")
WEBUI = os.path.join(ROOT, "data", "webui.html")


def read(path):
    return io.open(path, encoding="utf-8").read()


def lang_keys(path):
    keys = {}
    for line in read(path).splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        keys[k.strip()] = v.strip()
    return keys


def console():
    bad = False
    en = lang_keys(os.path.join(LANG_DIR, "en.txt"))
    used = set()
    for name in sorted(os.listdir(SRC_DIR)):
        if name.endswith((".cpp", ".h")):
            text = read(os.path.join(SRC_DIR, name))
            used |= set(re.findall(r'Lang::T\(\s*"([^"]+)"\s*\)', text))
            # The 2.0 widgets ask through a short helper: T("...").
            if name.startswith("ui_"):
                used |= set(re.findall(r'\bT\(\s*"([a-z][a-z0-9_.]+)"\s*\)', text))
    # The 2.0 pages (source/pages/*.inc) ask through Tx("...").
    pages = os.path.join(SRC_DIR, "pages")
    if os.path.isdir(pages):
        for name in sorted(os.listdir(pages)):
            if name.endswith(".inc"):
                used |= set(re.findall(r'\bTx\(\s*"([^"]+)"\s*\)', read(os.path.join(pages, name))))
    missing = sorted(k for k in used if k not in en)
    print(f"console: {len(used)} keys used in code, {len(en)} in en.txt")
    if missing:
        bad = True
        print("  used but missing from en.txt:")
        for k in missing:
            print("    " + k)
    for name in sorted(os.listdir(LANG_DIR)):
        if not name.endswith(".txt") or name == "en.txt":
            continue
        other = lang_keys(os.path.join(LANG_DIR, name))
        gaps = [k for k in en if k not in other]
        extra = [k for k in other if k not in en]
        line = f"  {name}: {len(gaps)} untranslated"
        if extra:
            line += f", {len(extra)} unknown (not in en.txt): " + ", ".join(extra[:6]) + (" ..." if len(extra) > 6 else "")
        print(line)
        if "-v" in sys.argv and gaps:
            print("    " + ", ".join(gaps))
    return bad


def webui():
    bad = False
    html = read(WEBUI)
    m = re.search(r"const LOCALES\s*=\s*\{(.*?)\n\};", html, re.S)
    if not m:
        print("webui: LOCALES table not found")
        return True
    body = m.group(1)
    # Split the table into per-language blocks: "  en: {", "  fr: {", ...
    blocks = {}
    for lm in re.finditer(r"\n  ([a-z]{2}):\s*\{(.*?)\n  \},?", body, re.S):
        blocks[lm.group(1)] = set(re.findall(r"'([A-Za-z0-9_.\-]+)'\s*:", lm.group(2)))
    en = blocks.get("en", set())
    used = set(re.findall(r'data-i18n(?:-placeholder|-title)?="([^"]+)"', html))
    used |= set(re.findall(r"\bt\('([A-Za-z0-9_.\-]+)'\)", html))
    missing = sorted(k for k in used if k not in en)
    print(f"webui: {len(used)} keys used, {len(en)} in English")
    if missing:
        bad = True
        print("  used but missing from English:")
        for k in missing:
            print("    " + k)
    for code, keys in blocks.items():
        if code == "en":
            continue
        print(f"  {code}: {len([k for k in en if k not in keys])} untranslated")
    return bad


if __name__ == "__main__":
    b1 = console()
    print()
    b2 = webui()
    sys.exit(1 if (b1 or b2) else 0)
