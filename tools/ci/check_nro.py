#!/usr/bin/env python3
"""Checks a built TomoToolNX.nro before it is published.

    python3 tools/ci/check_nro.py TomoToolNX.nro 2.0.1

The file must be an NRO with its icon, NACP and romfs attached, and both the
NACP (what hbmenu shows) and the code (what the updater compares with the
newest release's tag) must say the version being released. The GitHub
workflow runs this on every build, and it works on a local build too.
"""
import struct
import sys


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip().split("\n\n")[1])
    path, want = sys.argv[1], sys.argv[2]
    data = open(path, "rb").read()

    def fail(why):
        sys.exit("%s: %s" % (path, why))

    # NRO0 after the 16-byte start; the code's size, then the asset section.
    if len(data) < 0x80 or data[0x10:0x14] != b"NRO0":
        fail("not an NRO")
    code = struct.unpack_from("<I", data, 0x18)[0]
    if data[code:code + 4] != b"ASET":
        fail("no icon, NACP or romfs after the code")
    icon_off, icon_size, nacp_off, nacp_size, romfs_off, romfs_size = struct.unpack_from("<6Q", data, code + 8)
    if nacp_size < 0x4000:
        fail("no NACP")
    nacp = data[code + nacp_off:code + nacp_off + nacp_size]
    name = nacp[0:0x200].split(b"\0")[0].decode("utf-8", "replace")
    shown = nacp[0x3060:0x3070].split(b"\0")[0].decode("utf-8", "replace")
    if shown != want:
        fail("hbmenu would show version %r, not %r" % (shown, want))
    # updater.cpp's user agent, "TomoToolNX/" APP_VERSION, as one C string.
    if b"TomoToolNX/" + want.encode() + b"\0" not in data[:code]:
        fail("the app inside is not version %r" % want)
    if icon_size == 0:
        fail("no icon")
    if romfs_size < 1 << 20:
        fail("the romfs is missing or nearly empty")
    print("%s: %s %s, %.1f MB (romfs %.1f MB)" % (path, name, shown, len(data) / 1048576, romfs_size / 1048576))


if __name__ == "__main__":
    main()
