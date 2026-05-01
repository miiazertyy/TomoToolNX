# TomoToolNX

A Nintendo Switch homebrew tool for editing Tomodachi Life UGC textures and Miis directly from the console — no PC required. Built because no proper native solution existed for importing custom textures and sharing Miis without manual steps on a computer. Both WebUI and On-Switch modes are supported.

Based on [LivinTheDreamToolkit](https://gamebanana.com/tools/22435) by the UGC editor contributors.
Mii sharing based on [ShareMii](https://github.com/Star-F0rce/ShareMii) by Star-F0rce.

<img src="https://i.imgur.com/QQ9qFNz.png" width="640">
<img src="https://i.imgur.com/u4KvIKd.png" width="640">

---

## Requirements

devkitPro with `devkitA64`, libnx, and the following portlibs:

```
dkp-pacman -S switch-sdl2 switch-sdl2_image switch-sdl2_ttf switch-freetype switch-harfbuzz switch-zstd switch-curl switch-mbedtls
```

## Build

```
make
```

Output: `TomoToolNX.nro`

## Install

Copy `TomoToolNX.nro` to `/switch/TomoToolNX/` on your SD card and launch from the homebrew menu.

---

## Usage

Launch the app and select your user account. You will be asked what to do with any existing save backup before continuing. Then choose a mode.

### WebUI mode

Starts a local HTTP server on the console. Open `http://<ip>:8080` in a browser on any device on the same network.

The web UI has two tabs:

**Textures** — browse all UGC textures, preview them, export as PNG, and import a replacement PNG. Images are automatically padded to square and semi-transparent edge pixels are snapped to fully opaque before encoding to avoid artifacts.

**Miis** — list all Miis in the save, export any Mii as a `.ltd` file, and import a `.ltd` file into any slot. Drag and drop `.ltd` files onto the import area or use the file picker.

Press **B** on the console to stop the server and return to the mode picker.

### On-Switch mode

Browse and edit directly on the console using the controller. No network required.

Two tabs switchable with **L / R**:

**Textures tab**
- Up/Down — navigate the texture list
- A — import a PNG from `/switch/TomoToolNX/` on the SD card
- Y — export the selected texture as PNG to `/switch/TomoToolNX/`
- B — back to mode picker

**Miis tab**
- Up/Down — navigate the Mii list
- A — import a `.ltd` file from `/switch/TomoToolNX/`
- Y — export the selected Mii as a `.ltd` file to `/switch/TomoToolNX/`
- B — back to mode picker

---

## Backups

On first launch (or when choosing to make a new one), the entire save is copied to `/switch/TomoToolNX/Backup/save/` in the background while the app remains usable.

Before every texture import, the original files are backed up to `/switch/TomoToolNX/Backup/imports/<nnn>/`.

---

## Image tips

- Use PNG with a transparent background. Semi-transparent edge pixels (from background removal tools) will be automatically thresholded to fully transparent or fully opaque.
- The image does not need to be square — it will be stretched to fit the target texture size.
- For best results, remove the background cleanly with no soft edges.

---

## File formats

Textures are zstd-compressed NX block-linear images:

| File | Format | Size |
|------|--------|------|
| `.ugctex.zs` (FacePaint) | BC1, 512x512 | 131072 B |
| `.ugctex.zs` (Food/Goods) | BC1, 384x384 | 98304 B |
| `_Thumb_ugctex.zs` | BC3, 256x256 | 65536 B |
| `.canvas.zs` | RGBA8, 256x256 | 262144 B |

Mii data is stored in `.ltd` files (v3 format, compatible with ShareMii).
