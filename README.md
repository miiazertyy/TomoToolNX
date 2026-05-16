# TomoToolNX

A Nintendo Switch homebrew tool for editing Tomodachi Life save data directly from the console — no PC required. WebUI and on-Switch modes are both supported.

---

### Need help?
If you need help or wanna request a feature, [click here](https://github.com/miiazertyy/TomoToolNX/issues/new) to make a New issue! There's never any dumb questions.

---

> Based on [LivinTheDreamToolkit](https://gamebanana.com/tools/22435) by the UGC editor contributors.
> Mii sharing based on [ShareMii](https://github.com/Star-F0rce/ShareMii) by Star-F0rce.
> The save-file parser, generated data tables (wishes, habits, clothes, coordinates, treasures), Map.sav schema hashes, UGC kind table, housing-edit safety rules, and the Custom BC1/BC3 texture encoder are derived from [alexislours/ltd-save-editor](https://github.com/alexislours/ltd-save-editor) (AGPL-3.0-or-later).
> And big thanks to [tlmodding/ltd-gamedata](https://github.com/tlmodding/ltd-gamedata) for early reference for save key hashes.

(Outdated Pictures)

<img src="https://i.imgur.com/FO7lZ3o.png" width="600">
<img src="https://i.imgur.com/upNJWY0.png" width="600">
<img src="https://i.imgur.com/aHBKPVO.png" width="600">

---

### Submitting your translation
Open a Pull Request [here](https://github.com/miiazertyy/TomoToolNX/compare) with whichever files you touched. You can translate just the WebUI, just the homebrew, or both — every key counts.
Instructions at the bottom of the README.

---

## Features

- **Textures** — swap any UGC texture (FacePaint, Food, Goods, Thumbs) by importing a PNG. Images are resized, alpha-cleaned, and re-encoded automatically.
- **Miis** — edit just about everything visible: stats, words, habits, belongings, relationships, housing, and the social graph. Import / export individual Miis as `.ltd` files.
- **TomodachiShare** — browse the online catalogue, search and filter, preview, and download shared Miis directly into a save slot.
- **Player save** — adjust the global player fields: name, island, money, currency, birthday, region, languages, wishes, fountain, island size, and more.
- **Map** — view your island as a 120×80 tile grid, click any object to inspect and edit its position, rotation, linked map ID, or which Miis live in it; place new objects or remove existing ones. Works on both the WebUI and the on-Switch controller / touch UI.
- **Backups** — your save is copied to the SD card before any edit, and every texture import keeps a per-file backup. Restore from inside the app.
- **MTP over USB** — the SD card mounts as a USB drive while the WebUI is running (via [libhaze](https://github.com/ITotalJustice/libhaze)).
- **Translations** — both the WebUI and the on-Switch UI can be translated by editing plain text files. Contributions welcome.
- **Update check** — on launch, lets you know when a newer release is up on GitHub.

---

## Requirements

- [MSYS2](https://www.msys2.org/) (required to build on Windows)
- [devkitPro](https://devkitpro.org/wiki/Getting_Started) with `devkitA64` and `libnx`

Install the required portlibs via `dkp-pacman` (run inside the MSYS2 devkitPro shell):

```sh
dkp-pacman -S switch-sdl2 switch-sdl2_image switch-sdl2_ttf switch-sdl2_mixer switch-mpg123 switch-freetype switch-harfbuzz switch-zstd switch-curl switch-mbedtls
```

`switch-sdl2_mixer` (+ `switch-mpg123` for the MP3 backend) is used for the on-Switch UI click / nav sound effects. `dkp-pacman` will pull in the transitive deps (`libogg`, `libvorbisidec`, `libopus`, `opusfile`, `libmodplug`, `flac`) automatically.

## Build

Run inside the MSYS2 devkitPro shell:

```sh
make setup   # first time only — clones libhaze (MTP support) into libs/libhaze/
make
```

`make setup` only needs to be run once after cloning the repo. Every subsequent build is just `make`.

Output: `TomoToolNX.nro`

## Install

Copy `TomoToolNX.nro` to `/switch/TomoToolNX/` on your SD card and launch from the homebrew menu.

---

## Usage

Launch the app, pick a user account, decide what to do with any existing save backup, then choose a mode:

- **WebUI** — starts a local HTTP server. Open `http://<switch-ip>:8080` on any device on the same network and edit from the browser. The SD card also mounts over USB while the server is running.
- **On-Switch** — browse and edit directly with the controller. Tabs are switched with **L / R** (or **ZL / ZR**); each tab shows its own controls at the bottom of the screen.

Press **B** to back out at any point. The app prompts before discarding unsaved changes.

A few notes on textures: PNGs are auto-resized to the target format, a transparent background with clean edges gives the best result, and encoding is sRGB-aware to match the game's color reproduction. The default Custom encoder is a faithful port of [ltd-save-editor](https://github.com/alexislours/ltd-save-editor)'s BC1/BC3 encoder; a faster PCA-only fallback is available in the settings.

---

## Translations / Contributing a language

Both the **WebUI** and the **on-Switch homebrew UI** are translatable, and you don't need to know C++ to contribute. Untranslated keys automatically fall back to English, so partial translations are welcome.

### WebUI

Every translatable string lives in a single JavaScript block at the top of [`source/http_server.cpp`](source/http_server.cpp). Search the file for `── Translations ──` to find it.

1. Find the `LOCALE_META` array and add your locale code + display name:

    ```js
    const LOCALE_META = [
      {code:'en', label:'English'},
      {code:'fr', label:'Français'},
      {code:'es', label:'Español'},    // ← your new language
    ];
    ```

2. Just below, in the `LOCALES` object, copy the whole `en: { ... }` block, rename the key to your locale code, and translate each value:

    ```js
    es: {
      'tab.textures':'texturas',
      'tab.mii':'mii',
      'tab.player':'jugador',
      // ...translate as many keys as you want — missing ones fall back to English.
    },
    ```

Your language then appears in the WebUI **Settings → Language** dropdown (persisted in `localStorage` per device).

### On-Switch homebrew UI

Translations live as plain `key=value` text files in [`romfs/lang/`](romfs/lang/). To add a language:

1. Copy [`romfs/lang/en.txt`](romfs/lang/en.txt) to `romfs/lang/<your-code>.txt` — for example, `romfs/lang/es.txt`.
2. Translate every value on the right side of `=`. Leave keys (left side) and comment lines (`# …`) alone.
3. Open [`source/i18n.cpp`](source/i18n.cpp) and add a one-line entry to the `kAvailable` list with your code and display name:

    ```cpp
    const std::vector<Lang::Entry> kAvailable = {
        {"en", "English"},
        {"fr", "Français"},
        {"es", "Español"},               // ← your new language
    };
    ```

4. Rebuild the `.nro`. Your language now shows up under **Settings → Language** on-device (persisted in `config.ini`).
