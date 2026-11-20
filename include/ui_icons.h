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
    // 2.0
    Home,       // the Home tab
    Image,      // textures
    Users,      // Miis
    User,       // player
    Map,        // island map
    Globe,      // "in your browser"
    Wifi,       // network / address
    Usb,        // MTP over USB
    Phone,      // edit from a phone
    Check,      // done / saved / on
    Close,      // dismiss / off
    Plus,       // new
    Alert,      // warning triangle
    Info,       // "i" in a circle
    Trash,      // delete
    Upload,     // arrow out of tray (import / send)
    ChevronLeft,
    ChevronRight,
    ChevronDown,
    Palette,    // theme
    Clock,      // backups / history
    Folder,     // file browser
    Sparkle,    // island generator
    // 2.0, round 2
    Speaker,    // sounds
    Exit,       // leave the island
    Sliders,    // settings categories, adjust
    Grid,       // a grid of things
    Heart,      // relations
    Chat,       // words
    Smile,      // personality
    // round 3
    RotateLeft, // a turning arrow, counter-clockwise
    RotateRight,// the same, clockwise
    FlipH,      // mirror left to right
    FlipV,      // mirror top to bottom
    List,       // rows: a menu seen as a list
    PanelClose, // a window with its side panel, ‹: fold the sidebar
    PanelOpen,  // the same, ›: open it again
    Count       // not an icon: "none"
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
