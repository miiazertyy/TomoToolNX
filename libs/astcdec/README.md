# ASTC decoder

From [K0lb3/texture2ddecoder](https://github.com/K0lb3/texture2ddecoder)
(`src/Texture2DDecoder`, MIT, see LICENSE; the half-float helpers in `fp16/`
are from [Maratyszcza/FP16](https://github.com/Maratyszcza/FP16), MIT, notice
in `fp16.h`). It is the same decoder the Mii parts tool uses on a computer
(`tools/mii/nxfmt.py`), so a pack made on the console matches one made there.

TomoToolNX uses it to read the game's Mii face parts (ASTC 4x4) when it makes
the Mii parts pack on the console (`source/mii_pack.cpp`).

One change: `fp16/fp16.h` includes `"bitcasts.h"` instead of
`"fp16/bitcasts.h"`, so it builds without an extra include path.
