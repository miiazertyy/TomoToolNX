#!/usr/bin/env python3
"""Read files out of a Switch game's NSP without unpacking it.

For the Mii parts research (tools/mii/README.md): the game's files are read
straight from the NSP you own, only the bytes asked for are decrypted, and
nothing but the files you extract is written. The keys are read from your
own prod.keys / title.keys where they are, kept in memory, never printed or
copied.

    python tools/mii/ltdfs.py list    GAME.nsp --keys prod.keys [--titlekeys title.keys] > list.txt
    python tools/mii/ltdfs.py extract GAME.nsp --keys prod.keys --out DIR PATTERN [PATTERN...]

PATTERN is a substring of the file path (case-insensitive), e.g. "Mii/".

Formats (from the public switchbrew documentation): an NSP is a PFS0 holding
NCAs; an NCA has a 0xC00-byte header encrypted with AES-128-XTS (the header
key, big-endian sector tweaks), then sections encrypted with AES-128-CTR
(the title key for games bought as tickets, else a key from the key area);
the RomFS section's data is the last level of its IVFC hash tree.
Patch RomFS (BKTR) is not read: only the base game.
"""
import argparse
import os
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes


def load_keys(path):
    keys = {}
    if not path:
        return keys
    with open(path, encoding="utf-8", errors="ignore") as f:
        for line in f:
            if "=" not in line:
                continue
            k, v = line.split("=", 1)
            try:
                keys[k.strip().lower()] = bytes.fromhex(v.strip())
            except ValueError:
                pass
    return keys


def aes_ecb_decrypt(key, data):
    d = Cipher(algorithms.AES(key), modes.ECB()).decryptor()
    return d.update(data) + d.finalize()


class Pfs0:
    def __init__(self, f, base=0):
        f.seek(base)
        magic, n, strsz, _ = struct.unpack("<4sIII", f.read(16))
        if magic != b"PFS0":
            raise ValueError("not a PFS0 / NSP")
        entries = [struct.unpack("<QQII", f.read(24)) for _ in range(n)]
        strtab = f.read(strsz)
        data0 = base + 16 + 24 * n + strsz
        self.files = {}
        for off, size, noff, _ in entries:
            end = strtab.index(b"\0", noff)
            self.files[strtab[noff:end].decode()] = (data0 + off, size)


class Section:
    def __init__(self, nca, start, end, fsh):
        self.nca = nca
        self.start = start            # in the NCA
        self.end = end
        self.fs_type = fsh[2]         # 0 RomFS, 1 PFS0
        self.hash_type = fsh[3]       # 2 SHA-256 (PFS0), 3 IVFC (RomFS)
        self.enc_type = fsh[4]        # 1 none, 2 XTS, 3 CTR, 4 BKTR
        self.ctr_hi = fsh[0x140:0x148][::-1]
        self.data_off = 0             # where the file system starts in the section
        self.data_size = end - start
        if self.hash_type == 3:       # IVFC: the data is the last level
            if fsh[8:12] != b"IVFC":
                raise ValueError("bad IVFC")
            levels = struct.unpack_from("<I", fsh, 8 + 12)[0]
            lv = 8 + 0x10 + (levels - 2) * 0x18
            self.data_off, self.data_size = struct.unpack_from("<QQ", fsh, lv)
        elif self.hash_type == 2:     # PFS0 behind a hash table: master hash, block size,
            # layer count, then the regions (hash table, the PFS0 itself)
            self.data_off, self.data_size = struct.unpack_from("<QQ", fsh, 0x40)

    def read(self, off, size):
        """`size` bytes at `off` in the section's file system."""
        nca = self.nca
        pos = self.start + self.data_off + off
        if self.enc_type == 1:
            nca.f.seek(nca.base + pos)
            return nca.f.read(size)
        if self.enc_type != 3:
            raise ValueError("section encryption %d is not supported" % self.enc_type)
        aligned = pos & ~0xF
        lead = pos - aligned
        n = (lead + size + 15) & ~0xF
        nca.f.seek(nca.base + aligned)
        data = nca.f.read(n)
        ctr = self.ctr_hi + (aligned >> 4).to_bytes(8, "big")
        d = Cipher(algorithms.AES(nca.ctr_key), modes.CTR(ctr)).decryptor()
        return (d.update(data) + d.finalize())[lead:lead + size]


