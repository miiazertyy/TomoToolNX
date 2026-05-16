# `data/` — bin2s-embedded payloads

Files here are auto-converted by devkitPro's `bin2s` step into C symbols accessible from
C++ source. Filename `foo.bin` produces:

```c
extern const u8  foo_bin[];
extern const u8  foo_bin_end[];
extern const u32 foo_bin_size;
```

## Island Generator seed save

`seed_Mii.bin`, `seed_Map.bin`, `seed_Player.bin` are **stripped fresh Tomodachi Life save
files** the homebrew writes into a freshly-created save container when a user has no save
yet. To prepare them:

1. Boot Tomodachi Life on a fresh Switch user, walk through intro until the save is
   created (one default mii, no relationships, default map).
2. Quit the game and dump the save via TomoToolNX's existing backup feature.
3. Open `Mii.sav`, `Player.sav` in a hex editor; null out any byte ranges that contain
   the player's nickname or PII. Leave the file size unchanged.
4. Rename to `seed_Mii.bin`, `seed_Map.bin`, `seed_Player.bin` and drop them here.
5. Rebuild with `make`.

The empty placeholder files in this directory let the build succeed without a seed; the
"Generate Island" path checks `seed_Mii_bin_size > 0` at runtime and refuses gracefully
if the seed is missing.

## Map templates

`tpl_<id>.bin` holds the raw tile-hash array for a hand-authored map layout, extracted
from a real Map.sav by reading the array at hash `MapObject.ActorKey` and dumping its
payload bytes (size prefix included). One file per template. The list of available
templates is hardcoded in `island_generator.cpp` — keep `kTemplateIds[]` in sync with the
filenames here.
