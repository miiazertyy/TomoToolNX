// island_generator.cpp — see include/island_generator.h
//
// All logic stays on the C++ side so both the no-save bootstrap (Phase A) and
// the HTTP endpoints (Phase B) share one implementation. The web UI talks to
// this module exclusively through /api/genisland/* in http_server.cpp.

#include "island_generator.h"
#include "wishes_data.h"
#include "treasure_data.h"
#include "room_style_data.h"
#include "coordinate_data.h"
#include <switch.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

// bin2s-generated headers. Each one declares the bytes pointer + emits the
// size as a constexpr (C++ mode), so we lean on the headers rather than raw
// extern symbols. The Makefile's `-I$(BUILD)` exposes them on the include path.
#include "tpl_classic_bin.h"
#include "tpl_archipelago_bin.h"
#include "tpl_snowy_bin.h"
#include "tpl_cherry_bin.h"

namespace IslandGen {

// ── Field hashes (mirror the JS web-UI constants) ───────────────────────────
namespace H {
    // Map field names are hashed lazily and cached, since SaveEditor::Hash is
    // a runtime function (the C++ side has no equivalent of the JS-side hex
    // constants for these names).
    static uint32_t FloorHash() {
        static uint32_t v = SaveEditor::Hash("MapGrid.GridX.GridZ.FloorKeyHash");
        return v;
    }
    static uint32_t ActorHash() {
        static uint32_t v = SaveEditor::Hash("MapObject.ActorKey");
        return v;
    }
    // Schema-correct field paths (see ltd-save-editor's map.ts):
    //   MapObject.Location.GridPosX / .GridPosY
    // The earlier "MapObject.PosX"/"PosY" was wrong — those hashes don't
    // exist in the game's Map.sav, so SnapActorsToLand was reading -1 for
    // every house and never wrote a corrected position back. Result:
    // houses stayed where the user placed them, ending up "floating" on
    // water after a random map regen.
    static uint32_t ActorPosXHash() {
        static uint32_t v = SaveEditor::Hash("MapObject.Location.GridPosX");
        return v;
    }
    static uint32_t ActorPosYHash() {
        static uint32_t v = SaveEditor::Hash("MapObject.Location.GridPosY");
        return v;
    }
    static uint32_t HouseMapIdHash() {
        static uint32_t v = SaveEditor::Hash("House.MapId");
        return v;
    }

    // Mii relations (same hashes used by the JS social/relations code)
    static constexpr uint32_t REL_IA    = 0xf7420afbu;
    static constexpr uint32_t REL_IB    = 0x4071f71cu;
    static constexpr uint32_t REL_BASE  = 0x8b41897eu;
    static constexpr uint32_t REL_METER = 0x42c2fc2fu;
    static constexpr uint32_t REL_TST   = 0x1a892e50u;