class Nca:
    def __init__(self, f, base, keys, titlekeys):
        self.f, self.base = f, base
        f.seek(base)
        raw = f.read(0xC00)
        hk = keys.get("header_key")
        if not hk or len(hk) != 32:
            raise ValueError("header_key missing from the keys file")
        h = bytearray()
        for s in range(6):
            dec = Cipher(algorithms.AES(hk), modes.XTS(s.to_bytes(16, "big"))).decryptor()
            h += dec.update(raw[s * 0x200:(s + 1) * 0x200]) + dec.finalize()
        if h[0x200:0x204] != b"NCA3":
            raise ValueError("not an NCA3 (wrong keys?)")
        self.content_type = h[0x205]
        kg = max(h[0x206], h[0x220])
        kgi = kg - 1 if kg > 0 else 0
        rights = bytes(h[0x230:0x240])
        if any(rights):
            enc = titlekeys.get(rights.hex())
            kek = keys.get("titlekek_%02x" % kgi)
            if enc is None or kek is None:
                raise ValueError("no title key for this game")
            self.ctr_key = aes_ecb_decrypt(kek, enc)
        else:
            names = ("key_area_key_application_%02x", "key_area_key_ocean_%02x", "key_area_key_system_%02x")
            kak = keys.get(names[h[0x207]] % kgi)
            if kak is None:
                raise ValueError("key area key missing")
            self.ctr_key = aes_ecb_decrypt(kak, bytes(h[0x300:0x340]))[0x20:0x30]
        self.sections = []
        for i in range(4):
            start, end = struct.unpack_from("<II", h, 0x240 + i * 0x10)
            if end:
                self.sections.append(Section(self, start * 0x200, end * 0x200, bytes(h[0x400 + i * 0x200:0x600 + i * 0x200])))


class RomFs:
    def __init__(self, sec):
        self.sec = sec
        hdr = sec.read(0, 0x50)
        (_, _, _, self.dir_meta, self.dir_meta_size, _, _, self.file_meta, self.file_meta_size,
         self.data) = struct.unpack("<10Q", hdr)
        self.dirs = sec.read(self.dir_meta, self.dir_meta_size)
        self.filesmeta = sec.read(self.file_meta, self.file_meta_size)

    def walk(self):
        """(path, data offset, size) of every file."""
        out = []

        def dir_at(off, path):
            parent, sibling, child, file_, _, nlen = struct.unpack_from("<6I", self.dirs, off)
            name = self.dirs[off + 24:off + 24 + nlen].decode("utf-8", "replace")
            here = path + name + "/" if off else "/"
            f = file_
            while f != 0xFFFFFFFF:
                _, fsib, doff, dsize, _, fnlen = struct.unpack_from("<IIQQII", self.filesmeta, f)
                fname = self.filesmeta[f + 32:f + 32 + fnlen].decode("utf-8", "replace")
                out.append((here + fname, doff, dsize))
                f = fsib
            c = child
            while c != 0xFFFFFFFF:
                dir_at(c, here)
                c = struct.unpack_from("<I", self.dirs, c + 4)[0]

        dir_at(0, "")
        return out

    def read(self, doff, size):
        return self.sec.read(self.data + doff, size)


def ticket_keys(f, pfs):
    keys = {}
    for name, (off, size) in pfs.files.items():
        if name.endswith(".tik") and size >= 0x2C0:
            f.seek(off)
            t = f.read(0x2C0)
            keys[t[0x2A0:0x2B0].hex()] = t[0x180:0x190]
    return keys


def open_game(path, keys_path, titlekeys_path):
    keys = load_keys(keys_path)
    f = open(path, "rb")
    pfs = Pfs0(f)
    tks = load_keys(titlekeys_path)
    tks.update(ticket_keys(f, pfs))
    best = None
    for name, (off, size) in pfs.files.items():
        if not name.endswith(".nca") or name.endswith(".cnmt.nca"):
            continue
        nca = Nca(f, off, keys, tks)
        for s in nca.sections:
            if s.fs_type == 0 and s.hash_type == 3 and s.enc_type in (1, 3):
                if nca.content_type == 0 and (best is None or s.data_size > best.data_size):
                    best = s
    if best is None:
        raise SystemExit("no RomFS in this NSP")
    return RomFs(best)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("cmd", choices=["list", "extract"])
    ap.add_argument("nsp")
    ap.add_argument("--keys", required=True)
    ap.add_argument("--titlekeys")
    ap.add_argument("--out", default="romfs_out")
    ap.add_argument("patterns", nargs="*")
    a = ap.parse_intermixed_args()
    fs = open_game(a.nsp, a.keys, a.titlekeys)
    files = fs.walk()
    if a.cmd == "list":
        for p, _, size in files:
            print("%10d  %s" % (size, p))
        return
    pats = [p.lower() for p in a.patterns]
    n = total = 0
    for p, off, size in files:
        if pats and not any(q in p.lower() for q in pats):
            continue
        dst = os.path.join(a.out, p.lstrip("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as o:
            pos = 0
            while pos < size:
                chunk = min(8 << 20, size - pos)
                o.write(fs.read(off + pos, chunk))
                pos += chunk
        n += 1
        total += size
    print("extracted %d files, %.1f MB" % (n, total / 1e6), file=sys.stderr)


if __name__ == "__main__":
    main()
