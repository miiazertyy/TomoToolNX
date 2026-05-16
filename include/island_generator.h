#pragma once
// island_generator.h — Island Generator for TomoToolNX
//
// On-device save-mutation helpers used by the no-save bootstrap path
// (Phase A) and by the /api/genisland/* HTTP endpoints. All routines
// operate directly on SaveEditor::SavFile structures; the HTTP layer is
// expected to Load / mutate / Save around them.
//
// The "+N more" / canvas auto-fit work in the Mii social tab is
// orthogonal — these helpers only touch save data.

#include "save_editor.h"
#include <cstdint>
#include <string>
#include <vector>

namespace IslandGen {

// ── Map grid constants (fixed by the game) ──────────────────────────────────
inline constexpr int MAP_W = 120;
inline constexpr int MAP_H = 80;
inline constexpr int MAP_TILES = MAP_W * MAP_H;
inline constexpr uint32_t TILE_WATER = 0xd21b65b6u;

// ── Surface themes for "Surprise me" map generation ─────────────────────────
struct SurfaceTheme {
    const char* id;       // short id for HTTP/UI use
    uint32_t    land;     // tile hash for non-road land cells
    uint32_t    road;     // tile hash for road overlays (currently unused —
                          // reserved for future spanning-path pass)
};

const SurfaceTheme* AllSurfaceThemes(int* outCount);
const SurfaceTheme* SurfaceThemeById(const char* id); // null if unknown

// ── Map templates (raw bin2s payloads embedded in the .nro) ─────────────────
struct MapTemplate {
    const char* id;         // matches data/tpl_<id>.bin
    const char* displayName;
    const char* description;
    const uint8_t* bytes;   // payload (size-prefixed uint32 array, identical
                            // to a save's MAP_H_FLOOR entry payload)
    size_t      size;
};

const MapTemplate* AllMapTemplates(int* outCount);
const MapTemplate* MapTemplateById(const char* id);

// ── Map generation ──────────────────────────────────────────────────────────
//
// Procedurally generate a single connected walkable island into Map.sav's
// floor-tile array (MapGrid.GridX.GridZ.FloorKeyHash). Uses a 4-pass cellular
// automaton + flood-fill to guarantee one contiguous land region. Returns ""
// on success, error string otherwise.
//
// If `seed == 0`, time(0) is used.
std::string GenerateRandomMap(SaveEditor::SavFile& map,
                              const SurfaceTheme& theme,
                              uint32_t seed = 0);

// Apply a hand-authored template by copying its raw bytes verbatim over
// Map.sav's floor-tile array.
std::string ApplyMapTemplate(SaveEditor::SavFile& map,
                             const MapTemplate& tmpl);

// After any tile rewrite: walk every actor placed at (x,y) and, if its tile
// is water, search outward to the nearest land cell and snap to it. Keeps
// houses reachable. Returns number of actors moved.
int SnapActorsToLand(SaveEditor::SavFile& map);

// ── Dense random relationships ──────────────────────────────────────────────
//
// For every unique pair of valid miis in `mii`, write a relationship into the
// IA/IB pair table with a randomly-sampled type (weighted: Know 60%, Friend
// 25%, Lover/Couple 10%, Family 5%) and a meter value drawn from the same
// NATURAL[] pool the existing /api/mii relBatchKnow path uses. Bumps the
// per-pair timestamp to nowSecs. Returns number of pairs written.
int WriteDenseRelationships(SaveEditor::SavFile& mii, uint32_t seed = 0);

// Clear every relationship to (Unknown, 0). Used by the "Everyone unknown"
// generator option.
int WipeRelationships(SaveEditor::SavFile& mii);

// ── Wishes unlock (Player.sav) ──────────────────────────────────────────────
//
// Force-unlock every wish from WishesData::WISHES so freshly generated islands
// start with the full catalog available. Rebuilds Player.sav's
// Liberation.WishInfo.{WishIdValue, IsLiberated, IsNew} arrays to cover all
// 268 wish hashes. Returns count of wishes set to liberated.
int UnlockAllWishes(SaveEditor::SavFile& player);

// ── Level bump for every mii ────────────────────────────────────────────────
//
// Set every non-empty mii's Mii.MiiMisc.SatisfyInfo.Level to a random value in
// [minLevel, maxLevel]. Defaults give 100–150, matching the user's "above 100"
// requirement. Returns number of miis changed.
int RandomizeMiiLevels(SaveEditor::SavFile& mii,
                       int minLevel = 100, int maxLevel = 150,
                       uint32_t seed = 0);

// ── Housing pass ────────────────────────────────────────────────────────────
//
// Assign every non-empty mii to a valid (existing) house+room. Honors the
// existing housing crash trap: only writes house IDs that appear in
// MAP_H_HOUSEMAP. Returns number of miis newly placed.
int AssignHousing(SaveEditor::SavFile& mii, const SaveEditor::SavFile& map);

// ── Random belongings (official Treasure items) ─────────────────────────────
//
// For every valid Mii, randomly fill between minItems and maxItems of their
// 12 belonging slots with hashes drawn from
// treasure_data.h::TREASURE_HASHES. Slots beyond the picked count get
// cleared (StringId=0, UgcGoodsIndex=-1, GetTime=0). No-op if the
// GoodsStringId array doesn't divide evenly by the Mii count.
// Returns number of items written.
int RandomizeMiiBelongings(SaveEditor::SavFile& mii,
                           int minItems = 0, int maxItems = 6,
                           uint32_t seed = 0);

// ── Random room interiors ───────────────────────────────────────────────────
//
// For each Mii with a valid (HouseMapId, RoomIndex), pick a random hash from
// room_style_data.h::ROOM_STYLE_HASHES and write it to the corresponding
// House.RoomSettings.BaseStyleId slot in Map.sav. Layout is 9 room slots per
// house (= 70 × 9 = 630 total), addressed as `(HouseMapId-1) * 9 + RoomIndex`.
// Returns number of rooms styled.
int RandomizeMiiInteriors(SaveEditor::SavFile& map,
                          const SaveEditor::SavFile& mii,
                          uint32_t seed = 0);

// ── Random outfits ──────────────────────────────────────────────────────────
//
// For every valid Mii, pick a random Coordinate from coordinate_data.h
// and write every piece (Tops / Topslong / BottomsA / BottomsB / Shoes /
// Headwear / Accessory / All) into the Mii's Mii.MiiMisc.ClothInfo.*.KeyHash
// arrays. Women-only coordinates are only assigned to Miis whose Gender
// enum hashes to "Female"; Unisex coordinates go to anyone. Returns the
// number of Miis dressed.
int RandomizeMiiOutfits(SaveEditor::SavFile& mii, uint32_t seed = 0);

// ── Roads + decor pass ──────────────────────────────────────────────────────
//
// After SnapActorsToLand has placed every building at a random anchor,
// connect them all with Asphalt road tiles (L-shaped paths from each
// building to a central "hub" building) and repopulate the empty actor
// slots — which SnapActorsToLand cleared to remove the seed's decorative
// clutter — with a sprinkling of trees, lamps, hedges, and flowers on
// nearby land tiles. Intended for "random" map mode only; classic mode
// keeps its hand-crafted layout untouched.
// Returns number of decor actors placed.
int PaintRoadsAndDecor(SaveEditor::SavFile& map, uint32_t seed = 0);

} // namespace IslandGen