    // Mii housing
    static uint32_t MiiHouseMapId() {
        static uint32_t v = SaveEditor::Hash("Mii.Location.HouseMapId");
        return v;
    }
    static uint32_t MiiRoomIndex() {
        static uint32_t v = SaveEditor::Hash("Mii.Location.RoomIndex");
        return v;
    }
    static uint32_t MiiName() {
        static uint32_t v = SaveEditor::Hash("Mii.Name.Name");
        return v;
    }
}

// Relationship type enum hashes
static constexpr uint32_t REL_UNKNOWN = 0x0784a8dcu;
static constexpr uint32_t REL_FRIEND  = 0xba939a42u;
static constexpr uint32_t REL_COUPLE  = 0xc2d067a7u;
static constexpr uint32_t REL_LOVERS  = 0xb7ce0c18u;
static constexpr uint32_t REL_KNOWS   = 0x354a0515u;
static constexpr uint32_t REL_PARENT  = 0xdcfc7603u;
static constexpr uint32_t REL_CHILD   = 0xe193c5a2u;

// Valid meter values for non-fixed relationships (mirrors NATURAL[] in JS).
static constexpr int NATURAL_METERS[] = { 0, 20, 40, 60, 100, 120, 140 };
static constexpr int NATURAL_COUNT    = sizeof(NATURAL_METERS) / sizeof(int);

// ── Surface themes ──────────────────────────────────────────────────────────
//
// One entry per "Surprise me" surface. Roads are reserved for a future
// spanning-path pass; for the initial walkable-island guarantee we paint a
// uniform surface and rely on the cellular automaton to keep the land mass
// connected (no roads required for the game to consider tiles walkable).

static const SurfaceTheme kThemes[] = {
    { "grass",         0xff4ae68au, 0x2ef21057u },
    { "sand",          0x122a7d23u, 0x2b9b8582u },
    { "snow",          0x47f627bdu, 0xe9473287u },
    { "cherry",        0x54ae7e98u, 0xd1b37f49u },
    { "clover",        0xa27341edu, 0xafa5b5abu },
    { "fallenleaves",  0x8a58eb7du, 0x923cfbd7u },
    { "pebble",        0xa4afd856u, 0xca11e25au },
    { "cobblestone",   0xb019eff9u, 0xa442959eu },
};
static constexpr int kThemeCount = sizeof(kThemes) / sizeof(SurfaceTheme);

const SurfaceTheme* AllSurfaceThemes(int* outCount) {
    if (outCount) *outCount = kThemeCount;
    return kThemes;
}

const SurfaceTheme* SurfaceThemeById(const char* id) {
    if (!id) return nullptr;
    for (int i = 0; i < kThemeCount; i++) {
        if (std::strcmp(kThemes[i].id, id) == 0) return &kThemes[i];
    }
    return nullptr;
}

// ── Map templates ───────────────────────────────────────────────────────────

static const MapTemplate kTemplates[] = {
    { "classic",      "Classic Island",       "Round central island, beaches, a single main road.",
      tpl_classic_bin,      tpl_classic_bin_size },
    { "archipelago",  "Archipelago",          "Several small islands linked by sand bars.",
      tpl_archipelago_bin,  tpl_archipelago_bin_size },
    { "snowy",        "Snowy Peaks",          "Snowy land with a frozen lake in the middle.",
      tpl_snowy_bin,        tpl_snowy_bin_size },
    { "cherry",       "Cherry Blossom Park",  "Pink groves around a winding river.",
      tpl_cherry_bin,       tpl_cherry_bin_size },
};
static constexpr int kTemplateCount = sizeof(kTemplates) / sizeof(MapTemplate);

const MapTemplate* AllMapTemplates(int* outCount) {
    if (outCount) *outCount = kTemplateCount;
    return kTemplates;
}

const MapTemplate* MapTemplateById(const char* id) {
    if (!id) return nullptr;
    for (int i = 0; i < kTemplateCount; i++) {
        if (std::strcmp(kTemplates[i].id, id) == 0) return &kTemplates[i];
    }
    return nullptr;
}

// ── Tiny xorshift32 PRNG so generation is deterministic given the same seed
static inline uint32_t xorshift32(uint32_t& state) {
    uint32_t x = state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return state = x;
}

// ── Generic mutable-entry helpers (used by both wish-rebuild and rel-resize)

// Locate an entry by hash, mutable. SaveEditor::Entry is public so we mutate
// the payload bytes directly when SaveEditor's index-bounded setters won't
// grow arrays.
static SaveEditor::Entry* FindEntryMut(SaveEditor::SavFile& s, uint32_t hash) {
    for (auto& e : s.entries) if (e.hash == hash) return &e;
    return nullptr;
}

// Rewrite a UIntArray entry's payload from a fresh value list. Layout:
// [count:u32][values:u32...].
static void RewriteUIntArray(SaveEditor::SavFile& s, uint32_t hash,
                             const std::vector<uint32_t>& vals) {
    SaveEditor::Entry* e = FindEntryMut(s, hash);
    if (!e) return;
    e->type = SaveEditor::DT_UIntArray;
    e->payload.assign(4 + vals.size() * 4, 0);
    uint32_t cnt = (uint32_t)vals.size();
    e->payload[0] = (uint8_t)(cnt      );
    e->payload[1] = (uint8_t)(cnt >>  8);
    e->payload[2] = (uint8_t)(cnt >> 16);
    e->payload[3] = (uint8_t)(cnt >> 24);
    for (size_t i = 0; i < vals.size(); i++) {
        uint32_t v = vals[i];
        size_t o = 4 + i * 4;
        e->payload[o + 0] = (uint8_t)(v      );
        e->payload[o + 1] = (uint8_t)(v >>  8);
        e->payload[o + 2] = (uint8_t)(v >> 16);
        e->payload[o + 3] = (uint8_t)(v >> 24);
    }
}

// Rewrite a BoolArray entry. Payload is [count:u32][bytes...] where bit
// (i & 7) of byte (i >> 3) holds bool i. Byte region is aligned to 4 bytes.
static void RewriteBoolArray(SaveEditor::SavFile& s, uint32_t hash,
                             const std::vector<bool>& vals) {
    SaveEditor::Entry* e = FindEntryMut(s, hash);
    if (!e) return;
    e->type = SaveEditor::DT_BoolArray;
    size_t byteCount = (vals.size() + 7) / 8;
    size_t aligned   = (byteCount + 3) & ~size_t(3);
    e->payload.assign(4 + aligned, 0);
    uint32_t cnt = (uint32_t)vals.size();
    e->payload[0] = (uint8_t)(cnt      );
    e->payload[1] = (uint8_t)(cnt >>  8);
    e->payload[2] = (uint8_t)(cnt >> 16);
    e->payload[3] = (uint8_t)(cnt >> 24);
    for (size_t i = 0; i < vals.size(); i++) {
        if (!vals[i]) continue;
        e->payload[4 + (i >> 3)] |= (uint8_t)(1u << (i & 7u));
    }
}

// ── Tile array helpers ──────────────────────────────────────────────────────
//
// The floor entry is a UIntArray (DT_UIntArray = 21) of MAP_W*MAP_H elements.
// SaveEditor's SetUIntAt could write them one-by-one but burning ~9600 calls
// is wasteful — we mutate the entry payload bytes directly instead. Layout:
//   [uint32_t count] [uint32_t tileHash * count]
// Index encoding mirrors the JS map renderer: idx = x * MAP_H + y.

static SaveEditor::Entry* FindFloorEntry(SaveEditor::SavFile& s) {
    for (auto& e : s.entries) {
        if (e.hash == H::FloorHash()) return &e;
    }
    return nullptr;
}

static const SaveEditor::Entry* FindFloorEntry(const SaveEditor::SavFile& s) {
    for (auto& e : s.entries) {
        if (e.hash == H::FloorHash()) return &e;
    }
    return nullptr;
}

static inline void SetTileRaw(uint8_t* buf, int idx, uint32_t hash) {
    // Little-endian u32 store, no alignment assumptions.
    buf[idx*4 + 0] = (uint8_t)(hash      );
    buf[idx*4 + 1] = (uint8_t)(hash >>  8);
    buf[idx*4 + 2] = (uint8_t)(hash >> 16);
    buf[idx*4 + 3] = (uint8_t)(hash >> 24);
}

static inline uint32_t GetTileRaw(const uint8_t* buf, int idx) {
    return (uint32_t)buf[idx*4 + 0]
         | ((uint32_t)buf[idx*4 + 1] <<  8)
         | ((uint32_t)buf[idx*4 + 2] << 16)
         | ((uint32_t)buf[idx*4 + 3] << 24);
}

// ── Map generation ──────────────────────────────────────────────────────────

std::string GenerateRandomMap(SaveEditor::SavFile& map,
                              const SurfaceTheme& theme,
                              uint32_t seed) {
    SaveEditor::Entry* fe = FindFloorEntry(map);
    if (!fe) return "Map.sav has no floor entry";
    if (fe->payload.size() < 4 + (size_t)MAP_TILES * 4)
        return "Map.sav floor entry is too short";

    if (seed == 0) seed = (uint32_t)std::time(nullptr) ^ 0x9E3779B9u;
    if (seed == 0) seed = 1; // xorshift32 must never start at zero

    // ── 1. Random initial land mask (55% land)
    std::vector<uint8_t> a(MAP_TILES, 0);
    std::vector<uint8_t> b(MAP_TILES, 0);
    for (int i = 0; i < MAP_TILES; i++) {
        a[i] = (xorshift32(seed) % 100) < 55 ? 1 : 0;
    }
    // Force the rectangular border to water so generated islands never run
    // off the edge of the map.
    for (int x = 0; x < MAP_W; x++) {
        a[x * MAP_H + 0] = 0;
        a[x * MAP_H + (MAP_H - 1)] = 0;
    }
    for (int y = 0; y < MAP_H; y++) {
        a[0 * MAP_H + y] = 0;
        a[(MAP_W - 1) * MAP_H + y] = 0;
    }

    // ── 2. Four cellular-automaton smoothing passes (5/3 rule).
    for (int pass = 0; pass < 4; pass++) {
        for (int x = 1; x < MAP_W - 1; x++) {
            for (int y = 1; y < MAP_H - 1; y++) {
                int n = 0;
                for (int dx = -1; dx <= 1; dx++)
                    for (int dy = -1; dy <= 1; dy++) {
                        if (dx == 0 && dy == 0) continue;
                        n += a[(x + dx) * MAP_H + (y + dy)];
                    }
                int idx = x * MAP_H + y;
                if      (n >= 5) b[idx] = 1;
                else if (n <= 3) b[idx] = 0;
                else             b[idx] = a[idx];
            }
        }
        std::swap(a, b);
    }

    // ── 3. Flood-fill to find the largest connected land component.
    std::vector<int> comp(MAP_TILES, -1);
    int bestComp = -1;
    int bestSize = 0;
    int nextComp = 0;
    for (int x0 = 0; x0 < MAP_W; x0++) {
        for (int y0 = 0; y0 < MAP_H; y0++) {
            int idx0 = x0 * MAP_H + y0;
            if (!a[idx0] || comp[idx0] != -1) continue;
            int id = nextComp++;
            int size = 0;
            std::queue<int> q;
            q.push(idx0);
            comp[idx0] = id;
            while (!q.empty()) {
                int cur = q.front(); q.pop();
                size++;
                int cx = cur / MAP_H, cy = cur % MAP_H;
                const int dx4[] = { -1, 1, 0, 0 };
                const int dy4[] = { 0, 0, -1, 1 };
                for (int k = 0; k < 4; k++) {
                    int nx = cx + dx4[k], ny = cy + dy4[k];
                    if (nx < 0 || nx >= MAP_W || ny < 0 || ny >= MAP_H) continue;
                    int ni = nx * MAP_H + ny;
                    if (!a[ni] || comp[ni] != -1) continue;
                    comp[ni] = id;
                    q.push(ni);
                }
            }
            if (size > bestSize) { bestSize = size; bestComp = id; }
        }
    }

    // ── 4. Build the final land mask = "tile is in the largest component".
    std::vector<uint8_t> land(MAP_TILES, 0);
    for (int i = 0; i < MAP_TILES; i++)
        land[i] = (comp[i] == bestComp) ? 1 : 0;

    // ── 5. Fill inland lakes (water pockets fully enclosed by land).
    //      Flood-fill WATER outward from the border; anything not reached
    //      is a hole-in-the-island and gets converted to land. This kills
    //      the "swiss cheese" look the cellular automaton was leaving.
    {
        std::vector<uint8_t> reachedWater(MAP_TILES, 0);
        std::queue<int> q;
        for (int x = 0; x < MAP_W; x++) {
            int top = x * MAP_H + 0;
            int bot = x * MAP_H + (MAP_H - 1);
            if (!land[top]) { reachedWater[top] = 1; q.push(top); }
            if (!land[bot]) { reachedWater[bot] = 1; q.push(bot); }
        }
        for (int y = 0; y < MAP_H; y++) {
            int lt = 0 * MAP_H + y;
            int rt = (MAP_W - 1) * MAP_H + y;
            if (!land[lt]) { reachedWater[lt] = 1; q.push(lt); }
            if (!land[rt]) { reachedWater[rt] = 1; q.push(rt); }
        }
        const int dx4[] = { -1, 1, 0, 0 };
        const int dy4[] = { 0, 0, -1, 1 };
        while (!q.empty()) {
            int cur = q.front(); q.pop();
            int cx = cur / MAP_H, cy = cur % MAP_H;
            for (int k = 0; k < 4; k++) {
                int nx = cx + dx4[k], ny = cy + dy4[k];
                if (nx < 0 || nx >= MAP_W || ny < 0 || ny >= MAP_H) continue;
                int ni = nx * MAP_H + ny;
                if (land[ni] || reachedWater[ni]) continue;
                reachedWater[ni] = 1;
                q.push(ni);
            }
        }
        for (int i = 0; i < MAP_TILES; i++) {
            if (!land[i] && !reachedWater[i]) land[i] = 1; // inland lake → land
        }
    }

    // ── 6. Beach mask: any land tile that's adjacent to water becomes
    //      a sand tile. Gives the island a natural coastline instead of
    //      grass running right up to the waterline.
    constexpr uint32_t TILE_BEACH = 0xb6d76a62u; // Beach (sand)
    std::vector<uint8_t> beach(MAP_TILES, 0);
    {
        const int dx4[] = { -1, 1, 0, 0 };
        const int dy4[] = { 0, 0, -1, 1 };
        for (int x = 0; x < MAP_W; x++) {
            for (int y = 0; y < MAP_H; y++) {
                int i = x * MAP_H + y;
                if (!land[i]) continue;
                for (int k = 0; k < 4; k++) {
                    int nx = x + dx4[k], ny = y + dy4[k];
                    if (nx < 0 || nx >= MAP_W || ny < 0 || ny >= MAP_H) continue;
                    if (!land[nx * MAP_H + ny]) { beach[i] = 1; break; }
                }
            }
        }
    }

    // ── 7. Variant patches: sprinkle clusters of grass-like ground
    //      variants (cherry-petals, clover, fallen leaves) over the
    //      interior so the island isn't one uniform colour. Only ground-
    //      variant tiles are used — no cobblestone / snow / pebble that
    //      would look weird mixed into the base grass. Beach edges and
    //      water are skipped so the coastline stays a clean ring.
    static const uint32_t GROUND_VARIANTS[] = {
        0xa27341edu, // Clover         — green field with clovers
        0x54ae7e98u, // Cherry         — pink petals scattered on ground
        0x8a58eb7du, // FallenLeaves   — orange/red autumn leaves
    };
    constexpr int kVariantCount =
        (int)(sizeof(GROUND_VARIANTS) / sizeof(uint32_t));

    // Final per-tile assignment buffer (so patches can overwrite the base
    // grass before we serialise).
    std::vector<uint32_t> outHash((size_t)MAP_TILES);
    for (int i = 0; i < MAP_TILES; i++) {
        outHash[i] = !land[i] ? TILE_WATER : beach[i] ? TILE_BEACH : theme.land;
    }

    // Patch sprinkler: many centers per island, each painting an irregular
    // blob of one variant tile. Count and radius both bumped from the
    // original "a few cherry meadows" tuning — the user wanted noticeably
    // more clover / cherry / fallen-leaves coverage on the ground, not the
    // sparse hint we had before.
    {
        uint32_t ps = seed ^ 0x9DD16AAFu;
        if (ps == 0) ps = 1;
        const int kPatches = 18 + (int)(xorshift32(ps) % 9u); // 18..26 patches
        for (int p = 0; p < kPatches; p++) {
            int cx = (int)(xorshift32(ps) % (uint32_t)MAP_W);
            int cy = (int)(xorshift32(ps) % (uint32_t)MAP_H);
            int center = cx * MAP_H + cy;
            if (!land[center] || beach[center]) continue;  // grass tile only
            uint32_t variant = GROUND_VARIANTS[
                xorshift32(ps) % (uint32_t)kVariantCount];
            // Patch shape: 3..6 tile radius. Larger than the original 2..4
            // so each blob covers ~3-4× the area; combined with the 3× patch
            // count bump the total variant-ground footprint goes from ~3 %
            // of land to ~25 %.
            int radius = 3 + (int)(xorshift32(ps) % 4u);
            for (int dx = -radius; dx <= radius; dx++) {
                for (int dy = -radius; dy <= radius; dy++) {
                    int nx = cx + dx, ny = cy + dy;
                    if (nx < 0 || nx >= MAP_W || ny < 0 || ny >= MAP_H) continue;
                    int ni = nx * MAP_H + ny;
                    if (!land[ni] || beach[ni]) continue;
                    // Soft-edged blob: closer to center always paints, edges
                    // are randomly dropped so the patch isn't a perfect disc.
                    int sq = dx * dx + dy * dy;
                    if (sq > radius * radius) continue;
                    if (sq >= (radius - 1) * (radius - 1)
                        && (xorshift32(ps) & 1u)) continue;
                    outHash[ni] = variant;
                }
            }
        }
    }

    // ── 8. Write tile hashes from the assignment buffer.
    uint8_t* tiles = fe->payload.data() + 4; // skip the count prefix
    for (int i = 0; i < MAP_TILES; i++) {
        SetTileRaw(tiles, i, outHash[i]);
    }

    return "";
}

std::string ApplyMapTemplate(SaveEditor::SavFile& map,
                             const MapTemplate& tmpl) {
    if (tmpl.size < 1024 || tmpl.bytes == nullptr)
        return std::string("template '") + (tmpl.id ? tmpl.id : "?") + "' has no bundled bytes (rebuild after adding data/tpl_*.bin)";
    SaveEditor::Entry* fe = FindFloorEntry(map);
    if (!fe) return "Map.sav has no floor entry";
    // Strict size match: the template must be exactly the floor payload size,
    // because the tile array is fixed at MAP_W*MAP_H.
    if (tmpl.size != 4 + (size_t)MAP_TILES * 4) {
        char err[160];
        std::snprintf(err, sizeof(err),
                      "template '%s' size %u does not match expected %u",
                      tmpl.id ? tmpl.id : "?", (unsigned)tmpl.size,
                      (unsigned)(4 + MAP_TILES * 4));
        return err;
    }
    fe->payload.assign(tmpl.bytes, tmpl.bytes + tmpl.size);
    return "";
}

// ── Actor footprint table ───────────────────────────────────────────────────
//
// Per-actor occupied-tile metadata, mirroring the WebUI's MAP_ACTOR_INFO
// (http_server.cpp ~line 2628). Only actors whose footprint differs from
// the default 1×1 anchored-at-(0,0) appear here — everything else uses
// the default. Sorted by hash so LookupActorFootprint can binary-search.
//
// The stored grid position is the actor's "goal point" (anchor), not the
// top-left of its bounding box. For houses and facilities, x0 / y0 are
// negative — the building extends backwards from the anchor. SnapActorsToLand
// applies the rotation-aware footprint when checking land/collision so a
// Fountain Park (6×10) or Ferris Wheel (9×5) is no longer snap-placed by
// the old 3×3 proxy that left half of it in water.

struct ActorFootprint {
    uint32_t hash;
    int      x0, y0;
    int      w,  h;
};

// Generated from the JS MAP_ACTOR_INFO table — only non-default entries.
static constexpr ActorFootprint kActorFootprints[] = {
    { 0x020defc1u,   0,   0,  2,  1 }, // ObjGuardrail_04
    { 0x02a4af54u,   0,   0,  3,  1 }, // ObjArchAir_01
    { 0x0a29f456u,   0,   0,  4,  5 }, // ObjSignboardTutorial_04
    { 0x0b113fdeu,   0,   0,  2,  1 }, // ObjGuardrail_01
    { 0x0daa36aau,   0,   0,  4,  5 }, // ObjSignboardTutorial_03
    { 0x0ed12c63u,   0,   0,  2,  1 }, // ObjHedge_05
    { 0x0fc4dae6u,   0,   0,  2,  1 }, // ObjHedge_01
    { 0x143f32d9u,   0,   0,  2,  1 }, // ObjJackOLantern_02
    { 0x1680e2f4u,   0,   0,  2,  1 }, // ObjHedge_03
    { 0x17133c16u,   0,   0,  2,  1 }, // ObjGuardrail_02
    { 0x1835fdabu,   0,   1,  2,  2 }, // ObjTableBench_05
    { 0x18bc0888u,   0,   0,  2,  1 }, // ObjFenceGuardpipe_07
    { 0x1ad1b47du,   0,   0,  2,  1 }, // ObjBenchTerrace_05
    { 0x1b2ff9efu,   0,   0,  2,  1 }, // ObjJackOLantern_03
    { 0x1cc3f3b5u,   0,   0,  2,  1 }, // ObjFenceGuardpipe_04
    { 0x1db7b2ecu,   0,   1,  2,  2 }, // ObjTableBench_03
    { 0x1dd946b9u,   0,   0,  2,  2 }, // ObjLighthouse_02
    { 0x1eb0ff5cu,   0,   0,  2,  1 }, // ObjJackOLantern
    { 0x1f1eb32du,  -2,  -2,  6,  5 }, // FacilityFamilyRestaurant
    { 0x22f85aa9u,  -1,  -1,  4,  4 }, // FacilityItemShop
    { 0x241cba0au,   0,   0,  2,  1 }, // ObjBeachBed_04
    { 0x27d7affau,   0,   0,  2,  1 }, // ObjJackOLantern_05
    { 0x28c866e4u,   0,   0,  3,  1 }, // ObjArchAir_04
    { 0x28f121a9u,   0,   0,  2,  2 }, // ObjLighthouse_05
    { 0x2cd639c2u,   0,   0,  2,  1 }, // ObjBenchTerrace_01
    { 0x2d70949au,   0,   0,  2,  1 }, // ObjBenchHome
    { 0x35042895u,   0,   0,  2,  2 }, // ObjTreeChristmas
    { 0x3527b759u,   0,   0,  2,  1 }, // ObjGuardrail_03
    { 0x36147dc4u,   0,   0,  2,  1 }, // ObjBeachBed
    { 0x38c47000u,   0,   0,  2,  1 }, // ObjFenceGuardpipe_06
    { 0x39bb7d36u,   0,   0,  2,  1 }, // ObjFenceGuardpipe
    { 0x3a19045bu,   0,   0,  2,  1 }, // ObjHedge_02
    { 0x3a379430u,   0,   0,  2,  1 }, // ObjBenchPark
    { 0x3b21e48eu,   0,   0,  2,  1 }, // ObjBenchTerrace_07
    { 0x3e2ae3d8u,   0,   1,  2,  2 }, // ObjTableBench_04
    { 0x3f360d90u,   0,   0,  2,  1 }, // ObjGuardrail_05
    { 0x3f612d44u,   0,   0,  3,  1 }, // ObjArchAir_07
    { 0x408ec56cu,   0,   0,  2,  1 }, // ObjSeesaw_04
    { 0x43f76680u,   0,   0,  2,  1 }, // ObjFenceGuardpipe_03
    { 0x4499cc8cu,  -1,  -1,  4,  4 }, // FacilityPhotoStudio
    { 0x48870a72u,   0,   0,  2,  1 }, // ObjSeesaw_02
    { 0x48e1e211u,   0,   0,  2,  1 }, // ObjHedge
    { 0x49893c7bu,   0,   0,  3,  1 }, // ObjArchAir_03
    { 0x4d590b0fu,   0,   0,  2,  1 }, // ObjGuardrail_06
    { 0x4e992963u,  -1,  -1,  4,  4 }, // FacilityTower
    { 0x4f2a4a2cu,  -1,  -1,  3,  3 }, // FacilityMarket
    { 0x51456fc7u,   0,   0,  2,  1 }, // ObjBenchTerrace_02
    { 0x53a5e5b4u,   0,   0,  2,  1 }, // ObjBeachBed_01
    { 0x53cdc538u,   0,   1,  2,  2 }, // ObjTableBench_07
    { 0x5468ea41u,   0,   0,  2,  1 }, // ObjBenchTerrace
    { 0x54b486c1u,   0,   0,  2,  1 }, // ObjBenchHome_04
    { 0x5bc17e5au,   0,   0,  2,  1 }, // ObjHedge_06
    { 0x5c80d8f9u,   0,   0,  2,  1 }, // ObjJackOLantern_04
    { 0x5e545d46u,   0,   0,  2,  2 }, // ObjLighthouse_06
    { 0x5ea082c6u,   0,   0,  2,  1 }, // ObjHedge_04
    { 0x60974f1du,   0,   0,  2,  1 }, // ObjBenchPark_03
    { 0x623f9384u,   0,   0,  4,  5 }, // ObjSignboardTutorial
    { 0x631a5eddu,   0,   0,  2,  1 }, // ObjBeachBed_07
    { 0x639739e4u,  -1,  -1,  4,  4 }, // FacilitySupermarket
    { 0x6400ef93u,  -1,  -1,  4,  4 }, // FacilityInteriorShop
    { 0x6dfa5726u,   0,   0,  2,  1 }, // ObjBenchPark_07
    { 0x6e0f5ddcu,   0,   0,  2,  1 }, // ObjFenceGuardpipe_02
    { 0x6f96359eu,   0,   0,  2,  2 }, // ObjLighthouse_07
    { 0x738bd7a2u,  -4,  -2,  9,  5 }, // FacilityFerrisWheel
    { 0x739bb804u,   0,   0,  2,  1 }, // ObjBenchPark_01
    { 0x779b5f66u,  -2,  -4,  6, 10 }, // FacilityFountainPark
    { 0x7a3313bdu,   0,   0,  2,  2 }, // ObjLighthouse_04
    { 0x7bd6ecbeu,   0,   0,  2,  1 }, // ObjSeesaw_05
    { 0x7cb5537au,   0,   0,  4,  3 }, // FacilityFountain
    { 0x7ff35b85u,  -1,  -1,  4,  4 }, // FacilityClothShop
    { 0x81d8840du,   0,   0,  2,  1 }, // ObjSeesaw_06
    { 0x8260ac6du,   0,   0,  2,  1 }, // ObjBenchHome_02
    { 0x8653643du,   0,   0,  4,  5 }, // ObjSignboardTutorial_02
    { 0x86bf727cu,   0,   0,  2,  1 }, // ObjBeachBed_06
    { 0x8ca1ca2au,   0,   0,  2,  1 }, // ObjFenceGuardpipe_05
    { 0x93acc870u,   0,   0,  3,  1 }, // ObjArchAir_05
    { 0x93bfa683u,   0,   1,  2,  2 }, // ObjTableBench_01
    { 0x94046edfu,   0,   1,  2,  2 }, // ObjTableBench
    { 0x9737f2aeu,   0,   1,  2,  2 }, // ObjTableBench_02
    { 0xa14c1b16u,   0,   0,  3,  1 }, // ObjArchAir_02
    { 0xa5584e67u,   0,   0,  2,  2 }, // ObjLighthouse_01
    { 0xa5e069e0u,   0,   0,  3,  1 }, // ObjArchAir
    { 0xab8d53d0u,   0,   0,  2,  2 }, // ObjLighthouse
    { 0xacbce196u,   0,   0,  2,  1 }, // ObjBeachBed_05
    { 0xaf35c3deu,   0,   1,  2,  2 }, // ObjTableBench_06
    { 0xb06856c3u,   0,   0,  2,  1 }, // ObjBenchHome_01
    { 0xb3f08ea7u,  -1,  -1,  4,  4 }, // FacilityAtelier
    { 0xb4d25280u,   0,   0,  2,  1 }, // ObjBenchPark_05
    { 0xb5d0afa9u,   0,   0,  6, 10 }, // FacilityPark
    { 0xb7387ec4u,   0,   0,  2,  1 }, // ObjJackOLantern_01
    { 0xbf72663fu,   0,   0,  2,  1 }, // ObjBenchTerrace_03
    { 0xc4fb9bfau,   0,   0,  2,  1 }, // ObjBenchHome_03
    { 0xc6221de3u,   0,   0,  3,  1 }, // ObjArchAir_06
    { 0xc8d08275u,   0,   0,  2,  1 }, // ObjBeachBed_02
    { 0xc8eca18du,   0,   0,  2,  1 }, // ObjBenchTerrace_06
    { 0xc9e023efu,   0,   0,  2,  1 }, // ObjSeesaw
    { 0xcb84668fu,  -1,  -1,  4,  4 }, // FacilityBuildingShop
    { 0xce15d990u,   0,   0,  2,  1 }, // ObjBenchHome_06
    { 0xd8d4c1dcu,   0,   0,  2,  1 }, // ObjBeachBed_03
    { 0xd9d34019u,   0,   0,  2,  1 }, // ObjSeesaw_03
    { 0xdb31941cu,   0,   0,  2,  1 }, // ObjBenchHome_07
    { 0xdc6fbb0eu,   0,   0,  2,  1 }, // ObjJackOLantern_06
    { 0xe2123edeu,   0,   0,  2,  1 }, // ObjBenchPark_02
    { 0xe3ec5c38u,  -2,  -1,  6,  4 }, // HouseDollHouse
    { 0xe4678701u,   0,   0,  2,  1 }, // ObjBenchHome_05
    { 0xe8366039u,   0,   0,  2,  1 }, // ObjBenchPark_06
    { 0xe8c2afb9u,   0,   0,  2,  1 }, // ObjBenchTerrace_04
    { 0xef367adau,  -1,  -1,  3,  4 }, // HouseOneRoom
    { 0xf003e9c0u,  -1,  -1,  4,  4 }, // FacilityPawnShop
    { 0xf045992au,   0,   0,  2,  1 }, // ObjJackOLantern_07
    { 0xf4fac611u,   0,   0,  4,  5 }, // ObjSignboardTutorial_01
    { 0xf8ace4c7u,   0,   0,  2,  1 }, // ObjBenchPark_04
    { 0xfcc93164u,   0,   0,  2,  1 }, // ObjFenceGuardpipe_01
    { 0xfd0ccb79u,   0,   0,  2,  2 }, // ObjLighthouse_03
    { 0xfeb03bcbu,   0,   0,  2,  1 }, // ObjGuardrail
    { 0xffbee148u,   0,   0,  2,  1 }, // ObjSeesaw_01
};
static constexpr int kActorFootprintCount =
    sizeof(kActorFootprints) / sizeof(ActorFootprint);

// Default footprint for any actor not in the table — anchored 1×1 tile.
static ActorFootprint LookupActorFootprint(uint32_t hash) {
    int lo = 0, hi = kActorFootprintCount - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t m = kActorFootprints[mid].hash;
        if (m == hash) return kActorFootprints[mid];
        if (m < hash) lo = mid + 1; else hi = mid - 1;
    }
    return ActorFootprint{ hash, 0, 0, 1, 1 };
}

