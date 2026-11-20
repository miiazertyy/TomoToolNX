#!/usr/bin/env python3
"""The game's music for the app, small enough to carry: each piece the app
plays (source/music.cpp, kPieces) decoded from its BWAV (DSP-ADPCM) and
encoded as Opus, about a fifth of the size, with its loop points kept exact.

    python tools/assets/build_music.py <the game's RomFS folder> [--kbps 48] [--jobs 8]

Reads Sound/Resource/Stream/<name>.bwav (dumped from your own copy;
tools/mii/ltdfs.py can extract them) and writes:

  romfs/music/<name>.opus   the piece (48 kHz, stereo)
  romfs/music/index.txt     name samples loops loopStart loopEnd gain, one line each

`gain` brings each piece to about -30 dBFS, where most of the game's music
sits (the night and island-editing takes are much quieter in the files), so
the volume holds from one place to the next. Needs ffmpeg (with libopus).
"""
import argparse
import concurrent.futures as cf
import os
import struct
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "romfs", "music")

# The same pieces as Music::kPieces (file, then the second take when there is one).
PIECES = [
    "BGM_Field03_Title_Basic", "BGM_Field03_Basic", "BGM_Field06_Basic",
    "BGM_Field03_IslandEdit", "BGM_Field06_IslandEdit",
    "BGM_MiiEditor_01a", "BGM_MiiEditor_01b",
    "BGM_UGCEditor_02a", "BGM_UGCEditor_02b",
    "BGM_PhotoStudio", "BGM_Zukan_01", "BGM_Local_Communication_01",
    "BGM_Spot_Fountain_01", "BGM_Spot_FerrisWheel_01", "BGM_News_Topic_Happy_01",
    "BGM_Shop_Food", "BGM_Shop_Cloth", "BGM_Shop_Item", "BGM_Shop_Interior_01", "BGM_Shop_Building", "BGM_Shop_Pawn",
]
TARGET_RMS = 1036.0     # -30 dBFS


def parse(d):
    assert d[:4] == b"BWAV" and struct.unpack_from("<H", d, 4)[0] == 0xFEFF, "not a BWAV"
    total = struct.unpack_from("<H", d, 0x0E)[0]
    chans = []
    for c in range(min(total, 2)):
        o = 0x10 + c * 0x4C
        codec, _pan, rate, n = struct.unpack_from("<HHII", d, o)
        assert codec == 1 and rate == 48000, "DSP-ADPCM at 48 kHz expected"
        coefs = struct.unpack_from("<16h", d, o + 0x10)
        off, _off2, loops, lend, lstart = struct.unpack_from("<IIIII", d, o + 0x30)
        chans.append(dict(n=n, coefs=coefs, off=off, loops=loops, lend=lend, lstart=lstart))
    offs = [struct.unpack_from("<I", d, 0x10 + k * 0x4C + 0x30)[0] for k in range(min(total, 8))]
    for ch in chans:
        ch["end"] = min([x for x in offs if x > ch["off"]] + [len(d)])
    return chans


def decode(d, ch):
    n, off, end, co = ch["n"], ch["off"], ch["end"], ch["coefs"]
    out = np.zeros(n, np.int16)
    h1 = h2 = 0
    i = 0
    frame = 0
    while i < n:
        base = off + frame * 8
        ps = d[base] if base < end else 0
        scale = 1 << (ps & 15)
        c1, c2 = co[((ps >> 4) & 7) * 2], co[((ps >> 4) & 7) * 2 + 1]
        for s in range(14):
            if i >= n:
                break
            bi = base + 1 + (s >> 1)
            b = d[bi] if bi < end else 0
            nib = (b & 15) if s & 1 else (b >> 4)
            if nib >= 8:
                nib -= 16
            v = (nib * scale * 2048 + 1024 + c1 * h1 + c2 * h2) >> 11
            v = -32768 if v < -32768 else 32767 if v > 32767 else v
            h2 = h1
            h1 = v
            out[i] = v
            i += 1
        frame += 1
    return out


def build(game, name, kbps):
    path = os.path.join(game, "Sound", "Resource", "Stream", name + ".bwav")
    d = open(path, "rb").read()
    chans = parse(d)
    pcm = [decode(d, c) for c in chans]
    if len(pcm) == 1:
        pcm = pcm * 2
    st = np.stack(pcm, 1)
    f = st.astype(np.float64)
    rms = float(np.sqrt(np.mean(f * f)))
    peak = float(max(1.0, np.max(np.abs(f))))
    gain = min(max(0.5, min(3.0, TARGET_RMS / rms if rms > 1 else 1.0)), 0.9 * 32767 / peak)
    out = os.path.join(OUT, name + ".opus")
    cmd = ["ffmpeg", "-v", "error", "-y", "-f", "s16le", "-ar", "48000", "-ac", "2", "-i", "-",
           "-c:a", "libopus", "-b:a", "%dk" % kbps, "-vbr", "on", "-compression_level", "10",
           "-application", "audio", "-map_metadata", "-1", out]
    subprocess.run(cmd, input=st.astype("<i2").tobytes(), check=True)
    c = chans[0]
    loops = 1 if c["loops"] and c["lstart"] < min(c["lend"], c["n"]) else 0
    return "%s %d %d %d %d %.4f" % (name, c["n"], loops, c["lstart"], min(c["lend"], c["n"]), gain), os.path.getsize(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game")
    ap.add_argument("--kbps", type=int, default=48)
    ap.add_argument("--jobs", type=int, default=8)
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    lines, total = {}, 0
    with cf.ProcessPoolExecutor(a.jobs) as ex:
        futs = {ex.submit(build, a.game, n, a.kbps): n for n in PIECES}
        for fu in cf.as_completed(futs):
            line, size = fu.result()
            lines[futs[fu]] = line
            total += size
            print("%-30s %7.0f KB" % (futs[fu], size / 1024), flush=True)
    with open(os.path.join(OUT, "index.txt"), "w", newline="\n") as f:
        f.write("# name samples loops loopStart loopEnd gain (tools/assets/build_music.py)\n")
        for n in PIECES:
            f.write(lines[n] + "\n")
    print("%d pieces, %.1f MB" % (len(PIECES), total / 1048576))


if __name__ == "__main__":
    main()
