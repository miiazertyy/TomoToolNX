# TomoToolNX

Nintendo Switch homebrew for editing Tomodachi Life UGC textures. Runs a local web server on the console — open the browser on any device on the same network to import and export textures.

Based on [LivinTheDreamToolkit](https://gamebanana.com/tools/22435) by the UGC editor contributors.

---

## Requirements

devkitPro with `devkitA64`, libnx, and the following portlibs:

```
dkp-pacman -S switch-sdl2 switch-sdl2_image switch-zstd
```

## Build

```
make
```

Output: `TomoToolNX.nro`

## Install

Copy `TomoToolNX.nro` to `/switch/TomoToolNX/` on your SD card and launch from the homebrew menu.

## Usage

Launch the app. It will display the Switch's IP address on screen. Open `http://<ip>:8080` in a browser on any device connected to the same network.

From the web UI you can browse textures, preview them, export as PNG, and import a replacement PNG. Originals are backed up automatically before any import.

## File formats

Textures are zstd-compressed NX block-linear images:

| File | Format | Size |
|------|--------|------|
| `.ugctex.zs` (FacePaint) | BC1, 512x512 | 131072 B |
| `.ugctex.zs` (Food/Goods) | BC1, 384x384 | 98304 B |
| `_Thumb_ugctex.zs` | BC3, 256x256 | 65536 B |
| `.canvas.zs` | RGBA8, 256x256 | 262144 B |

Backups are written to `<ugc-folder>/Backup/<nnn>/` before each import.
Exported PNGs and debug logs go to `/switch/tomodachi-ugc/` on the SD card.

## Project structure

```
TomoToolNX/
├── Makefile
├── include/
│   ├── http_server.h
│   ├── texture_processor.h
│   ├── ugc_scanner.h
│   ├── backup.h
│   └── save_mount.h
└── source/
    ├── main.cpp
    ├── http_server.cpp
    ├── texture_processor.cpp
    ├── ugc_scanner.cpp
    ├── backup.cpp
    └── save_mount.cpp
```