// Actor hashes that count as "buildings" (Facility* + every House* variant).
// Everything else — street lamps, benches, hedges, trees, signboards, etc. —
// is treated as an item and gets removed from the random-generated island so
// the snapper only has to find homes for the ~43 things that actually matter.
// Sorted for binary-search lookup.
static constexpr uint32_t kBuildingHashes[] = {
    0x093937e7u, // HouseUgc10_00
    0x14025ceeu, // HouseUgc01_00
    0x1f1eb32du, // FacilityFamilyRestaurant
    0x22f85aa9u, // FacilityItemShop
    0x29e42a1cu, // HouseUgc12_00
    0x2f4f8d48u, // HouseUgc04_00
    0x3207d786u, // HouseUgc14_02
    0x35164cbfu, // HouseUgc13_02
    0x42d3a844u, // HouseUgc03_00
    0x4499cc8cu, // FacilityPhotoStudio
    0x46db694fu, // HouseUgc15_02
    0x4e992963u, // FacilityTower
    0x4eac4d85u, // HouseUgc08_00
    0x4f2a4a2cu, // FacilityMarket
    0x513263f6u, // HouseUgc05_01
    0x56a34205u, // HouseUgc01_01
    0x5cf467dfu, // HouseUgc09_00
    0x637a6fb4u, // HouseUgc05_02
    0x639739e4u, // FacilitySupermarket
    0x6400ef93u, // FacilityInteriorShop
    0x66ec608eu, // HouseUgc14_01
    0x6eeac56cu, // HouseUgc04_01
    0x738bd7a2u, // FacilityFerrisWheel
    0x779b5f66u, // FacilityFountainPark
    0x79d748bcu, // HouseUgc06_02
    0x7cb5537au, // FacilityFountain
    0x7ff35b85u, // FacilityClothShop
    0x871c7babu, // HouseUgc11_00
    0xa25e6f94u, // HouseUgc10_01
    0xaca6a414u, // HouseUgc06_01
    0xb2c7da0fu, // HouseUgc02_02
    0xb3f08ea7u, // FacilityAtelier
    0xb5d0afa9u, // FacilityPark
    0xcb84668fu, // FacilityBuildingShop
    0xccd75542u, // HouseUgc03_01
    0xd4fd4783u, // HouseUgc08_01
    0xd7b7c769u, // HouseUgc02_01
    0xde8fe6dbu, // HouseUgc11_01
    0xe32c6920u, // HouseUgc15_00
    0xe3ec5c38u, // HouseDollHouse
    0xef367adau, // HouseOneRoom
    0xf003e9c0u, // FacilityPawnShop
    0xf14d2e67u, // HouseUgc13_00
};
static constexpr int kBuildingHashCount =
    sizeof(kBuildingHashes) / sizeof(uint32_t);

