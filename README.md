# TomoToolNX

A Nintendo Switch homebrew tool for editing Tomodachi Life UGC textures, Miis, player data, and Mii relationships directly from the console — no PC required. Both WebUI and On-Switch modes are supported.
If you need help or wanna request a feature, [click here](https://github.com/miiazertyy/TomoToolNX/issues/new) to make a New issue! There's never any dumb questions.

Based on [LivinTheDreamToolkit](https://gamebanana.com/tools/22435) by the UGC editor contributors.
Mii sharing based on [ShareMii](https://github.com/Star-F0rce/ShareMii) by Star-F0rce.

<img src="https://i.imgur.com/FO7lZ3o.png" width="600">
<img src="https://i.imgur.com/upNJWY0.png" width="600">
<img src="https://i.imgur.com/aHBKPVO.png" width="600">
And more!

---

## Requirements

devkitPro with `devkitA64`, libnx, and the following portlibs:

```sh
dkp-pacman -S switch-sdl2 switch-sdl2_image switch-sdl2_ttf switch-freetype switch-harfbuzz switch-zstd switch-curl switch-mbedtls
```

## Build

```sh
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

**Textures** — browse all UGC textures, preview them, export as PNG, and import a replacement PNG. Images are automatically resized and semi-transparent edge pixels are snapped to fully opaque before encoding to avoid artifacts. Colorspace is converted from sRGB to linear for correct in-game color reproduction.

**Miis** — list all Miis in the save, export any Mii as a `.ltd` file, and import a `.ltd` file into any slot. A social graph overlay shows the relationship network for any selected Mii — click "social graph" in the toolbar to open it.

Press **B** on the console to stop the server and return to the mode picker.

---

### On-Switch mode

Browse and edit directly on the console using the controller. No network required.

Five tabs are available, navigated with **L / R** (or **ZL / ZR**):

---

#### Textures tab

- **Up / Down** — navigate the texture list
- **A** — import a PNG from the SD card (file browser opens)
- **Y** — export the selected texture as PNG
- **B** — back to mode picker

The export folder defaults to `/switch/TomoToolNX/Exports` and can be changed via the file browser. The chosen path is saved persistently in `/switch/TomoToolNX/config.ini`.

---

#### Miis tab

- **Up / Down** — navigate the Mii list
- **A** — import a `.ltd` file from the SD card
- **Y** — export the selected Mii as a `.ltd` file
- **B** — back to mode picker

---

#### Player tab

Edit player save data fields: Name, Island Name, Money, Currency, and Boot Count.

- **Up / Down** — select field
- **Left / Right** (or left/right stick) — navigate the nudge buttons (numeric fields) / cycle currency options
- **A** — fire the focused nudge button, or open the keyboard for text fields
- **X** — undo (reverts the selected field to the value it had when the tab was opened)
- **B** — back (prompts to save if there are unsaved changes)

Changes are saved to the player save file. A warning is shown on first entry reminding you to make a backup before editing.

---

#### Mii Stats tab

Edit per-Mii stats and view the social relationship graph. Use **ZL / ZR** to switch between Miis.

**Stats sub-tab** (default):

- **Up / Down** — select field
- **Left / Right** (or stick) — navigate the nudge buttons
- **A** — fire the focused nudge button, or open the keyboard for text fields
- **X** — undo (reverts to the value when the tab was opened)
- **Y** — switch to the Social sub-tab
- **B** — back

**Social sub-tab** (press **Y** to enter):

- **ZL / ZR** — previous / next Mii
- **X** — toggle between focus view (selected Mii at center, connected Miis as satellites) and global graph (all Miis arranged in a circle with all relationship edges drawn)
- **Y** — return to the Stats sub-tab
- **B** — back

The social graph uses color-coded edges by relationship type. The global graph is also accessible from the WebUI Mii tab.

---

## Backups

On first launch (or when choosing to make a new one), the entire save is copied to `/switch/TomoToolNX/Backup/save/` in the background while the app remains usable.

Before every texture import, the original files are backed up to `/switch/TomoToolNX/Backup/imports/<nnn>/`.

---

## Image tips

- Use PNG with a transparent background. Semi-transparent edge pixels (from background removal tools) will be automatically thresholded to fully transparent or fully opaque.
- The image does not need to match the target resolution — it will be resized to fit automatically.
- For best results, remove the background cleanly with no soft edges.
- Colorspace conversion (sRGB → linear) is applied automatically on import to match the game's expected encoding.

---

## File formats

Textures are zstd-compressed NX block-linear images:

| File | Format | Size |
| --- | --- | --- |
| `.ugctex.zs` (FacePaint) | BC1, 512×512 | 131072 B |
| `.ugctex.zs` (Food/Goods) | BC1, 384×384 | 98304 B |
| `.ugctex.zs` (256) | BC3, 256×256 | 65536 B |
| `_Thumb_ugctex.zs` | BC3, 256×256 | 65536 B |
| `.canvas.zs` | RGBA8, 256×256 | 262144 B |

Mii data is stored in `.ltd` files (v3 format, compatible with ShareMii).

Save data is read from `Player.dat` and `Mii.dat` within the mounted save. Changes are written back and committed when leaving the respective tab or pressing the save shortcut.
