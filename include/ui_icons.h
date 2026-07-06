#pragma once
// ─── UiIcons — anti-aliased vector button icons ───────────────────────────────
// Replaces the old per-call scan-line / 1px-line glyph drawing with a small
// SDF rasterizer. Each icon is defined as stroke geometry (round-capped
// segments + arcs) on a 24×24 design grid — the same grid and visual language
// as the WebUI's feather/lucide icons — then baked once per (icon, size) into
// a white texture whose alpha channel carries sub-pixel edge coverage.
//
// Because the baked texture is white, a single texture serves every colour:
// tinting happens at blit time via SDL_SetTextureColorMod, mirroring the
// corner-mask caching pattern in main.cpp. Rasterisation cost is one-time and
// tiny (icons are < 64px), so runtime cost is a single RenderCopy per icon.

#include <SDL2/SDL.h>

namespace UiIcons {

enum class Id {
    Gear,       // settings cog
    Floppy,     // save / backup-restore picker
    Pencil,     // edit / rename
    Magnifier,  // zoom / search
    Undo,       // curved arrow, left
    Redo,       // curved arrow, right (mirror of Undo)
    Download,   // arrow into tray (PNG export)
    Funnel,     // category filter
};

// Bind the renderer that textures will be created on. Call once after
// SDL_CreateRenderer. Draw() is a safe no-op until this happens.
void Init(SDL_Renderer* ren);

// Destroy all cached textures. Call before SDL_DestroyRenderer.
void Quit();

// Draw icon centred on (cx, cy), scaled so its 24-unit design grid maps to
// sizePx pixels. Colour (including alpha) is applied via texture modulation.
void Draw(Id id, int cx, int cy, int sizePx, SDL_Color col);

} // namespace UiIcons
