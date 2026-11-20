#!/usr/bin/env python3
"""The app's interface sounds from Tomodachi Life's own: romfs/sfx/game.bank,
the same format as ui.bank (render_sounds.mjs), one game sound per voice of
source/ui_sound.cpp (the cursor, decide, open, close, page, switches, undo...).

    python tools/sounds/build_game_bank.py <the game's RomFS folder>

Reads Sound/Resource/System_Static.Product.100.bars.zs (dumped from your own
copy; tools/mii/ltdfs.py can extract it): a BARS archive of BWAV sounds, each
named in its AMTA. Each sound is decoded (DSP-ADPCM), brought to the same
peak, and stored dry (the game's sounds carry their own space).
"""
import os
import re
import struct
import sys
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "mii"))
import nxfmt  # noqa: E402

OUT = os.path.join(HERE, "..", "..", "romfs", "sfx", "game.bank")
RATE = 48000

# voice (ui_sound.cpp kNames) -> the game's sound
VOICES = {
    "tap": "SE_UI_Decide1s",          # a press
    "up": None,                       # (the press's second half: the game's decide is one sound)
    "tick": "SE_UI_Cursor",           # the highlight moves
    "on": "SE_UI_button_check",
    "off": "SE_UI_button_uncheck",
    "select": "SE_UI_Decide3",        # picked from a list
    "open": "SE_UI_OpenMiddle",       # a sheet or page opens
    "menu": "SE_UI_OpenLong",         # a menu opens
    "ok": "SE_UI_Decide5",            # done well
    "info": "SE_UI_TelopBottom3",     # a note
    "err": "SE_UI_Relation_Gauge_LevelDown",
    "send": "SE_UI_TelopBottom5",
    "receive": "SE_UI_TelopBottom",
    "delete": "SE_UI_Down_Short",
    "undo": "SE_UI_Undo",
    "wind": "SE_UI_SlideClick",       # a slider's notch
    "slide": "SE_UI_SlideClick",
    "rise": "SE_UI_ZoomUp",
    "done": "SE_UI_Register",         # saved
    "key": "SE_UI_UgcEditor_Spoit",
    "land": "SE_Mii_PickDown_s",      # put down
    "pluck": "SE_UI_circle_cursor_on",
    "zip": "SE_UI_ZoomDown",
    "seat": "SE_UI_circle_off",       # a bump at the edge
    "glint": "SE_UI_anim_FocusInfoOn_play",
    "dawn": "SE_UI_MiiCharacter_Start",   # the app opens
    "whoosh": "SE_UI_PAGE",           # another page
    "settle": "SE_UI_TelopBottom4",   # a card arrives
    "grab": "SE_UI_Pointing_TouchStart",  # picked up
    "release": "SE_UI_Close_s",       # back, close
}
PEAK = 0.7 * 32767        # every sound to the same peak (-3 dB), at most four times louder


def bars(data):
    magic, _size, bom, _ver, count = struct.unpack_from("<4sIHHI", data, 0)
    assert magic == b"BARS" and bom == 0xFEFF, "not a BARS"
    pairs = [struct.unpack_from("<II", data, 16 + 4 * count + 8 * i) for i in range(count)]
    out = {}
    for am, wav in pairs:
        size = struct.unpack_from("<I", data, am + 8)[0]
        names = re.findall(rb"[A-Za-z][A-Za-z0-9_]{3,}", data[am:am + size])
        name = next((n.decode() for n in names if n.startswith((b"SE_", b"BGM_", b"Jingle_", b"JINGLE_"))), None)
        if name and wav and data[wav:wav + 4] == b"BWAV":
            out[name] = wav
    return out


def decode_bwav(data, base):
    total = struct.unpack_from("<H", data, base + 0x0E)[0]
    chans = []
    for c in range(min(total, 2)):
        o = base + 0x10 + c * 0x4C
        codec, _pan, rate, n = struct.unpack_from("<HHII", data, o)
        coefs = struct.unpack_from("<16h", data, o + 0x10)
        off = base + struct.unpack_from("<I", data, o + 0x30)[0]
        pcm = np.zeros(n, np.float64)
        if codec == 0:
            pcm[:] = np.frombuffer(data, "<i2", n, off)
        else:
            h1 = h2 = 0
            for i in range(n):
                f, s = divmod(i, 14)
                b0 = off + f * 8
                ps = data[b0]
                if s == 0:
                    scale = 1 << (ps & 15)
                    c1, c2 = coefs[((ps >> 4) & 7) * 2], coefs[((ps >> 4) & 7) * 2 + 1]
                b = data[b0 + 1 + (s >> 1)]
                nib = (b & 15) if s & 1 else (b >> 4)
                if nib >= 8:
                    nib -= 16
                v = (nib * scale * 2048 + 1024 + c1 * h1 + c2 * h2) >> 11
                v = max(-32768, min(32767, v))
                h2, h1 = h1, v
                pcm[i] = v
        chans.append((pcm, rate))
    pcm = np.mean([c[0] for c in chans], axis=0)
    rate = chans[0][1]
    if rate != RATE:
        t = np.arange(int(len(pcm) * RATE / rate)) * (rate / RATE)
        pcm = np.interp(t, np.arange(len(pcm)), pcm)
    return pcm


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    data = nxfmt.read(os.path.join(sys.argv[1], "Sound", "Resource", "System_Static.Product.100.bars.zs"))
    found = bars(data)
    entries, chunks, cursor = [], [], 0
    for voice, se in VOICES.items():
        if se is None:
            pcm = np.zeros(1)
        else:
            if se not in found:
                sys.exit("not in this copy of the game: " + se)
            pcm = decode_bwav(data, found[se])
            # The tail that is only silence goes.
            loud = np.nonzero(np.abs(pcm) > 24)[0]
            pcm = pcm[: (loud[-1] + 1) if len(loud) else 1]
            peak = float(np.max(np.abs(pcm))) or 1.0
            pcm = pcm * min(4.0, PEAK / peak)
        s16 = np.clip(np.round(pcm), -32768, 32767).astype("<i2")
        entries.append((voice, len(s16), cursor))
        chunks.append(s16.tobytes())
        cursor += len(s16)
        print("%-8s %-36s %5.0f ms" % (voice, se or "(silent)", len(s16) / RATE * 1000))
    head = bytearray(16 + len(entries) * 36)
    struct.pack_into("<4sIII", head, 0, b"TTSB", 1, RATE, len(entries))
    for i, (voice, frames, dry) in enumerate(entries):
        o = 16 + i * 36
        head[o:o + 12] = voice.encode("latin1")[:11].ljust(12, b"\0")
        struct.pack_into("<fBBHIII", head, o + 12, 0.5, 0, 0, 0, frames, dry, 0xFFFFFFFF)
    raw = bytes(head) + b"".join(chunks)
    packed = zlib.compress(raw, 9)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(b"TTSZ" + struct.pack("<I", len(raw)) + packed)
    print("%d voices, %.0f KB raw, %.0f KB packed -> %s" % (len(entries), len(raw) / 1024, len(packed) / 1024, os.path.relpath(OUT)))


if __name__ == "__main__":
    main()