static bool IsBuildingHash(uint32_t hash) {
    int lo = 0, hi = kBuildingHashCount - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t m = kBuildingHashes[mid];
        if (m == hash) return true;
        if (m < hash) lo = mid + 1; else hi = mid - 1;
    }
    return false;
}

// Mirrors mapActorRect in http_server.cpp: rotate the (x0,y0,w,h)
// rectangle by `rotDeg` (nearest 90°) and return the rotated bounding
// box. We do the corner rotation explicitly so the math matches what
// the WebUI uses to draw the same actor.
static ActorFootprint RotateFootprint(ActorFootprint fp, float rotDeg) {
    int t = ((int)std::lround(rotDeg / 90.0f) % 4 + 4) % 4;
    if (t == 0) return fp;
    int x0 = fp.x0, y0 = fp.y0, x1 = fp.x0 + fp.w - 1, y1 = fp.y0 + fp.h - 1;
    int cx[4] = { x0, x1, x0, x1 };
    int cy[4] = { y0, y0, y1, y1 };
    int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
    for (int i = 0; i < 4; i++) {
        int x = cx[i], y = cy[i];
        for (int r = 0; r < t; r++) { int nx = y; int ny = -x; x = nx; y = ny; }
        if (x < minX) minX = x;
        if (y < minY) minY = y;
        if (x > maxX) maxX = x;
        if (y > maxY) maxY = y;
    }
    return ActorFootprint{ fp.hash, minX, minY, maxX - minX + 1, maxY - minY + 1 };
}

// ── Snap actors to land ─────────────────────────────────────────────────────
//
// Two-phase: collect every actor's anchor + rotated footprint, sort by area
// descending (so big buildings claim their plot before street lamps fill in
// the cracks), then for each actor check whether (a) every tile under its
// footprint is land, AND (b) none of those tiles are already occupied by an
// earlier-placed actor. If both pass, the actor stays put and we mark its
// tiles occupied; otherwise we BFS outward from the anchor for the first
// position where (a) and (b) both hold.
//
// This replaces an earlier single-tile / 3×3-proxy snapper that left big
// facilities (Fountain Park 6×10, Ferris Wheel 9×5, Doll House 6×4, etc.)
// half-submerged because their bounding boxes are much larger than 3×3.

int SnapActorsToLand(SaveEditor::SavFile& map) {
    const SaveEditor::Entry* fe = FindFloorEntry(map);
    if (!fe || fe->payload.size() < 4 + (size_t)MAP_TILES * 4) return 0;
    const uint8_t* tiles = fe->payload.data() + 4;

    auto isLand = [&](int x, int y) -> bool {
        if (x < 0 || x >= MAP_W || y < 0 || y >= MAP_H) return false;
        return GetTileRaw(tiles, x * MAP_H + y) != TILE_WATER;
    };

    // Occupancy grid — one bit per tile, set when an earlier actor's
    // footprint claims that tile. Prevents two big buildings overlapping
    // (which the snapper would otherwise happily do — both anchors would
    // independently find the same patch of land).
    std::vector<uint8_t> occupied((size_t)MAP_W * MAP_H, 0);
    auto occGet = [&](int x, int y) -> bool {
        if (x < 0 || x >= MAP_W || y < 0 || y >= MAP_H) return true; // OOB = blocked
        return occupied[(size_t)x * MAP_H + y] != 0;
    };
    auto occSet = [&](int x, int y) {
        if (x < 0 || x >= MAP_W || y < 0 || y >= MAP_H) return;
        occupied[(size_t)x * MAP_H + y] = 1;
    };

    // Can the actor with rotated footprint `fp` sit with its anchor at
    // (ax, ay)? Every covered tile must be land AND not already occupied,
    // AND the anchor itself must be a valid in-bounds map coordinate.
    //
    // The anchor check matters because some actors (e.g. ObjTableBench
    // has x0=0, y0=1) have an anchor that sits OUTSIDE their footprint —
    // the footprint can pass at (ax, -1) since the +1 offset puts the
    // tiles back into y >= 0, but writing y=-1 to the save's position
    // array breaks the game's loader (it treats out-of-range slots as
    // corrupt rather than as "skip"). Some game logic also presumably
    // walks Miis to the anchor tile, which has to exist on the grid.
    auto footprintOK = [&](const ActorFootprint& fp, int ax, int ay) -> bool {
        if (ax < 0 || ax >= MAP_W || ay < 0 || ay >= MAP_H) return false;
        for (int dx = 0; dx < fp.w; dx++)
            for (int dy = 0; dy < fp.h; dy++) {
                int tx = ax + fp.x0 + dx;
                int ty = ay + fp.y0 + dy;
                if (!isLand(tx, ty)) return false;
                if (occGet(tx, ty))   return false;
            }
        return true;
    };
    auto claim = [&](const ActorFootprint& fp, int ax, int ay) {
        for (int dx = 0; dx < fp.w; dx++)
            for (int dy = 0; dy < fp.h; dy++)
                occSet(ax + fp.x0 + dx, ay + fp.y0 + dy);
    };

    int actorCount = SaveEditor::ArraySize(map, H::ActorHash());

    struct PlacementJob {
        int             idx;
        uint32_t        hash;
        int             x, y;
        float           origRot;        // seed rotation — fallback if random rot can't be placed
        float           rot;            // current (possibly rotated) rotation
        ActorFootprint  baseFp;         // unrotated footprint from the table
        ActorFootprint  rotFp;          // baseFp rotated by `rot`
        int             area;
    };
    std::vector<PlacementJob> jobs;
    jobs.reserve((size_t)actorCount);

    uint32_t hRotY = SaveEditor::Hash("MapObject.MapObjectMisc.RotY");
    for (int i = 0; i < actorCount; i++) {
        uint32_t a = SaveEditor::GetUIntAt(map, H::ActorHash(), i, 0u);
        if (a == 0) continue;
        int x = SaveEditor::GetIntAt(map, H::ActorPosXHash(), i, -1);
        int y = SaveEditor::GetIntAt(map, H::ActorPosYHash(), i, -1);
        // Sentinels for "actor slot allocated but not placed". Snapping
        // these would invent a placement.
        if (x < 0 || y < 0 || x >= MAP_W || y >= MAP_H) continue;
        // Random-island filter: keep only Houses + Facilities. Street
        // lamps, benches, hedges, trees, signboards, etc. get deleted
        // (ActorKey = 0 tells the game / WebUI to skip that slot). This
        // both keeps the island visually clean and cuts the snapper's
        // workload — only ~30-40 actors need real placement instead of
        // hundreds of decorative items that were prone to snap-conflicts.
        if (!IsBuildingHash(a)) {
            SaveEditor::SetUIntAt(map, H::ActorHash(), i, 0u);
            continue;
        }
        // Roll a random 90° rotation so the island doesn't look like the
        // same 100% layout every time. Seeded off the actor index + tick.
        // We DO NOT write the rotation to the save yet — if the snapper
        // can't place this actor at any land tile with the new rotation,
        // we revert to the original seed rotation later (otherwise we end
        // up with a building visually extending into water at its anchor).
        float origRot = SaveEditor::GetFloatAt(map, hRotY, i, 0.0f);
        uint32_t rotSeed = (uint32_t)i ^ (uint32_t)armGetSystemTick();
        if (rotSeed == 0) rotSeed = 1;
        float rot = (float)((xorshift32(rotSeed) & 3u) * 90u);
        ActorFootprint baseFp = LookupActorFootprint(a);
        ActorFootprint fp     = RotateFootprint(baseFp, rot);
        PlacementJob j{ i, a, x, y, origRot, rot, baseFp, fp, fp.w * fp.h };
        jobs.push_back(j);
    }

    // Buildings (big footprints) first, so they claim their plots before
    // the 1×1 street lamps / hedges / trees fill in around them.
    std::stable_sort(jobs.begin(), jobs.end(),
                     [](const PlacementJob& A, const PlacementJob& B) {
                         return A.area > B.area;
                     });

    // Reusable BFS — finds the nearest (ax, ay) where `fp` fully fits on
    // land and doesn't overlap any earlier-claimed tile. Returns true and
    // fills outX/outY on success.
    auto bfs = [&](const ActorFootprint& fp, int startX, int startY,
                   int& outX, int& outY) -> bool {
        outX = -1; outY = -1;
        if (footprintOK(fp, startX, startY)) {
            outX = startX; outY = startY; return true;
        }
        const int maxR = MAP_W + MAP_H;
        for (int r = 1; r < maxR && outX < 0; r++) {
            for (int dx = -r; dx <= r && outX < 0; dx++) {
                int dy = r - std::abs(dx);
                auto try_ = [&](int nx, int ny) -> bool {
                    if (footprintOK(fp, nx, ny)) {
                        outX = nx; outY = ny; return true;
                    }
                    return false;
                };
                if (try_(startX + dx, startY + dy)) break;
                if (dy != 0 && try_(startX + dx, startY - dy)) break;
            }
        }
        return outX >= 0;
    };

    // Random-anchor search: roll up to N random (x, y) candidates and
    // accept the first one that's a clean placement. This is what gives
    // each generation a different layout — the old "if seed position
    // works, keep it" path left every building at its 100%-save spot.
    // If rejection sampling fails, fall back to a BFS from a fresh random
    // start so we at least get *something* placed even on tight maps.
    //
    // We additionally enforce a minimum Manhattan distance between every
    // pair of placed building anchors so the island doesn't end up with
    // houses cheek-to-jowl in a clump. The cap relaxes if the rejection
    // sampling can't find a wider spot — we don't want the map's smaller
    // buildings to give up entirely on cramped islands.
    uint32_t snapSeed = (uint32_t)armGetSystemTick() ^ 0xC0DEFACEu;
    if (snapSeed == 0) snapSeed = 1;
    std::vector<std::pair<int,int>> placedAnchors;
    placedAnchors.reserve(jobs.size());
    auto farEnough = [&](int x, int y, int minDist) -> bool {
        for (auto& a : placedAnchors) {
            if (std::abs(a.first - x) + std::abs(a.second - y) < minDist) return false;
        }
        return true;
    };
    auto randomPlace = [&](const ActorFootprint& fp,
                           int minDist,
                           int& outX, int& outY) -> bool {
        constexpr int kTries = 200;
        for (int t = 0; t < kTries; t++) {
            int rx = (int)(xorshift32(snapSeed) % (uint32_t)MAP_W);
            int ry = (int)(xorshift32(snapSeed) % (uint32_t)MAP_H);
            if (!footprintOK(fp, rx, ry)) continue;
            if (!farEnough(rx, ry, minDist)) continue;
            outX = rx; outY = ry; return true;
        }
        // Random rejection sampling failed → BFS from a random anchor
        // (ignoring spacing — better cramped than missing).
        int rx = (int)(xorshift32(snapSeed) % (uint32_t)MAP_W);
        int ry = (int)(xorshift32(snapSeed) % (uint32_t)MAP_H);
        return bfs(fp, rx, ry, outX, outY);
    };

    // Minimum Manhattan distance between building anchors. 9 tiles is
    // enough that a 6-wide Fountain Park and a 4-wide Item Shop placed
    // adjacent still have ~3 tiles of road / decor between them. We
    // shrink this for smaller buildings (low-area footprints don't need
    // as much breathing room as a 9×5 Ferris Wheel).
    int moved = 0;
    for (auto& j : jobs) {
        // Larger footprints get a wider spacing requirement; tiny 1×1
        // UGC houses just need 4 tiles to not clip into each other.
        int spacing = (j.area >= 25) ? 10
                    : (j.area >=  9) ? 8
                                     : 5;
        int bestX = -1, bestY = -1;
        ActorFootprint chosenFp = j.rotFp;
        float          chosenRot = j.rot;
        bool placed = randomPlace(j.rotFp, spacing, bestX, bestY);
        if (!placed) {
            ActorFootprint origFp = RotateFootprint(j.baseFp, j.origRot);
            if (randomPlace(origFp, spacing, bestX, bestY)) {
                chosenFp  = origFp;
                chosenRot = j.origRot;
                placed    = true;
            }
        }
        if (!placed) {
            // Truly nowhere — leave the actor at its seed pose. Better an
            // ugly placement than a missing building.
            continue;
        }
        SaveEditor::SetFloatAt(map, hRotY, j.idx, chosenRot);
        SaveEditor::SetIntAt(map, H::ActorPosXHash(), j.idx, bestX);
        SaveEditor::SetIntAt(map, H::ActorPosYHash(), j.idx, bestY);
        claim(chosenFp, bestX, bestY);
        placedAnchors.emplace_back(bestX, bestY);
        if (bestX != j.x || bestY != j.y) moved++;
    }
    return moved;
}

