# Real Mii pictures

TomoToolNX draws each Mii as it really looks, from the game's own parts: a
**Mii parts pack** made from the game by `build_pack.py`. The app carries one
(`romfs/miiparts`, about 7 MB); without a pack it keeps its drawn faces.

## Make the pack

You need the game (an NSP you own) and your console's keys (`prod.keys`, and
`title.keys` if the game came as a ticket). The keys are read where they are,
kept in memory and never written anywhere.

```
pip install cryptography zstandard numpy pillow texture2ddecoder
python tools/mii/build_pack.py --nsp "GAME.nsp" --keys prod.keys --titlekeys title.keys --out romfs/miiparts
```

Files already taken out of the game work too (`--romfs DIR`). It takes about
three minutes. `--only head,hair,...` and `--hairs 1,2,...` remake a part of it
(the index then lists only what was made).

`python tools/mii/compose.py PACK FILE.ltd ...` draws Miis from `.ltd` files
with a pack (face paint included): the same drawing as the app, in Python, and
the reference for it. `source/mii_art.cpp` matches it to the pixel.

## How the game keeps its Miis

All of this was read from the game's files and code with the tools in this
folder.

| What | Where (RomFS) | Format |
|---|---|---|
| Each part: its number, texture or model | `Pack/MiiParts.pack.zs` → `Mii/Parts/*.mii__Parts.bgyml` | SARC of BYML |
| Each eye's lids, lashes and shine: default place, size, turn | `RSDB/MiiEyeAccessoryParam.Product.100.rstbl.byml.zs` (rows by the murmur3 of the eye's `EyeAccessoryRef`) | BYML |
| Editor colour order (skin is 100–113) | `Editor/MiiEditorColorOrder/*.bgyml` | BYML |
| 2D face parts: eyes, mouths (and their no-lip versions), eyebrows, moustaches, stubble, wrinkles, make-up, eyelashes, eyelids, highlights, mole | `Tex/Pack/MiiParts.bntx.zs` | BNTX, ASTC 4x4 and BC4 |
| Head shapes (16): the head (its own texture on `uv0`, half the face, mirrored), a `Mask` sheet for the face texture, anchor bones `set_nose`, `set_hair`, `set_beard`, `set_ear` | `Model/MiiHead00..15.bfres.zs` | BFRES 10.2 |
| Hairstyles: 129 classic, 115 new, 110 back pieces (+ 83 front pieces); 25 hair parts, one model per place | `Model/MiiHairAllLegacy*`, `MiiHairAll*`, `MiiHairBack*`, `MiiHairFront*`, `MiiHairParts*` | BFRES (a file can hold several models) |
| Hair shading (`_Mim`: R dark detail, G shine) and two-tone masks (`_Mgh`) | `Tex/MiiHair*_Mim`, `_Mgh` | BNTX, BC4/BC5 |
| Noses (31), glasses (19), beards (13), ears (4) | `Model/MiiNose*`, `MiiGlass*`, `MiiBeard*`, `MiiEar*` | BFRES |
| Lens opacity, face paint place and size, edge light | `Parameter/MiiSystem/System.mii__SystemParam.bgyml` | BYML |

- A number in the save (`CharInfoEx`, layout from
  [tlmodding/charinfo-ex](https://github.com/tlmodding/charinfo-ex)) is the
  part's `PartsIndex`. A back piece marked `IsAttachableHairFront` takes a front
  piece (`hairTypeFront`). The built hairstyle (`HairBack000`, number 206)
  also takes a hair part: `hairTypeBack` is its `PartsIndex` (0 none),
  `hairStyle` its place (upper, middle, lower x 4 + centre, left, right, both).
- Colours: the 100 common colours, the skin tones 100–109 and the upper lip's
  shade of each colour are the system Mii library's (`nn::mii`
  `CommonColorTable`, `FacelineColorTable`, `UpperLipColorTable`, in the
  game's `sdk`); 110–113 are the game's own table. The mole is `nn::mii`
  `MoleColor`; wrinkles are black.
- The face texture (eyes, eyebrows, mouth, moustache, mole) follows the
  placement of Nintendo's earlier Mii library (FFL, as decompiled in
  [aboood40091/ffl](https://github.com/aboood40091/ffl)), with the new game's
  rotation offsets. Lids, lashes and the shine sit in each eye's frame (its
  `AxisForExpression`), from the eye's defaults moved by the stored values.
  The mouth turns by `mouthRotate` x 11.25 degrees; with lipstick off (flag
  bit 5) the game draws its no-lip texture, or only its line.
- Make-up, wrinkles and stubble are painted into the head's own texture
  (half the face, mirrored down the middle; game code at 0x175afc): a part is
  a quad anchored on its side toward the middle, stepped out by X and down by
  Y in 1.29/32 of the texture, sized 0.16 (eyeshadow 0.18) x (0.4 scale + 1).
  Blush carries black freckles in its green channel; stubble has a shaved
  blue-grey in red and the stubble colour in green.
- Face paint (the save's `Ugc/UgcFacePaintNNN.canvas.zs`, inside a `.ltd`
  after the A3A3A3A3 mark) lies on the face texture's sheet, 0.75 of its size,
  a little high, under the face's parts unless `IsFacePaintFront`; it reaches
  past the head.
- Two-tone hair (flags bit 2 back hair, bit 3 bangs) is the second colour
  where the `_Mgh` mask says: towards the tips for back hair and hair parts,
  in streaks for bangs.
- Models: vertex buffers sit on 256-byte boundaries in the buffer area;
  positions are float, normals 10-10-10-2, UVs half floats; bones are 0x58
  bytes (scale, rotation, translation at +0x30, +0x3C, +0x4C); a shape's
  points are in its bone's space (0 skin bones), each point's bone (1), or
  model space (2+).

## The tools

| File | Does |
|---|---|
| `ltdfs.py` | Lists and extracts files straight from an NSP (PFS0, NCA, RomFS), decrypting only what is read |
| `nxfmt.py` | Reads zstd, SARC, BYML (with v7 hash dictionaries), BNTX (decoded through `texture2ddecoder`) and BFRES models |
| `miimodel.py` | A model in model space (bones applied), any model of a file by name |
| `gbuf.py` | A small deferred rasterizer: front views keeping depth, normals and texture coordinates |
| `build_pack.py` | Makes the pack the app draws with |
| `compose.py` | Draws a Mii with a pack, as the app does |
| `raster.py`, `render_mii.py` | The first 3D renderer (a `Mii.sav` slot, on the computer) |

## The pack

Front views drawn with one camera, so they stack. Each 3D piece (head, ear,
hairstyle, front piece, hair part, beard) has a picture and a depth picture,
and the app lays them down nearest last, pixel by pixel, as the game's depth
test would (a front piece tucks under the back hair, long hair falls behind
the ears). Pictures are RGBA: R x 1.25 multiplies the part's colour, G lightens
it (shine, the pale rim along the edges), B darkens it (lines), A covers; in
hair pictures B holds the two-tone masks. Each head also has where the face
texture lies on it (`headuv`) and where its own texture lies (`headfl`).
`index.txt` lists every part; `build_pack.py`'s header describes each file.

## Still to do

- Hats and other headwear, and the decorations of hair models (in the hair's
  colour for now).
- Eyes stacked in the middle (`eyeX` 0) at large sizes come out small.
- The console builder (`source/mii_pack.cpp`) still makes the earlier pack
  format: it only runs when the app carries no pack, and this version does not
  draw with that format.