// ── Mii roster enumeration ──────────────────────────────────────────────────

static std::vector<int> CollectValidMiiSlots(const SaveEditor::SavFile& mii) {
    std::vector<int> out;
    const uint32_t hName = H::MiiName();
    int n = SaveEditor::ArraySize(mii, hName);
    for (int i = 0; i < n; i++) {
        std::string nm = SaveEditor::GetWStr32At(mii, hName, i);
        if (!nm.empty()) out.push_back(i);
    }
    return out;
}

// ── Dense random relationships ──────────────────────────────────────────────
//
// The pair table is a fixed-shape array (~C(70,2)=2415 slots). Each slot `i`
// holds: IA[i], IB[i], BASE[i*2], BASE[i*2+1], METER[i*2], METER[i*2+1],
// TST[i]. We iterate through every slot and either: (a) repurpose it for the
// (slotA, slotB) pair we want to write, or (b) leave it as Unknown. Same shape
// the JS code uses — we just write to every reachable slot so n-1 connections
// exist per mii.

// Resize the relationship pair-table arrays so they can hold `pairCount`
// pairs. Required because SaveEditor's SetIntAt/SetEnumAt won't grow arrays —
// a freshly-seeded save with no prior pairs may have a smaller pair table than
// we need for n*(n-1)/2 dense entries. Mirrors the JS web-UI's pattern of
// "rewrite the whole array payload at once".
static void ResizeRelTable(SaveEditor::SavFile& mii, int pairCount) {
    // IA, IB, TST are 1 entry per pair.
    auto resizeOne = [&](uint32_t hash, SaveEditor::DataType ty,
                         size_t bytesPerEntry, size_t entries) {
        SaveEditor::Entry* e = FindEntryMut(mii, hash);
        if (!e) return;
        e->type = ty;
        size_t old = e->payload.size();
        size_t want = 4 + entries * bytesPerEntry;
        if (want <= old) return;  // never shrink
        uint32_t cnt = (uint32_t)entries;
        e->payload.assign(want, 0);
        e->payload[0] = (uint8_t)(cnt      );
        e->payload[1] = (uint8_t)(cnt >>  8);
        e->payload[2] = (uint8_t)(cnt >> 16);
        e->payload[3] = (uint8_t)(cnt >> 24);
    };
    resizeOne(H::REL_IA,    SaveEditor::DT_IntArray,    4, (size_t)pairCount);
    resizeOne(H::REL_IB,    SaveEditor::DT_IntArray,    4, (size_t)pairCount);
    resizeOne(H::REL_BASE,  SaveEditor::DT_EnumArray,   4, (size_t)pairCount * 2);
    resizeOne(H::REL_METER, SaveEditor::DT_IntArray,    4, (size_t)pairCount * 2);
    resizeOne(H::REL_TST,   SaveEditor::DT_UInt64Array, 8, (size_t)pairCount);
}

int WriteDenseRelationships(SaveEditor::SavFile& mii, uint32_t seed) {
    if (seed == 0) seed = (uint32_t)std::time(nullptr) ^ 0xDEADBEEFu;
    if (seed == 0) seed = 1;

    std::vector<int> slots = CollectValidMiiSlots(mii);
    int n = (int)slots.size();
    if (n < 2) return 0;

    // Ensure the pair table can hold n*(n-1)/2 entries even on a save where
    // the pair-table arrays were never populated. The game's canonical size
    // for a 70-mii island is 2415 (= C(70,2)), so we never shrink below that
    // either — ResizeRelTable only grows.
    int wantedPairs = std::max(n * (n - 1) / 2, 2415);
    ResizeRelTable(mii, wantedPairs);

    const int pc = SaveEditor::ArraySize(mii, H::REL_IA);
    if (pc <= 0) return 0;

    // Build the list of unique pairs we want to express.
    struct Pair { int a, b; };
    std::vector<Pair> wanted;
    wanted.reserve((size_t)n * (n - 1) / 2);
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            wanted.push_back({ slots[i], slots[j] });

    // Type pool: Knows ~70%, Friend ~30%. COUPLE and LOVERS are excluded
    // here because they have implicit uniqueness rules in-game — each Mii
    // can only have ONE active Couple (marriage) and presumably one Lover.
    // A uniform random sample across 2415 pairs gave many Miis multiple
    // couple/lover relationships, which made the game refuse to load the
    // save. Knows/Friend have no such uniqueness constraint and are safe
    // to assign densely.
    // Parent / Child are also excluded — a fully-random sample produces
    // nonsensical family trees and there's no founder narrative to
    // anchor parent/child links to on a fresh island.
    auto pickType = [&]() -> uint32_t {
        uint32_t r = xorshift32(seed) % 100;
        if (r < 70)  return REL_KNOWS;
        return REL_FRIEND;
    };
    auto pickMeter = [&]() -> int32_t {
        return NATURAL_METERS[xorshift32(seed) % NATURAL_COUNT];
    };

    int written = 0;
    // The "TypeSetTime" field stores game-local seconds, not UNIX time —
    // writing 1.77 billion (current UNIX epoch) here tripped some internal
    // sanity check and made the game refuse to load the save. Match the
    // seed's behaviour (0 = "always known / no specific transition time").
    uint64_t nowSecs = 0;
    int slotsAvailable = std::min(pc, (int)wanted.size());

    for (int i = 0; i < slotsAvailable; i++) {
        const Pair& p = wanted[i];
        SaveEditor::SetIntAt   (mii, H::REL_IA,    i,        p.a);
        SaveEditor::SetIntAt   (mii, H::REL_IB,    i,        p.b);
        uint32_t t = pickType();
        // BASE/METER store two directed entries per pair slot (i*2, i*2+1).
        SaveEditor::SetEnumAt  (mii, H::REL_BASE,  i*2,      t);
        SaveEditor::SetEnumAt  (mii, H::REL_BASE,  i*2+1,    t);
        SaveEditor::SetIntAt   (mii, H::REL_METER, i*2,      pickMeter());
        SaveEditor::SetIntAt   (mii, H::REL_METER, i*2+1,    pickMeter());
        SaveEditor::SetUInt64At(mii, H::REL_TST,   i,        nowSecs);
        written++;
    }

    // Clear any remaining slots so stale data doesn't show up as duplicates.
    for (int i = slotsAvailable; i < pc; i++) {
        SaveEditor::SetIntAt   (mii, H::REL_IA,    i, -1);
        SaveEditor::SetIntAt   (mii, H::REL_IB,    i, -1);
        SaveEditor::SetEnumAt  (mii, H::REL_BASE,  i*2,   REL_UNKNOWN);
        SaveEditor::SetEnumAt  (mii, H::REL_BASE,  i*2+1, REL_UNKNOWN);
        SaveEditor::SetIntAt   (mii, H::REL_METER, i*2,   0);
        SaveEditor::SetIntAt   (mii, H::REL_METER, i*2+1, 0);
        SaveEditor::SetUInt64At(mii, H::REL_TST,   i, 0);
    }

    return written;
}

int WipeRelationships(SaveEditor::SavFile& mii) {
    const int pc = SaveEditor::ArraySize(mii, H::REL_IA);
    for (int i = 0; i < pc; i++) {
        SaveEditor::SetIntAt   (mii, H::REL_IA,    i, -1);
        SaveEditor::SetIntAt   (mii, H::REL_IB,    i, -1);
        SaveEditor::SetEnumAt  (mii, H::REL_BASE,  i*2,   REL_UNKNOWN);
        SaveEditor::SetEnumAt  (mii, H::REL_BASE,  i*2+1, REL_UNKNOWN);
        SaveEditor::SetIntAt   (mii, H::REL_METER, i*2,   0);
        SaveEditor::SetIntAt   (mii, H::REL_METER, i*2+1, 0);
        SaveEditor::SetUInt64At(mii, H::REL_TST,   i, 0);
    }
    return pc;
}

// ── Housing pass ────────────────────────────────────────────────────────────

// ── Wishes unlock ───────────────────────────────────────────────────────────
//
// Rebuild the three parallel arrays under Liberation.WishInfo so every wish in
// WishesData::WISHES[] is present and marked liberated. The save's existing
// arrays may be shorter than 268 — we grow them by appending via SetUIntAt /
// SetBoolAt, which the SaveEditor heap writer handles transparently.

int UnlockAllWishes(SaveEditor::SavFile& player) {
    const uint32_t hIds = SaveEditor::Hash("Liberation.WishInfo.WishIdValue");
    const uint32_t hLib = SaveEditor::Hash("Liberation.WishInfo.IsLiberated");
    const uint32_t hNew = SaveEditor::Hash("Liberation.WishInfo.IsNew");

    // Read existing arrays into local buffers (some entries may be empty in a
    // freshly-seeded save).
    int existing = SaveEditor::ArraySize(player, hIds);
    std::vector<uint32_t> ids(existing);
    std::vector<bool>     lib(existing, false);
    int newCount = SaveEditor::ArraySize(player, hNew);
    std::vector<bool>     isNew(newCount, false);
    for (int i = 0; i < existing; i++) {
        ids[(size_t)i] = SaveEditor::GetUIntAt (player, hIds, i, 0u);
        lib[(size_t)i] = SaveEditor::GetBoolAt(player, hLib, i, false);
    }
    for (int i = 0; i < newCount; i++)
        isNew[(size_t)i] = SaveEditor::GetBoolAt(player, hNew, i, false);

    auto findIdx = [&](uint32_t target) -> int {
        for (size_t k = 0; k < ids.size(); k++) if (ids[k] == target) return (int)k;
        return -1;
    };

    int changed = 0;
    for (size_t w = 0; w < sizeof(WishesData::WISHES) / sizeof(WishesData::WishEntry); w++) {
        uint32_t target = WishesData::WISHES[w].hash;
        int idx = findIdx(target);
        if (idx < 0) {
            ids.push_back(target);
            lib.push_back(true);
            isNew.push_back(false);
            changed++;
        } else {
            if (!lib[(size_t)idx]) { lib[(size_t)idx] = true; changed++; }
        }
    }
    // Keep the IsNew array length matching IsLiberated/WishIdValue so the
    // game's parallel-array reads stay consistent.
    while (isNew.size() < ids.size()) isNew.push_back(false);

    // Rewrite the entry payloads in one shot — SaveEditor::SetUIntAt /
    // SetBoolAt won't grow arrays, so we replace bytes directly.
    RewriteUIntArray(player, hIds, ids);
    RewriteBoolArray(player, hLib, lib);
    if (newCount > 0 || isNew.size() > 0)
        RewriteBoolArray(player, hNew, isNew);
    return changed;
}

// ── Random mii levels ───────────────────────────────────────────────────────

int RandomizeMiiLevels(SaveEditor::SavFile& mii,
                       int minLevel, int maxLevel,
                       uint32_t seed) {
    if (maxLevel < minLevel) std::swap(minLevel, maxLevel);
    if (seed == 0) seed = (uint32_t)std::time(nullptr) ^ 0xC0FFEE01u;
    if (seed == 0) seed = 1;

    const uint32_t hLevel = SaveEditor::Hash("Mii.MiiMisc.SatisfyInfo.Level");
    std::vector<int> slots = CollectValidMiiSlots(mii);
    int span = maxLevel - minLevel + 1;
    for (int slot : slots) {
        int v = minLevel + (int)(xorshift32(seed) % (uint32_t)span);
        SaveEditor::SetIntAt(mii, hLevel, slot, v);
    }
    return (int)slots.size();
}

int AssignHousing(SaveEditor::SavFile& mii, const SaveEditor::SavFile& map) {
    // Collect the set of valid house IDs from Map.sav. Writing a HouseMapId
    // that isn't in this set is the documented game-crash trap, so we strictly
    // intersect.
    std::vector<int32_t> houses;
    {
        int hc = SaveEditor::ArraySize(map, H::HouseMapIdHash());
        for (int i = 0; i < hc; i++) {
            int32_t id = SaveEditor::GetIntAt(map, H::HouseMapIdHash(), i, -1);
            if (id > 0) houses.push_back(id);
        }
    }
    if (houses.empty()) return 0;

    // Tomodachi Life's per-house cap maxes at 8 (matches the housingRoomCap
    // approximation in the JS housing UI). With no observed occupants we'd
    // otherwise clamp to 1, which would force every mii into a different
    // house and starve large islands — so we let ourselves use up to 8 rooms
    // regardless and trust the game to refuse one-room houses (the JS code's
    // crash trap is on unknown house IDs, not over-room writes).
    constexpr int kMaxRooms = 8;
    std::vector<std::vector<bool>> occupied(houses.size(),
        std::vector<bool>(kMaxRooms, false));

    auto houseIndex = [&](int32_t id) -> int {
        for (size_t i = 0; i < houses.size(); i++)
            if (houses[i] == id) return (int)i;
        return -1;
    };

    // First pass: read existing occupancy so we never displace a mii that
    // already has a valid slot.
    std::vector<int> slots = CollectValidMiiSlots(mii);
    std::vector<bool> alreadyPlaced(slots.size(), false);
    for (size_t k = 0; k < slots.size(); k++) {
        int slot = slots[k];
        int32_t h = SaveEditor::GetIntAt(mii, H::MiiHouseMapId(), slot, -1);
        int32_t r = SaveEditor::GetIntAt(mii, H::MiiRoomIndex(),  slot, -1);
        int hi = houseIndex(h);
        if (hi >= 0 && r >= 0 && r < kMaxRooms && !occupied[hi][r]) {
            occupied[hi][r] = true;
            alreadyPlaced[k] = true;
        }
    }

    // Second pass: place miis without a valid slot. Walk houses in order,
    // rooms in order; restart from the first house once all are full.
    int placed = 0;
    size_t cursorHouse = 0;
    int cursorRoom = 0;
    for (size_t k = 0; k < slots.size(); k++) {
        if (alreadyPlaced[k]) continue;
        bool found = false;
        for (size_t attempts = 0; attempts < houses.size() * kMaxRooms; attempts++) {
            size_t hi = cursorHouse;
            int    ri = cursorRoom;
            // advance cursor for next attempt
            cursorRoom++;
            if (cursorRoom >= kMaxRooms) { cursorRoom = 0; cursorHouse = (cursorHouse + 1) % houses.size(); }
            if (occupied[hi][ri]) continue;
            occupied[hi][ri] = true;
            SaveEditor::SetIntAt(mii, H::MiiHouseMapId(), slots[k], houses[hi]);
            SaveEditor::SetIntAt(mii, H::MiiRoomIndex(),  slots[k], ri);
            placed++;
            found = true;
            break;
        }
        if (!found) break; // island is full
    }
    return placed;
}

// ── Random belongings ──────────────────────────────────────────────────────
//
// Each Mii has a fixed-size pocket (12 slots in the seed). The game's UI
// scans `Mii.Belongings.GoodsOwnInfoSlot.GoodsStringId` and treats slot
// `miiIdx * slotsPerMii + s` as belonging to Mii `miiIdx`. We pick a random
// item count in [minItems, maxItems] per Mii and fill that many slots with
// random Treasure hashes from treasure_data.h, then clear the rest.
//
// Unlike relationship-typing (which has the Couple/Lovers uniqueness trap),
// the same item can appear in multiple Miis' pockets — Treasures are not
// unique per resident in-game.

int RandomizeMiiBelongings(SaveEditor::SavFile& mii,
                           int minItems, int maxItems,
                           uint32_t seed) {
    if (seed == 0) seed = (uint32_t)std::time(nullptr) ^ 0xBEEFC0DEu;
    if (seed == 0) seed = 1;
    if (minItems < 0) minItems = 0;
    if (maxItems < minItems) maxItems = minItems;

    const uint32_t hSid  = SaveEditor::Hash("Mii.Belongings.GoodsOwnInfoSlot.GoodsStringId");
    const uint32_t hUgc  = SaveEditor::Hash("Mii.Belongings.GoodsOwnInfoSlot.UgcGoodsIndex");
    const uint32_t hTime = SaveEditor::Hash("Mii.Belongings.GoodsOwnInfoSlot.GetTime");

    int totalSlots = SaveEditor::ArraySize(mii, hSid);
    if (totalSlots <= 0) return 0;

    // slotsPerMii is derived the same way the WebUI does it: total slots
    // divided by the Mii count. The game allocates `slotsPerMii * miiCount`
    // contiguous entries; per-Mii index = miiIdx * slotsPerMii + slot.
    const uint32_t hName = H::MiiName();
    int miiArraySize = SaveEditor::ArraySize(mii, hName);
    if (miiArraySize <= 0 || totalSlots % miiArraySize != 0) {
        // Layout doesn't match expected (slotsPerMii * miiCount) shape.
        // Bail rather than write into the wrong Mii's pocket.
        return 0;
    }
    int slotsPerMii = totalSlots / miiArraySize;

    int written = 0;
    const int span = maxItems - minItems + 1;
    for (int m = 0; m < miiArraySize; m++) {
        // Skip empty Mii slots — they shouldn't get inventory.
        std::string nm = SaveEditor::GetWStr32At(mii, hName, m);
        bool slotEmpty = nm.empty();

        int n = slotEmpty
                  ? 0
                  : minItems + (int)(xorshift32(seed) % (uint32_t)span);
        if (n > slotsPerMii) n = slotsPerMii;

        for (int s = 0; s < slotsPerMii; s++) {
            int idx = m * slotsPerMii + s;
            uint32_t sid;
            if (s < n) {
                sid = IslandGen::TreasureData::TREASURE_HASHES[
                    xorshift32(seed) % (uint32_t)IslandGen::TreasureData::TREASURE_HASH_COUNT];
                written++;
            } else {
                sid = 0;
            }
            SaveEditor::SetUIntAt  (mii, hSid,  idx, sid);
            SaveEditor::SetIntAt   (mii, hUgc,  idx, -1);   // not a UGC item
            SaveEditor::SetUInt64At(mii, hTime, idx, 0);    // game-local zero
        }
    }
    return written;
}

// ── Roads + decor (random-map dressing pass) ───────────────────────────────
//
// Building anchors have already been randomised by SnapActorsToLand and the
// empty actor slots cleared to ActorKey=0. This pass:
//   1. Picks the building closest to the centroid of all placed buildings
//      as the "hub" and paints an L-shaped Asphalt road from each other
//      building to the hub (water tiles are skipped, so paths still respect
//      the coastline).
//   2. Walks the cleared actor slots and refills them with a random decor
//      hash (tree / lamp / hedge / flower / bench) placed on a random land
//      tile that isn't a road, isn't already taken by a building footprint,
//      and isn't right on top of another decor we just placed.
//
// Footprint-aware: we re-derive each building's rotated rect to mark
// occupied tiles so decor never overlaps a building.

int PaintRoadsAndDecor(SaveEditor::SavFile& map, uint32_t seed) {
    if (seed == 0) seed = (uint32_t)std::time(nullptr) ^ 0xDECC0FFEu;
    if (seed == 0) seed = 1;

    SaveEditor::Entry* fe = FindFloorEntry(map);
    if (!fe || fe->payload.size() < 4 + (size_t)MAP_TILES * 4) return 0;
    uint8_t* tiles = fe->payload.data() + 4;

    auto isLand = [&](int x, int y) -> bool {
        if (x < 0 || x >= MAP_W || y < 0 || y >= MAP_H) return false;
        return GetTileRaw(tiles, x * MAP_H + y) != TILE_WATER;
    };

    constexpr uint32_t TILE_ASPHALT = 0xd7e5e4e0u;

    // ── 1. Gather placed building anchors + their footprints ─────────────
    struct Placed {
        int             idx;
        int             x, y;
        ActorFootprint  fp;
    };
    std::vector<Placed> bldgs;

    uint32_t hRotY = SaveEditor::Hash("MapObject.MapObjectMisc.RotY");
    int actorCount = SaveEditor::ArraySize(map, H::ActorHash());
    for (int i = 0; i < actorCount; i++) {
        uint32_t a = SaveEditor::GetUIntAt(map, H::ActorHash(), i, 0u);
        if (a == 0 || !IsBuildingHash(a)) continue;
        int x = SaveEditor::GetIntAt(map, H::ActorPosXHash(), i, -1);
        int y = SaveEditor::GetIntAt(map, H::ActorPosYHash(), i, -1);
        if (x < 0 || y < 0 || x >= MAP_W || y >= MAP_H) continue;
        float rot = SaveEditor::GetFloatAt(map, hRotY, i, 0.0f);
        ActorFootprint fp = RotateFootprint(LookupActorFootprint(a), rot);
        bldgs.push_back({i, x, y, fp});
    }
    if (bldgs.empty()) return 0;

    // Building-occupancy bitmap (also marks tiles right next to a building
    // as "no decor here" so we don't bury the entryway under a hedge).
    std::vector<uint8_t> occupied((size_t)MAP_W * MAP_H, 0);
    auto markOcc = [&](int x, int y) {
        if (x < 0 || x >= MAP_W || y < 0 || y >= MAP_H) return;
        occupied[(size_t)x * MAP_H + y] = 1;
    };
    for (auto& b : bldgs) {
        for (int dx = 0; dx < b.fp.w; dx++)
            for (int dy = 0; dy < b.fp.h; dy++)
                markOcc(b.x + b.fp.x0 + dx, b.y + b.fp.y0 + dy);
    }

    // ── 2. Build a Minimum Spanning Tree over the building anchors ──────
    // Kruskal's: sort all building-pair edges by Manhattan distance then
    // greedily add the shortest edge that doesn't form a cycle until every
    // building is in one component. This replaces the "every building
    // shoots an L straight at one central hub" pattern (which always
    // looked like spokes around a wheel) with a network where each
    // building connects to its NEAREST neighbour. Subsequent pathfinding
    // (step 3) then has every reason to merge shared segments.
    struct Edge { int a, b; int dist; };
    std::vector<Edge> edges;
    edges.reserve(bldgs.size() * (bldgs.size() - 1) / 2);
    for (size_t i = 0; i < bldgs.size(); i++) {
        for (size_t j = i + 1; j < bldgs.size(); j++) {
            int d = std::abs(bldgs[i].x - bldgs[j].x)
                  + std::abs(bldgs[i].y - bldgs[j].y);
            edges.push_back({(int)i, (int)j, d});
        }
    }
    std::sort(edges.begin(), edges.end(),
              [](const Edge& A, const Edge& B){ return A.dist < B.dist; });
    // Union-find for cycle detection.
    std::vector<int> parent(bldgs.size());
    for (size_t i = 0; i < bldgs.size(); i++) parent[i] = (int)i;
    std::function<int(int)> find = [&](int x) -> int {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    std::vector<Edge> mst;
    mst.reserve(bldgs.size() - 1);
    for (auto& e : edges) {
        int ra = find(e.a), rb = find(e.b);
        if (ra == rb) continue;
        parent[ra] = rb;
        mst.push_back(e);
        if (mst.size() == bldgs.size() - 1) break;
    }

    // ── 3. Path every MST edge with Dijkstra over a noisy land grid ─────
    // The cost function:
    //   - blocked  → water tile, building footprint, out-of-bounds
    //   - 0 cost   → tile we've already painted as road (encourages later
    //                paths to merge into a shared corridor — natural-
    //                looking road network)
    //   - 1+jitter → fresh land tile; the small 0..3 jitter from a per-
    //                tile noise table is what makes the routes wobble
    //                instead of forming a perfect Manhattan staircase.
    std::vector<uint8_t> roadMask((size_t)MAP_TILES, 0);
    auto paintRoad = [&](int x, int y) {
        if (!isLand(x, y)) return;
        if (occupied[(size_t)x * MAP_H + y]) return;
        SetTileRaw(tiles, x * MAP_H + y, TILE_ASPHALT);
        roadMask[(size_t)x * MAP_H + y] = 1;
    };
    // Per-tile noise table — sampled once per generation so the same
    // generation gets consistent path wobble (deterministic given a seed).
    std::vector<uint8_t> noise((size_t)MAP_TILES);
    {
        uint32_t ns = seed ^ 0x517CC1B7u;
        if (ns == 0) ns = 1;
        for (int i = 0; i < MAP_TILES; i++) noise[i] = (uint8_t)(xorshift32(ns) & 3u);
    }
    auto tileCost = [&](int x, int y) -> int {
        if (x < 0 || x >= MAP_W || y < 0 || y >= MAP_H) return -1;
        size_t idx = (size_t)x * MAP_H + y;
        if (!isLand(x, y)) return -1;
        if (occupied[idx]) return -1;
        if (roadMask[idx]) return 0;          // free to reuse
        return 1 + (int)noise[idx];
    };

    // Dijkstra wrapper — runs once per MST edge to produce a wiggly path.
    // To keep the search bounded we abort once we've visited > 8000 tiles
    // (whole map is MAP_W*MAP_H = 9600 — plenty of headroom but we never
    // run forever on a corner-case map).
    std::vector<int> dist((size_t)MAP_TILES, INT_MAX);
    std::vector<int> prev((size_t)MAP_TILES, -1);
    using P = std::pair<int, int>;  // (dist, idx)
    // Find the nearest tile outside `(ax,ay)`'s building footprint, picked
    // to minimize Manhattan distance to (towardX, towardY). Expands ring
    // by ring so buildings with negative x0/y0 offsets — whose immediate
    // neighbours are still inside their own footprint — still get a
    // usable "doorstep" tile to start/end the path on.
    auto nearestFreeTo = [&](int ax, int ay, int towardX, int towardY) -> int {
        const int maxR = std::max(MAP_W, MAP_H);
        for (int r = 1; r < maxR; r++) {
            int best = -1, bestDist = INT_MAX;
            for (int dx = -r; dx <= r; dx++) {
                int dy = r - std::abs(dx);
                auto consider = [&](int nx, int ny) {
                    if (tileCost(nx, ny) < 0) return;
                    int d = std::abs(nx - towardX) + std::abs(ny - towardY);
                    if (d < bestDist) {
                        bestDist = d;
                        best = nx * MAP_H + ny;
                    }
                };
                consider(ax + dx, ay + dy);
                if (dy != 0) consider(ax + dx, ay - dy);
            }
            if (best >= 0) return best;
        }
        return -1;
    };

    for (auto& e : mst) {
        std::fill(dist.begin(), dist.end(), INT_MAX);
        std::fill(prev.begin(), prev.end(), -1);
        int sx = bldgs[e.a].x, sy = bldgs[e.a].y;
        int tx = bldgs[e.b].x, ty = bldgs[e.b].y;
        // Compute the doorsteps on BOTH ends: source-doorstep picks the
        // free tile near source closest to target, target-doorstep picks
        // the free tile near target closest to source. Dijkstra then runs
        // doorstep → doorstep.
        int startIdx = nearestFreeTo(sx, sy, tx, ty);
        int endIdx   = nearestFreeTo(tx, ty, sx, sy);
        if (startIdx < 0 || endIdx < 0) continue;
        if (startIdx == endIdx) {
            // Buildings basically next to each other — single-tile road
            // is enough to "connect" them visually.
            paintRoad(startIdx / MAP_H, startIdx % MAP_H);
            continue;
        }
        dist[startIdx] = 0;
        std::priority_queue<P, std::vector<P>, std::greater<P>> pq;
        pq.push({0, startIdx});
        int reachedIdx = -1;
        int visited = 0;
        while (!pq.empty() && visited < 8000) {
            auto [d, idx] = pq.top(); pq.pop();
            if (d > dist[idx]) continue;
            visited++;
            if (idx == endIdx) { reachedIdx = idx; break; }
            int cx = idx / MAP_H, cy = idx % MAP_H;
            const int dx4[] = { 1, -1, 0, 0 };
            const int dy4[] = { 0, 0, 1, -1 };
            for (int k = 0; k < 4; k++) {
                int nx = cx + dx4[k], ny = cy + dy4[k];
                int c = tileCost(nx, ny);
                if (c < 0) continue;
                int ni = nx * MAP_H + ny;
                int nd = d + c;
                if (nd < dist[ni]) { dist[ni] = nd; prev[ni] = idx; pq.push({nd, ni}); }
            }
        }
        if (reachedIdx < 0) continue;
        // Walk back and paint every tile on the path as road.
        for (int n = reachedIdx; n != -1; n = prev[n]) {
            paintRoad(n / MAP_H, n % MAP_H);
        }
    }

    // ── 4. Decor — refill the cleared actor slots with random items ─────
    // Curated pool of 1×1 / 2×1 decor actors so we never have to worry
    // about footprint overlap beyond the anchor tile itself. Trees and
    // flowers are weighted more heavily than lamps and benches for a
    // natural look.
    static const uint32_t DECOR_POOL[] = {
        // Trees — weighted heaviest. ObjTreeChristmas is intentionally out
        // (looked seasonal-themed on every random island regardless of the
        // surface theme). Mix of broadleaf, conifer, cherry, ginkgo, palm
        // gives a nice variety of canopy shapes.
        0x00f79623u, // ObjTreeBroadleaf
        0x00f79623u, // ObjTreeBroadleaf       — weight
        0x0f012cb7u, // ObjTreeBroadleaf_01
        0x5fd51925u, // ObjTreeBroadleaf_02
        0x70ccf14bu, // ObjTreeCherry (sakura)
        0x70ccf14bu, // ObjTreeCherry          — weight
        0x9818108cu, // ObjTreeConiferous
        0x256ae934u, // ObjTreeConiferous_01
        0x0da9f2ccu, // ObjTreeGinkgo
        0x2664f7d1u, // ObjTreePalm
        // Flowers — small, scatter-friendly. Decent weight on Nemophila
        // and Tulip since those read most clearly at icon scale.
        0xe21c58ebu, // ObjFlowerNemophila
        0xe21c58ebu, // ObjFlowerNemophila     — weight
        0xf698e38bu, // ObjFlowerAnemone
        0xefa370e9u, // ObjFlowerCosmos
        0x7ab365bbu, // ObjFlowerLavender
        0xe828641cu, // ObjFlowerSunflowers
        0x80c4e173u, // ObjFlowerTulip
        0x80c4e173u, // ObjFlowerTulip         — weight
        0xc28ce29du, // ObjFlowerNarcissus
        0x28895d61u, // ObjFlowerPampasGrass
        // Street furniture & garden seating — more variety now: a mix of
        // park benches, home/terrace benches (= "salon de jardin"),
        // picnic tables, and street lamps (modern + retro).
        0x0fc4dae6u, // ObjHedge_01
        // Park benches
        0x3a379430u, // ObjBenchPark
        0x739bb804u, // ObjBenchPark_01
        0xe2123edeu, // ObjBenchPark_02
        0x60974f1du, // ObjBenchPark_03
        // Garden / terrace benches
        0x2d70949au, // ObjBenchHome
        0xb06856c3u, // ObjBenchHome_01
        0x8260ac6du, // ObjBenchHome_02
        0x5468ea41u, // ObjBenchTerrace
        0x2cd639c2u, // ObjBenchTerrace_01
        0x51456fc7u, // ObjBenchTerrace_02
        // Picnic tables
        0x94046edfu, // ObjTableBench
        0x93bfa683u, // ObjTableBench_01
        0x9737f2aeu, // ObjTableBench_02
        0x1db7b2ecu, // ObjTableBench_03
        // Street lamps — modern + retro mix
        0x1b96fc41u, // ObjStreetLamp
        0xc5a68179u, // ObjStreetLamp_01
        0xb33cd644u, // ObjStreetLamp_02
        0x02b59bf9u, // ObjStreetLamp_05
        0xf57069cau, // ObjStreetLampRetro
        0x536c8eebu, // ObjStreetLampRetro_01
        0xef56842du, // ObjStreetLampRetro_03
        // Playground — seesaws (balançoire) and spring-rider animals
        0xc9e023efu, // ObjSeesaw
        0xffbee148u, // ObjSeesaw_01
        0x48870a72u, // ObjSeesaw_02
        0xd9d34019u, // ObjSeesaw_03
        0x821c5664u, // ObjSwingRider
        0x7cc23163u, // ObjSwingRider_01
        0x893aa4bdu, // ObjSwingRider_02
        0x7b28f8c4u, // ObjSwingRider_03
    };
    static constexpr int DECOR_POOL_COUNT =
        sizeof(DECOR_POOL) / sizeof(uint32_t);

    // ── 3b. Beach access: find each connected beach (sand-tile) cluster
    //       large enough to walk on, then place a wooden step at one of
    //       its inland borders so Miis have a route from grass to sand,
    //       plus a parasol/bed on the beach interior for atmosphere.
    constexpr uint32_t TILE_BEACH_HASH = 0xb6d76a62u;
    auto getTile = [&](int x, int y) -> uint32_t {
        if (x < 0 || x >= MAP_W || y < 0 || y >= MAP_H) return 0;
        return GetTileRaw(tiles, x * MAP_H + y);
    };
    auto isBeach = [&](int x, int y) -> bool {
        return getTile(x, y) == TILE_BEACH_HASH;
    };

    // Quick disjoint-set on beach tiles. Sized small enough that a plain
    // BFS per component is fine.
    std::vector<int> beachComp((size_t)MAP_TILES, -1);
    std::vector<int> compSize;
    {
        for (int x = 0; x < MAP_W; x++) {
            for (int y = 0; y < MAP_H; y++) {
                int idx = x * MAP_H + y;
                if (beachComp[idx] != -1 || !isBeach(x, y)) continue;
                int cid = (int)compSize.size();
                std::queue<int> q; q.push(idx);
                beachComp[idx] = cid;
                int sz = 0;
                while (!q.empty()) {
                    int cur = q.front(); q.pop();
                    sz++;
                    int cx2 = cur / MAP_H, cy2 = cur % MAP_H;
                    const int dx4[] = { -1, 1, 0, 0 };
                    const int dy4[] = { 0, 0, -1, 1 };
                    for (int k = 0; k < 4; k++) {
                        int nx = cx2 + dx4[k], ny = cy2 + dy4[k];
                        if (nx < 0 || nx >= MAP_W || ny < 0 || ny >= MAP_H) continue;
                        int ni = nx * MAP_H + ny;
                        if (beachComp[ni] != -1) continue;
                        if (!isBeach(nx, ny)) continue;
                        beachComp[ni] = cid;
                        q.push(ni);
                    }
                }
                compSize.push_back(sz);
            }
        }
    }

    // For each big-enough beach, pick a border tile for a step and an
    // interior tile for a parasol. "Big enough" = ≥ 8 sand tiles, which
    // weeds out tiny single-tile coastline curls.
    static const uint32_t BEACH_STEPS[] = {
        0x16905ce1u, // ObjStepWood
        0x75fe8121u, // ObjStepWood_01
        0xf29bd6d8u, // ObjStepStone
    };
    static const uint32_t BEACH_FURN[] = {
        0xf082e4cau, // ObjBeachParasol
        0x06019f97u, // ObjBeachParasol_01
        0x36147dc4u, // ObjBeachBed
        0x53a5e5b4u, // ObjBeachBed_01
    };
    constexpr int kBeachSizeMin = 8;

    auto claimEmptySlot = [&]() -> int {
        for (int i = 0; i < actorCount; i++) {
            uint32_t a = SaveEditor::GetUIntAt(map, H::ActorHash(), i, 0u);
            if (a == 0) return i;
        }
        return -1;
    };
    auto placeActor = [&](uint32_t hash, int x, int y) -> bool {
        int slot = claimEmptySlot();
        if (slot < 0) return false;
        SaveEditor::SetUIntAt(map, H::ActorHash(),     slot, hash);
        SaveEditor::SetIntAt (map, H::ActorPosXHash(), slot, x);
        SaveEditor::SetIntAt (map, H::ActorPosYHash(), slot, y);
        SaveEditor::SetFloatAt(map, hRotY,             slot,
                               (float)((xorshift32(seed) & 3u) * 90u));
        markOcc(x, y);
        return true;
    };

    for (int cid = 0; cid < (int)compSize.size(); cid++) {
        if (compSize[cid] < kBeachSizeMin) continue;
        // Collect border + interior candidates for this component.
        std::vector<int> borders;   // beach tile with a non-beach LAND neighbour
        std::vector<int> interior;  // beach tile fully surrounded by beach/water
        for (int x = 0; x < MAP_W; x++) {
            for (int y = 0; y < MAP_H; y++) {
                int idx = x * MAP_H + y;
                if (beachComp[idx] != cid) continue;
                if (occupied[idx]) continue;
                bool touchesInlandLand = false;
                const int dx4[] = { -1, 1, 0, 0 };
                const int dy4[] = { 0, 0, -1, 1 };
                for (int k = 0; k < 4; k++) {
                    int nx = x + dx4[k], ny = y + dy4[k];
                    if (!isLand(nx, ny)) continue;
                    if (isBeach(nx, ny))  continue;
                    touchesInlandLand = true; break;
                }
                if (touchesInlandLand) borders.push_back(idx);
                else                   interior.push_back(idx);
            }
        }
        // Step at a random border, parasol/bed in the interior.
        if (!borders.empty()) {
            int b = borders[xorshift32(seed) % (uint32_t)borders.size()];
            uint32_t h = BEACH_STEPS[
                xorshift32(seed) %
                (uint32_t)(sizeof(BEACH_STEPS) / sizeof(uint32_t))];
            placeActor(h, b / MAP_H, b % MAP_H);
        }
        if (!interior.empty()) {
            int b = interior[xorshift32(seed) % (uint32_t)interior.size()];
            uint32_t h = BEACH_FURN[
                xorshift32(seed) %
                (uint32_t)(sizeof(BEACH_FURN) / sizeof(uint32_t))];
            placeActor(h, b / MAP_H, b % MAP_H);
        }
    }

    int placed = 0;
    // Max decor count = number of empty actor slots. Don't blow up on
    // unbounded maps — cap to a sensible total so we don't iterate over
    // hundreds of empty slots placing every single one.
    constexpr int kMaxDecor = 250;
    for (int i = 0; i < actorCount && placed < kMaxDecor; i++) {
        uint32_t a = SaveEditor::GetUIntAt(map, H::ActorHash(), i, 0u);
        if (a != 0) continue;  // only refill cleared slots

        // Find a random valid tile (land, not road, not already occupied).
        int rx = -1, ry = -1;
        for (int t = 0; t < 60; t++) {
            int cand_x = (int)(xorshift32(seed) % (uint32_t)MAP_W);
            int cand_y = (int)(xorshift32(seed) % (uint32_t)MAP_H);
            if (!isLand(cand_x, cand_y)) continue;
            size_t idx = (size_t)cand_x * MAP_H + cand_y;
            if (occupied[idx]) continue;
            if (roadMask[idx]) continue;  // don't block roads
            rx = cand_x; ry = cand_y;
            break;
        }
        if (rx < 0) continue;

        uint32_t hash = DECOR_POOL[xorshift32(seed) % (uint32_t)DECOR_POOL_COUNT];
        float rot = (float)((xorshift32(seed) & 3u) * 90u);
        SaveEditor::SetUIntAt(map, H::ActorHash(),     i, hash);
        SaveEditor::SetIntAt (map, H::ActorPosXHash(), i, rx);
        SaveEditor::SetIntAt (map, H::ActorPosYHash(), i, ry);
        SaveEditor::SetFloatAt(map, hRotY,             i, rot);
        // Block the tile so the next decor pick doesn't land on top.
        markOcc(rx, ry);
        placed++;
    }

    return placed;
}

// ── Random outfits ─────────────────────────────────────────────────────────
//
// Every Mii has eight ClothInfo.<piece>.KeyHash slots — Tops, Topslong,
// BottomsA, BottomsB, Shoes, Headwear, Accessory, All. A Coordinate from
// coordinate_data.h is just a pre-assembled bundle of those 8 KeyHashes
// (with some 0 = "no piece worn"); writing all 8 at once is what the
// game's outfit picker does internally, so doing the same here gives the
// Mii a fully-coordinated look without needing to think about gender-
// specific top vs topslong, tucked-in flag, etc.
//
// Women-only coordinates are gated on the Mii's Gender enum hashing to
// "Female" (Murmur3 of the identifier, same as every other enum). Unisex
// coordinates are open to anyone.

int RandomizeMiiOutfits(SaveEditor::SavFile& mii, uint32_t seed) {
    if (seed == 0) seed = (uint32_t)std::time(nullptr) ^ 0xC1234567u;
    if (seed == 0) seed = 1;

    // KeyHash array hashes for each clothing piece.
    const uint32_t hTops      = SaveEditor::Hash("Mii.MiiMisc.ClothInfo.Tops.KeyHash");
    const uint32_t hTopslong  = SaveEditor::Hash("Mii.MiiMisc.ClothInfo.Topslong.KeyHash");
    const uint32_t hBottomsA  = SaveEditor::Hash("Mii.MiiMisc.ClothInfo.BottomsA.KeyHash");
    const uint32_t hBottomsB  = SaveEditor::Hash("Mii.MiiMisc.ClothInfo.BottomsB.KeyHash");
    const uint32_t hShoes     = SaveEditor::Hash("Mii.MiiMisc.ClothInfo.Shoes.KeyHash");
    const uint32_t hHeadwear  = SaveEditor::Hash("Mii.MiiMisc.ClothInfo.Headwear.KeyHash");
    const uint32_t hAccessory = SaveEditor::Hash("Mii.MiiMisc.ClothInfo.Accessory.KeyHash");
    const uint32_t hAll       = SaveEditor::Hash("Mii.MiiMisc.ClothInfo.All.KeyHash");
    const uint32_t hGender    = SaveEditor::Hash("Mii.MiiBody.AppearanceParam.Gender");
    const uint32_t hName      = H::MiiName();

    const uint32_t FEMALE_ENUM_HASH = SaveEditor::Hash("Female");

    int miiCount = SaveEditor::ArraySize(mii, hName);
    if (miiCount <= 0) return 0;

    // Precompute pools of (a) all coordinates, (b) only the Unisex ones.
    // Men/Third get drawn from the Unisex pool; Female draws from the full
    // pool. This matches the source coordinates.json which has Unisex +
    // Women categories only (no Male-specific entries).
    std::vector<int> unisexPool;
    unisexPool.reserve(IslandGen::CoordinateData::COORDINATE_COUNT);
    for (int i = 0; i < IslandGen::CoordinateData::COORDINATE_COUNT; i++) {
        if (IslandGen::CoordinateData::COORDINATES[i].unisex) unisexPool.push_back(i);
    }
    if (unisexPool.empty()) return 0; // sanity — table should never be empty

    int dressed = 0;
    for (int m = 0; m < miiCount; m++) {
        std::string nm = SaveEditor::GetWStr32At(mii, hName, m);
        if (nm.empty()) continue;

        uint32_t genderH = SaveEditor::GetEnumAt(mii, hGender, m, 0u);
        bool female = (genderH == FEMALE_ENUM_HASH);

        // Female Miis can wear EITHER unisex or women-only entries; pick
        // from the full pool. Everyone else (Male / Third / Invalid) is
        // limited to the unisex pool because the women-only outfits use
        // a female-only body mesh and look broken on a male Mii.
        int idx;
        if (female) {
            idx = (int)(xorshift32(seed) %
                        (uint32_t)IslandGen::CoordinateData::COORDINATE_COUNT);
        } else {
            idx = unisexPool[xorshift32(seed) % (uint32_t)unisexPool.size()];
        }
        const auto& c = IslandGen::CoordinateData::COORDINATES[idx];
        SaveEditor::SetUIntAt(mii, hTops,      m, c.tops);
        SaveEditor::SetUIntAt(mii, hTopslong,  m, c.topslong);
        SaveEditor::SetUIntAt(mii, hBottomsA,  m, c.bottomsA);
        SaveEditor::SetUIntAt(mii, hBottomsB,  m, c.bottomsB);
        SaveEditor::SetUIntAt(mii, hShoes,     m, c.shoes);
        SaveEditor::SetUIntAt(mii, hHeadwear,  m, c.headwear);
        SaveEditor::SetUIntAt(mii, hAccessory, m, c.accessory);
        SaveEditor::SetUIntAt(mii, hAll,       m, c.all);
        dressed++;
    }
    return dressed;
}

// ── Random room interiors ──────────────────────────────────────────────────
//
// Each house has 9 room slots in Map.sav's BaseStyleId UIntArray (so a 70-
// house island uses 630 entries; verified against the seed). The slot for
// a given Mii is `(HouseMapId - 1) * 9 + RoomIndex`. We only touch rooms
// occupied by a valid Mii — empty rooms keep their seed value (0 = default
// unstyled) which the game accepts. Picking from the full room-style hash
// pool means a fresh island has 70 different lived-in vibes.

int RandomizeMiiInteriors(SaveEditor::SavFile& map,
                          const SaveEditor::SavFile& mii,
                          uint32_t seed) {
    if (seed == 0) seed = (uint32_t)std::time(nullptr) ^ 0xDECAFC0Du;
    if (seed == 0) seed = 1;

    const uint32_t hBaseStyle  = SaveEditor::Hash("House.RoomSettings.BaseStyleId");
    const uint32_t hHouseMapId = SaveEditor::Hash("House.MapId");
    const uint32_t hMiiHouseId = H::MiiHouseMapId();
    const uint32_t hMiiRoomIdx = H::MiiRoomIndex();
    const uint32_t hMiiName    = H::MiiName();

    int baseSlots = SaveEditor::ArraySize(map, hBaseStyle);
    int houseCount = SaveEditor::ArraySize(map, hHouseMapId);
    if (baseSlots <= 0 || houseCount <= 0) return 0;
    if (baseSlots % houseCount != 0) return 0; // layout assumption broken
    int roomsPerHouse = baseSlots / houseCount;

    int written = 0;
    int miiCount = SaveEditor::ArraySize(mii, hMiiName);
    for (int m = 0; m < miiCount; m++) {
        // Skip empty Mii slots — their assigned house/room is meaningless.
        std::string nm = SaveEditor::GetWStr32At(mii, hMiiName, m);
        if (nm.empty()) continue;
        int32_t houseId = SaveEditor::GetIntAt(mii, hMiiHouseId, m, -1);
        int32_t roomIdx = SaveEditor::GetIntAt(mii, hMiiRoomIdx, m, -1);
        if (houseId <= 0 || houseId > houseCount) continue;
        if (roomIdx < 0 || roomIdx >= roomsPerHouse) continue;
        int slotIdx = (houseId - 1) * roomsPerHouse + roomIdx;
        uint32_t styleHash = IslandGen::RoomStyleData::ROOM_STYLE_HASHES[
            xorshift32(seed) % (uint32_t)IslandGen::RoomStyleData::ROOM_STYLE_HASH_COUNT];
        SaveEditor::SetUIntAt(map, hBaseStyle, slotIdx, styleHash);
        written++;
    }
    return written;
}

} // namespace IslandGen
