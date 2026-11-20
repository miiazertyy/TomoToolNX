// page_map.inc — TomoToolNX 2.0: the Map place. The island as a canvas (baked
// to a texture, redrawn only when the tiles change), objects drawn over it,
// and a panel of tools on the right. Exploring the canvas, the stick moves the
// player's hand, A picks objects up and puts them down (or paints, or puts a
// new one down), X turns what is held, Y goes to the next object and B goes
// back to the tools. Every write is 1.x's (TMapInsNudge, MapPaintLineTo,
// MapHist*, MapExportPng, the place and delete code) or made the same way, so
// Map.sav changes the same way.
// Included by shell.inc inside namespace S2.

static int sMapCW = 1010, sMapCH = 720;              // the canvas: the whole tab (PageMap sets it)
// What of the canvas the floating tools cover (pixels from each edge), so the
// hand is kept where it can be seen.
static int sMapSafeT = 0, sMapSafeR = 0, sMapSafeB = 0;
static const Ui::Id kMapCanvasId = Ui::HashStr("map.canvas");

static SDL_Texture* sMapTex = nullptr;
static SDL_Color sMapSea{ 40, 78, 104, 255 };   // the deep water at the island's corner, for beyond its edge
static uint32_t sMapFloorSig = 0, sMapObjSig = 0;
struct MapObjRect { int slot, x, y, w, h, group; uint32_t actor; int turns; };   // turns: quarter turns, as MapActorRect
static std::vector<MapObjRect> sMapObjs;
static bool sMapDelAsk = false, sMapPicker = false, sMapTilePicker = false;
static float sMapStickX = 0, sMapStickY = 0;
static bool sMapDragInside = false;

static const SaveEditor::Entry* MapEntry(uint32_t h) {
    for (const auto& e : gMapSav.entries) if (e.hash == h) return &e;
    return nullptr;
}
static uint32_t PayloadSig(const SaveEditor::Entry* e, uint32_t sig) {
    if (!e) return sig;
    for (uint8_t b : e->payload) sig = (sig ^ b) * 16777619u;
    return sig;
}

// The baked island: 8 pixels a tile.
constexpr int kMapTilePx = 8;

// Bake the floor into a picture when it changed: square tiles, as the game
// lays the island out. Similar grounds (grass and clover) are one softly
// mottled area; a one-pixel seam marks where another ground, a road or the
// sea starts, with a line of foam on the water side and the sea deepening
// away from the coast. Only the tiles whose surroundings changed are drawn
// again (painting stays smooth).
static void MapBake() {
    static const uint32_t H_FLOOR = SaveEditor::Hash(MapKeys::FloorKeyHash);
    const uint32_t sig = PayloadSig(MapEntry(H_FLOOR), 2166136261u);
    if (sMapTex && sig == sMapFloorSig) return;
    sMapFloorSig = sig;
    constexpr int TW = MAP_WIDTH, TH = MAP_HEIGHT, P = kMapTilePx;
    constexpr int W = TW * P, H = TH * P;
    static std::vector<uint32_t> sPix, sKey;            // the picture, and what each tile was drawn from
    bool full = false;
    if (!sMapTex) {
        sMapTex = SDL_CreateTexture(gRen, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, W, H);
        if (!sMapTex) return;
        SDL_SetTextureScaleMode(sMapTex, SDL_ScaleModeLinear);
        sPix.assign((size_t)W * H, 0xff181010u);
        sKey.assign((size_t)TW * TH, 0xffffffffu);
        full = true;
    }
    const int n = SaveEditor::ArraySize(gMapSav, H_FLOOR);
    std::vector<const MapData::TileInfo*> tile((size_t)TW * TH, nullptr);
    std::vector<uint8_t> wet((size_t)TW * TH, 0), dist((size_t)TW * TH, 0);
    for (int x = 0; x < TW; x++)
        for (int y = 0; y < TH; y++) {
            const int i = x * TH + y;                     // the save keeps columns
            const MapData::TileInfo* t = i < n ? MapData::TileLookup(SaveEditor::GetUIntAt(gMapSav, H_FLOOR, i, 0)) : nullptr;
            tile[(size_t)y * TW + x] = t;
            wet[(size_t)y * TW + x] = (!t || (t->code && (!strcmp(t->code, "Water") || !strcmp(t->code, "Seaside")))) ? 1 : 0;
        }
    // How far each water tile is from land (1 = touching it), up to 6.
    {
        std::vector<int> q;
        q.reserve((size_t)TW * TH);
        for (int i = 0; i < TW * TH; i++) { dist[i] = wet[i] ? 255 : 0; if (!wet[i]) q.push_back(i); }
        for (size_t h = 0; h < q.size(); h++) {
            const int i = q[h], x = i % TW, y = i / TW;
            const int d = dist[i];
            if (d >= 6) continue;
            const int nb[4][2] = { { x - 1, y }, { x + 1, y }, { x, y - 1 }, { x, y + 1 } };
            for (auto& v : nb) {
                if (v[0] < 0 || v[1] < 0 || v[0] >= TW || v[1] >= TH) continue;
                const int j = v[1] * TW + v[0];
                if (dist[j] > d + 1) { dist[j] = (uint8_t)(d + 1); q.push_back(j); }
            }
        }
    }
    auto pack = [](float r, float g, float b) {
        auto c8 = [](float v) { return (uint32_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); };
        return 0xff000000u | (c8(b) << 16) | (c8(g) << 8) | c8(r);
    };
    // What each tile brings to the blend: the ground under it (a road's own
    // ground is its colour, lightened), the road on it, the water.
    struct Mix3 { float gr, gg, gb, rr, rg, rb, wr, wg, wb; float road, water, deep; uint32_t area; };
    std::vector<Mix3> mx((size_t)TW * TH);
    for (int i = 0; i < TW * TH; i++) {
        const MapData::TileInfo* t = tile[i];
        Mix3& m = mx[i];
        const float r = t ? t->r : 58, g = t ? t->g : 106, b = t ? t->b : 133;
        const bool road = t && t->code && strstr(t->code, "_Road");
        m.road = road ? 1.f : 0.f;
        m.water = wet[i] ? 1.f : 0.f;
        m.deep = wet[i] ? std::min(1.f, (dist[i] - 1) / 5.f) : 0.f;
        const float lift = road ? 1.f / 0.84f : 1.f;
        m.gr = std::min(255.f, r * lift); m.gg = std::min(255.f, g * lift); m.gb = std::min(255.f, b * lift);
        m.rr = r; m.rg = g; m.rb = b;
        m.wr = r; m.wg = g; m.wb = b;
        m.area = 0;
    }
    auto at = [&](int x, int y) {
        x = std::max(0, std::min(TW - 1, x)); y = std::max(0, std::min(TH - 1, y));
        return (size_t)y * TW + x;
    };
    // Grounds of nearly the same colour (grass and clover, say) make one area,
    // softly mottled; between areas the border is crisp and rounded, like the
    // coast. The road tiles of a kind are an area of their own (the path is
    // drawn over them). An area is known by the smallest tile hash in it, so it
    // keeps its name while painting.
    {
        std::vector<const MapData::TileInfo*> kinds;
        auto kindOf = [&](const MapData::TileInfo* t) {
            for (size_t k = 0; k < kinds.size(); k++) if (kinds[k] == t) return (int)k;
            kinds.push_back(t);
            return (int)kinds.size() - 1;
        };
        std::vector<int> kindAt((size_t)TW * TH, -1);
        for (int i = 0; i < TW * TH; i++)
            if (!wet[i] && mx[i].road < 0.5f) kindAt[i] = kindOf(tile[i]);
        std::vector<int> root(kinds.size());
        for (size_t k = 0; k < kinds.size(); k++) root[k] = (int)k;
        auto find = [&](int k) { while (root[k] != k) { root[k] = root[root[k]]; k = root[k]; } return k; };
        for (size_t a = 0; a < kinds.size(); a++)
            for (size_t b = a + 1; b < kinds.size(); b++) {
                const float dr = (float)kinds[a]->r - kinds[b]->r, dg = (float)kinds[a]->g - kinds[b]->g, db = (float)kinds[a]->b - kinds[b]->b;
                if (dr * dr + dg * dg + db * db < 46.f * 46.f) root[find((int)a)] = find((int)b);
            }
        std::vector<uint32_t> name(kinds.size(), 0xffffffffu);
        for (size_t k = 0; k < kinds.size(); k++) { const int r = find((int)k); name[r] = std::min(name[r], kinds[k]->hash); }
        for (int i = 0; i < TW * TH; i++) {
            if (wet[i]) continue;
            mx[i].area = kindAt[i] >= 0 ? name[find(kindAt[i])] : (tile[i]->hash ^ 0x5bd1e995u);
        }
        // Inside an area, each tile leans toward its neighbours of the same
        // area, so the mottling stays gentle.
        std::vector<float> soft((size_t)TW * TH * 3, 0.f);
        for (int ty = 0; ty < TH; ty++)
            for (int tx = 0; tx < TW; tx++) {
                const size_t i = (size_t)ty * TW + tx;
                const Mix3& m = mx[i];
                if (m.water > 0.5f) continue;
                float ar = 0, ag = 0, ab = 0, aw = 0;
                for (int dy = -2; dy <= 2; dy++)
                    for (int dx = -2; dx <= 2; dx++) {
                        const int x = tx + dx, y = ty + dy;
                        if (x < 0 || y < 0 || x >= TW || y >= TH) continue;
                        const Mix3& o = mx[(size_t)y * TW + x];
                        if (o.water > 0.5f || o.area != m.area) continue;
                        const float k = 1.f / (1.f + 0.5f * (float)(abs(dx) + abs(dy)));
                        ar += o.gr * k; ag += o.gg * k; ab += o.gb * k; aw += k;
                    }
                soft[i * 3] = m.gr + (ar / aw - m.gr) * 0.7f;
                soft[i * 3 + 1] = m.gg + (ag / aw - m.gg) * 0.7f;
                soft[i * 3 + 2] = m.gb + (ab / aw - m.gb) * 0.7f;
            }
        for (int i = 0; i < TW * TH; i++)
            if (mx[i].water < 0.5f) { mx[i].gr = soft[i * 3]; mx[i].gg = soft[i * 3 + 1]; mx[i].gb = soft[i * 3 + 2]; }
    }
    int minX = TW, minY = TH, maxX = -1, maxY = -1;
    for (int ty = 0; ty < TH; ty++)
        for (int tx = 0; tx < TW; tx++) {
            const size_t ti = (size_t)ty * TW + tx;
            // A tile's pixels depend on the 3 x 3 tiles around it (and, through
            // their softened colours and areas, on the tiles around those).
            uint32_t key = 2166136261u;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    const size_t k = at(tx + dx, ty + dy);
                    const Mix3& m = mx[k];
                    const uint32_t col = ((uint32_t)m.gr << 16) ^ ((uint32_t)m.gg << 8) ^ (uint32_t)m.gb;
                    key = (key ^ ((tile[k] ? tile[k]->hash : 0u) * 2654435761u ^ ((uint32_t)dist[k] << 24))) * 16777619u;
                    key = (key ^ (col * 0x9e3779b1u ^ m.area)) * 16777619u;
                }
            if (!full && sKey[ti] == key) continue;
            sKey[ti] = key;
            minX = std::min(minX, tx); maxX = std::max(maxX, tx);
            minY = std::min(minY, ty); maxY = std::max(maxY, ty);
            // Square tiles, as the game lays the island out: each tile its own
            // colour; a one-pixel seam where it meets another ground, a road or
            // the sea, and a crisp line of foam on the water side of the coast.
            const Mix3& m = mx[ti];
            const size_t nb[4] = { at(tx, ty - 1), at(tx + 1, ty), at(tx, ty + 1), at(tx - 1, ty) };   // up, right, down, left
            auto isLand = [&](size_t j) { return mx[j].water < 0.5f; };
            auto differs = [&](size_t j) {
                const Mix3& o = mx[j];
                return o.water > 0.5f || o.area != m.area || (o.road > 0.5f) != (m.road > 0.5f);
            };
            const bool dU = differs(nb[0]), dR = differs(nb[1]), dD = differs(nb[2]), dL = differs(nb[3]);
            const bool lU = isLand(nb[0]), lR = isLand(nb[1]), lD = isLand(nb[2]), lL = isLand(nb[3]);
            for (int py = 0; py < P; py++)
                for (int pxl = 0; pxl < P; pxl++) {
                    const int gx = tx * P + pxl, gy = ty * P + py;
                    uint32_t hsh = (uint32_t)gx * 73856093u ^ (uint32_t)gy * 19349663u;
                    hsh = (hsh ^ (hsh >> 13)) * 1274126177u;
                    const float noise = ((hsh >> 8) & 0xff) / 127.5f - 1.f;   // -1 … 1
                    const bool eU = py == 0, eR = pxl == P - 1, eD = py == P - 1, eL = pxl == 0;
                    float r, g, b;
                    if (m.water > 0.5f) {
                        // Sea: lighter near the shore, deeper out, a faint ripple;
                        // foam along the land it touches.
                        const float kk = 1.14f - 0.32f * m.deep;
                        const float rip = 1.f + 0.03f * sinf(gx * 0.55f + gy * 0.35f);
                        r = m.wr * kk * rip; g = m.wg * kk * rip; b = m.wb * (kk + 0.05f) * rip;
                        const bool f1 = (eU && lU) || (eR && lR) || (eD && lD) || (eL && lL);
                        const bool f2 = (py == 1 && lU) || (pxl == P - 2 && lR) || (py == P - 2 && lD) || (pxl == 1 && lL);
                        const float foam = f1 ? 0.6f : f2 ? 0.22f : 0.f;
                        r += (242 - r) * foam; g += (248 - g) * foam; b += (250 - b) * foam;
                    } else {
                        const bool road = m.road > 0.5f;
                        r = road ? m.rr : m.gr; g = road ? m.rg : m.gg; b = road ? m.rb : m.gb;
                        const float grain = 1.f + (road ? 0.015f : 0.035f) * noise;
                        r *= grain; g *= grain; b *= grain;
                        // The seam: this tile's own edge, a little darker where
                        // something else starts (the coast a little more).
                        float k = 1.f;
                        if ((eU && dU) || (eR && dR) || (eD && dD) || (eL && dL)) k = 0.86f;
                        if ((eU && !lU) || (eR && !lR) || (eD && !lD) || (eL && !lL)) k = 0.8f;
                        r *= k; g *= k; b *= k;
                    }
                    sPix[(size_t)gy * W + gx] = pack(r, g, b);
                }
        }
    if (maxX < 0) return;
    const SDL_Rect dirty{ minX * P, minY * P, (maxX - minX + 1) * P, (maxY - minY + 1) * P };
    SDL_UpdateTexture(sMapTex, &dirty, &sPix[(size_t)dirty.y * W + dirty.x], W * 4);
    const uint32_t c0 = sPix[(size_t)(P / 2) * W + P / 2];
    sMapSea = SDL_Color{ (Uint8)(c0 & 0xff), (Uint8)((c0 >> 8) & 0xff), (Uint8)((c0 >> 16) & 0xff), 255 };
}

// The island as a small picture for Home: Map.sav is read when it is not yet,
// then baked like the canvas.
static SDL_Texture* MapThumb() {
    if (!gMapSav.loaded) {
        static double sTry = -10;
        if (Motion::Now() - sTry < 5.0) return nullptr;
        sTry = Motion::Now();
        std::string err;
        SaveEditor::Load(SAVE_MAP_SAV, gMapSav, err);
        if (!gMapSav.loaded) return nullptr;
    }
    MapBake();
    return sMapTex;
}

// The objects' footprints, rebuilt when an object array changed.
static void MapObjects() {
    static const uint32_t hAct = SaveEditor::Hash(MapKeys::ActorKey), hX = SaveEditor::Hash(MapKeys::GridPosX);
    static const uint32_t hY = SaveEditor::Hash(MapKeys::GridPosY), hR = SaveEditor::Hash(MapKeys::RotY);
    uint32_t sig = 2166136261u;
    sig = PayloadSig(MapEntry(hAct), sig); sig = PayloadSig(MapEntry(hX), sig);
    sig = PayloadSig(MapEntry(hY), sig); sig = PayloadSig(MapEntry(hR), sig);
    if (sig == sMapObjSig && !sMapObjs.empty()) return;
    sMapObjSig = sig;
    sMapObjs.clear();
    const int n = SaveEditor::ArraySize(gMapSav, hAct);
    for (int i = 0; i < n; i++) {
        const uint32_t a = SaveEditor::GetUIntAt(gMapSav, hAct, i, 0);
        if (!a) continue;
        const MapData::ActorInfo* info = MapData::ActorLookup(a);
        const MapFootprint fp = MapActorRect(info, SaveEditor::GetFloatAt(gMapSav, hR, i, 0.0f));
        const int turns = ((int)std::lround(SaveEditor::GetFloatAt(gMapSav, hR, i, 0.0f) / 90.0f) % 4 + 4) % 4;
        sMapObjs.push_back({ i, SaveEditor::GetIntAt(gMapSav, hX, i, 0) + fp.x0, SaveEditor::GetIntAt(gMapSav, hY, i, 0) + fp.y0, fp.w, fp.h,
                             info ? (int)info->group : (int)MapData::ACTOR_UNKNOWN, a, turns });
    }
}

// Zoomed out, the island covers the canvas (no bands at its sides); each step
// closer, bigger.
static int MapPx() {
    const int base = std::max((sMapCW + MAP_WIDTH - 1) / MAP_WIDTH, (sMapCH + MAP_HEIGHT - 1) / MAP_HEIGHT);
    static const float kStep[4] = { 1.f, 1.6f, 2.3f, 3.2f };
    return std::max(1, (int)lroundf(base * kStep[std::max(1, std::min(4, gMapZoom)) - 1]));
}
static void MapMetrics(int& cols, int& rows) {
    const int px = MapPx();
    cols = std::min(MAP_WIDTH, sMapCW / px);
    rows = std::min(MAP_HEIGHT, sMapCH / px);
}
// How far the view goes: the island's edge as far as the floating tools'
// edge and no further, so any tile can be brought out from under them.
static void MapViewRange(float& x0, float& x1, float& y0, float& y1) {
    const float px = (float)MapPx();
    x0 = 0.f;
    x1 = std::max(0.f, MAP_WIDTH - (sMapCW - sMapSafeR) / px);
    y0 = -sMapSafeT / px;
    y1 = std::max(0.f, MAP_HEIGHT - (sMapCH - sMapSafeB) / px);
}
static void MapViewRangeI(int& x0, int& x1, int& y0, int& y1) {
    float a, b, c, d;
    MapViewRange(a, b, c, d);
    x0 = (int)floorf(a); x1 = (int)ceilf(b); y0 = (int)floorf(c); y1 = (int)ceilf(d);
}
static void MapClampViewF(float& vx, float& vy) {
    float x0, x1, y0, y1;
    MapViewRange(x0, x1, y0, y1);
    vx = std::max(x0, std::min(vx, x1));
    vy = std::max(y0, std::min(vy, y1));
}

// The view follows the cursor, keeping a few tiles of room ahead of it, and
// clear of the toolbar, the inspector and the button bar.
static void MapFollow() {
    int cols, rows;
    MapMetrics(cols, rows);
    const int px = MapPx();
    const int t = (sMapSafeT + px - 1) / px, r = (sMapSafeR + px - 1) / px, b = (sMapSafeB + px - 1) / px;
    const int m = std::max(1, std::min(cols - r, rows - t - b) / 8);
    if (gMapCursorX < gMapViewX + m) gMapViewX = gMapCursorX - m;
    if (gMapCursorX >= gMapViewX + cols - r - m) gMapViewX = gMapCursorX - cols + r + m + 1;
    if (gMapCursorY < gMapViewY + t + m) gMapViewY = gMapCursorY - t - m;
    if (gMapCursorY >= gMapViewY + rows - b - m) gMapViewY = gMapCursorY - rows + b + m + 1;
    int x0, x1, y0, y1;
    MapViewRangeI(x0, x1, y0, y1);
    gMapViewX = std::max(x0, std::min(x1, gMapViewX));
    gMapViewY = std::max(y0, std::min(y1, gMapViewY));
}

// Put the object being placed down at the cursor (1.x's place, as is).
static void MapPlaceHere() {
    const uint32_t hAct = SaveEditor::Hash(MapKeys::ActorKey);
    const int n = SaveEditor::ArraySize(gMapSav, hAct);
    int slot = -1;
    for (int i = 0; i < n; i++) if (SaveEditor::GetUIntAt(gMapSav, hAct, i, 0) == 0) { slot = i; break; }
    if (slot < 0) { MapSetMsg(Lang::T("map.no.empty.slot"), COL_RED, 180); return; }
    const int oldX = SaveEditor::GetIntAt(gMapSav, SaveEditor::Hash(MapKeys::GridPosX), slot, 0);
    const int oldY = SaveEditor::GetIntAt(gMapSav, SaveEditor::Hash(MapKeys::GridPosY), slot, 0);
    const float oldR = SaveEditor::GetFloatAt(gMapSav, SaveEditor::Hash(MapKeys::RotY), slot, 0.0f);
    const int oldL = SaveEditor::GetIntAt(gMapSav, SaveEditor::Hash(MapKeys::LinkedMapId), slot, -1);
    SaveEditor::SetUIntAt(gMapSav, hAct, slot, gMapPlaceActor);
    SaveEditor::SetIntAt(gMapSav, SaveEditor::Hash(MapKeys::GridPosX), slot, gMapCursorX);
    SaveEditor::SetIntAt(gMapSav, SaveEditor::Hash(MapKeys::GridPosY), slot, gMapCursorY);
    SaveEditor::SetFloatAt(gMapSav, SaveEditor::Hash(MapKeys::RotY), slot, 0.0f);
    SaveEditor::SetIntAt(gMapSav, SaveEditor::Hash(MapKeys::LinkedMapId), slot, -1);
    MapHistEntry he; he.label = "place";
    he.objs.push_back({ slot, 0, 0, (int64_t)gMapPlaceActor });
    he.objs.push_back({ slot, 1, (int64_t)oldX, (int64_t)gMapCursorX });
    he.objs.push_back({ slot, 2, (int64_t)oldY, (int64_t)gMapCursorY });
    he.objs.push_back({ slot, 3, (int64_t)(int)oldR, 0 });
    he.objs.push_back({ slot, 4, (int64_t)oldL, (int64_t)-1 });
    MapHistPush(std::move(he));
    gMapSelSlot = slot;
    gMapDirty = true;
    gMapMode = MapMode::Inspect;
    Sound::Play(Sound::Voice::Land);
}

static void MapDeleteSelected() {
    if (gMapSelSlot < 0) return;
    const uint32_t hAct = SaveEditor::Hash(MapKeys::ActorKey);
    const uint32_t oldAct = SaveEditor::GetUIntAt(gMapSav, hAct, gMapSelSlot, 0);
    SaveEditor::SetUIntAt(gMapSav, hAct, gMapSelSlot, 0);
    MapHistEntry he; he.label = "delete";
    he.objs.push_back({ gMapSelSlot, 0, (int64_t)oldAct, 0 });
    MapHistPush(std::move(he));
    gMapDirty = true;
    gMapSelSlot = -1;
    gMapMode = MapMode::Idle;
    Sound::Play(Sound::Voice::Delete);
}

// ── Exploring ────────────────────────────────────────────────────────────────
// Exploring, the canvas owns the controls: the left stick moves the player's
// hand freely over the island (the D-pad a tile at a time) and the view glides
// after it; A picks up the object under the hand and puts it down again, X
// turns what is held, B puts it back, Y jumps to the next object, ZL and ZR
// zoom on the hand. Not exploring, the map rests under a veil and A starts, so
// moving the highlight onto it never takes the D-pad by surprise.
static bool  sMapExplore = false;
static bool  sMapCarry = false;               // an object is in the hand
static int   sCarrySlot = -1, sCarryDX = 0, sCarryDY = 0, sCarryRot = 0, sCarryRot0 = 0;
static float sViewXf = -1.f, sViewYf = -1.f;  // the view as drawn, gliding to gMapViewX/Y
static float sCurXf = -1.f, sCurYf = -1.f;    // the hand as drawn, gliding to the cursor
static float sMapPanX = 0.f, sMapPanY = 0.f;  // a finger's drag not yet a whole tile

// The hand to the tile under a finger, as the view is drawn (it may still be
// gliding).
static void MapCursorToFinger(const SDL_Rect& c, float fx, float fy) {
    const int px = MapPx();
    const int ax = c.x, ay = c.y;
    float vx = sViewXf >= 0.f ? sViewXf : (float)gMapViewX, vy = sViewYf >= 0.f ? sViewYf : (float)gMapViewY;
    MapClampViewF(vx, vy);                                              // as MapCanvas draws it
    gMapCursorX = std::max(0, std::min(MAP_WIDTH - 1, (int)floorf(vx + (fx - ax) / px)));
    gMapCursorY = std::max(0, std::min(MAP_HEIGHT - 1, (int)floorf(vy + (fy - ay) / px)));
}
// A finger dragging the view, a tile at a time; the hand rides along so it
// stays where it was on screen.
static void MapPanBy(float ddx, float ddy) {
    int x0, x1, y0, y1;
    MapViewRangeI(x0, x1, y0, y1);
    const float px = (float)MapPx();
    sMapPanX += ddx; sMapPanY += ddy;
    while (sMapPanX >= px) { sMapPanX -= px; if (gMapViewX > x0) { gMapViewX--; gMapCursorX = std::max(0, gMapCursorX - 1); } }
    while (sMapPanX <= -px) { sMapPanX += px; if (gMapViewX < x1) { gMapViewX++; gMapCursorX = std::min(MAP_WIDTH - 1, gMapCursorX + 1); } }
    while (sMapPanY >= px) { sMapPanY -= px; if (gMapViewY > y0) { gMapViewY--; gMapCursorY = std::max(0, gMapCursorY - 1); } }
    while (sMapPanY <= -px) { sMapPanY += px; if (gMapViewY < y1) { gMapViewY++; gMapCursorY = std::min(MAP_HEIGHT - 1, gMapCursorY + 1); } }
}

static void MapCarryBegin(int slot) {
    const int ax = SaveEditor::GetIntAt(gMapSav, SaveEditor::Hash(MapKeys::GridPosX), slot, 0);
    const int ay = SaveEditor::GetIntAt(gMapSav, SaveEditor::Hash(MapKeys::GridPosY), slot, 0);
    const int rot = (int)std::lround(SaveEditor::GetFloatAt(gMapSav, SaveEditor::Hash(MapKeys::RotY), slot, 0.0f));
    sCarrySlot = slot;
    sCarryDX = ax - gMapCursorX;
    sCarryDY = ay - gMapCursorY;
    sCarryRot0 = sCarryRot = ((rot % 360) + 360) % 360;
    sMapCarry = true;
    Sound::Play(Sound::Voice::Grab);
}
// Put what is held down where the hand is: one undo step for the move and the
// turn together (nothing at all when it went back where it was).
static void MapCarryDrop() {
    if (!sMapCarry) return;
    sMapCarry = false;
    const uint32_t hX = SaveEditor::Hash(MapKeys::GridPosX), hY = SaveEditor::Hash(MapKeys::GridPosY);
    const uint32_t hR = SaveEditor::Hash(MapKeys::RotY);
    const int s = sCarrySlot;
    const int ox = SaveEditor::GetIntAt(gMapSav, hX, s, 0), oy = SaveEditor::GetIntAt(gMapSav, hY, s, 0);
    const int nx = std::max(0, std::min(MAP_WIDTH - 1, gMapCursorX + sCarryDX));
    const int ny = std::max(0, std::min(MAP_HEIGHT - 1, gMapCursorY + sCarryDY));
    MapHistEntry he; he.label = "move";
    if (nx != ox) { SaveEditor::SetIntAt(gMapSav, hX, s, nx); he.objs.push_back({ s, 1, (int64_t)ox, (int64_t)nx }); }
    if (ny != oy) { SaveEditor::SetIntAt(gMapSav, hY, s, ny); he.objs.push_back({ s, 2, (int64_t)oy, (int64_t)ny }); }
    if (sCarryRot != sCarryRot0) {
        SaveEditor::SetFloatAt(gMapSav, hR, s, (float)sCarryRot);
        he.objs.push_back({ s, 3, (int64_t)sCarryRot0, (int64_t)sCarryRot });
    }
    if (!he.objs.empty()) { MapHistPush(std::move(he)); gMapDirty = true; }
    Sound::Play(Sound::Voice::Land);
}
static void MapCarryCancel() {
    if (!sMapCarry) return;
    sMapCarry = false;
    Sound::Play(Sound::Voice::Release);
}
static void MapCarryTurn() {
    if (!sMapCarry) return;
    sCarryRot = (sCarryRot + 90) % 360;
    Sound::Play(Sound::Voice::Tick, { 0.6f });
}

// The object under the hand (its footprint holds the cursor), or nullptr.
static const MapObjRect* MapHovered() {
    for (const auto& o : sMapObjs)
        if (gMapCursorX >= o.x && gMapCursorX < o.x + o.w && gMapCursorY >= o.y && gMapCursorY < o.y + o.h &&
            !(sMapCarry && o.slot == sCarrySlot))
            return &o;
    return nullptr;
}

// Y: the hand to the next object, in reading order from where it is.
static void MapNextObject() {
    if (sMapObjs.empty()) return;
    const MapObjRect* cur = MapHovered();
    auto key = [](const MapObjRect& o) { return o.y * MAP_WIDTH + o.x; };
    const int from = cur ? key(*cur) : gMapCursorY * MAP_WIDTH + gMapCursorX;
    const MapObjRect* best = nullptr;
    const MapObjRect* first = nullptr;
    for (const auto& o : sMapObjs) {
        if (!first || key(o) < key(*first)) first = &o;
        if (key(o) > from && (!best || key(o) < key(*best))) best = &o;
    }
    if (!best) best = first;
    gMapCursorX = best->x + (best->w - 1) / 2;
    gMapCursorY = best->y + (best->h - 1) / 2;
    MapFollow();
    Sound::Play(Sound::Voice::Tick, { 0.3f + 0.4f * gMapCursorX / (float)MAP_WIDTH, true });
}

// ZL / ZR: closer or further, the hand staying in the middle.
static void MapZoomTo(int z) {
    z = std::max(1, std::min(4, z));
    if (z == gMapZoom) { Sound::Play(Sound::Voice::Seat, { 0.5f, true, 0.6f }); return; }
    gMapZoom = z;
    int cols, rows;
    MapMetrics(cols, rows);
    // The hand in the middle of what the floating tools leave in view.
    const int px = MapPx();
    const int t = sMapSafeT / px, r = sMapSafeR / px, b = sMapSafeB / px;
    int x0, x1, y0, y1;
    MapViewRangeI(x0, x1, y0, y1);
    gMapViewX = std::max(x0, std::min(x1, gMapCursorX - (cols - r) / 2));
    gMapViewY = std::max(y0, std::min(y1, gMapCursorY - t - (rows - t - b) / 2));
    sViewXf = (float)gMapViewX; sViewYf = (float)gMapViewY;   // a new scale: no glide across it
    Sound::Play(Sound::Voice::Tick, { z / 4.f });
}

// What A does on the canvas.
static void MapAct() {
    if (gMapMode == MapMode::Place) { MapPlaceHere(); return; }
    if (gMapTool == MapTool::Paint) { Sound::Play(Sound::Voice::Tap); MapStrokeBegin(); MapPaintLineTo(gMapCursorX, gMapCursorY); return; }
    if (sMapCarry) { MapCarryDrop(); return; }
    const MapObjRect* h = MapHovered();
    const int slot = h ? h->slot : MapObjectAt(gMapCursorX, gMapCursorY, 1);
    if (slot >= 0) {
        gMapSelSlot = slot; gMapMode = MapMode::Inspect; gMapTool = MapTool::Select;
        if (sMapExplore) MapCarryBegin(slot); else Sound::Play(Sound::Voice::Select);
    } else {
        gMapSelSlot = -1; gMapMode = MapMode::Idle;
        Sound::Play(Sound::Voice::Seat, { 0.4f, false, 0.6f });
    }
}

// One object as a little block: a shadow (longer when lifted), the block in
// its group's colour, a lit top, a darker rim, and on a big enough house a roof.
// A ground's name in the app's language ("Grass", "Grass (path)").
static std::string MapTileName(uint32_t h) {
    const MapData::TileInfo* ti = MapData::TileLookup(h);
    if (!ti || !ti->code) return "?";
    std::string code = ti->code;
    const size_t p = code.find("_Road");
    const bool road = p != std::string::npos;
    if (road) code = code.substr(0, p);
    const std::string key = "map.floor." + code;
    std::string name = Tx(key.c_str());
    if (name == key) name = code;
    return road ? TokenFill(Tx("map.floor.road"), "{name}", name) : name;
}

// Close up, an object is its picture (the game's render) standing on its
// plot, over a soft shadow; the plot's outline shows when it is chosen.
static void MapDrawPicture(const SDL_Rect& b, SDL_Texture* pic, int pw, int ph, int group, bool sel, bool hover, float lift) {
    const int up = (int)lroundf(lift * 7.f);
    const int rr = std::max(2, std::min(b.w, b.h) / 3);
    const SDL_Color gc = MAP_GROUP_SDL_COL[std::max(0, std::min(6, group))];
    // The plot: a faint tint of its kind (small things only: a building
    // fills its own), and the shadow it casts.
    const bool big = std::min(b.w, b.h) >= 2 * MapPx();
    if (!big || sel || hover) FillRectRounded(b.x, b.y, b.w, b.h, rr, Alpha(gc, sel || hover ? 90 : 30));
    // The picture in the middle of its plot, as big as fits (a little past it).
    const float k = std::min(b.w * 1.15f / pw, b.h * 1.15f / ph);
    const int dw = std::max(1, (int)lroundf(pw * k)), dh = std::max(1, (int)lroundf(ph * k));
    // A soft shadow under it.
    const int shW = std::max(2, dw * 4 / 5), shH = std::max(2, std::min(b.h / 3, dh / 4));
    FillRectRounded(b.x + (b.w - shW) / 2, b.y + (b.h + dh) / 2 - shH / 2 + (int)lroundf(lift * 3.f), shW, shH, shH / 2,
                    SDL_Color{ 0, 0, 0, (Uint8)(60 - 20 * lift) });
    const SDL_Rect r{ b.x + (b.w - dw) / 2, b.y + (b.h - dh) / 2 - up, dw, dh };
    DrawTexture(pic, nullptr, r);
    if (hover) StrokeRectRounded((float)b.x - 2.f, (float)b.y - 2.f, (float)b.w + 4.f, (float)b.h + 4.f, (float)rr + 2.f, 2.f,
                                 SDL_Color{ 255, 255, 255, 210 });
    if (sel) StrokeRectRounded((float)b.x - 3.f, (float)b.y - 3.f, (float)b.w + 6.f, (float)b.h + 6.f, (float)rr + 3.f, 2.5f, gTheme.gold);
}

// A big facility seen from straight above (romfs:/items/Top_<actor>.png,
// tools/assets/build_map_tops.py): it covers its whole plot the way the game
// builds it, rather than a small front view in the middle of a large plot.
static SDL_Texture* MapActorTop(uint32_t actor, int* w, int* h) {
    const MapData::ActorInfo* info = MapData::ActorLookup(actor);
    if (!info || !info->key) { if (w) *w = 0; if (h) *h = 0; return nullptr; }
    return Art::ItemPicture((std::string("Top_") + info->key).c_str(), w, h);
}
// Over plot `b`, turned `turns` quarter turns the way MapActorRect turns the
// plot (one is a quarter turn anticlockwise on the map).
static void MapDrawTop(const SDL_Rect& b, SDL_Texture* pic, int turns, bool sel, bool hover, float lift) {
    const int up = (int)lroundf(lift * 7.f);
    const bool odd = (turns & 1) != 0;
    const SDL_Rect d = odd ? SDL_Rect{ b.x + b.w / 2 - b.h / 2, b.y + b.h / 2 - b.w / 2 - up, b.h, b.w } : SDL_Rect{ b.x, b.y - up, b.w, b.h };
    if (lift > 0.f) {
        const int sh = std::max(2, std::min(b.w, b.h) / 12);
        FillRectRounded(b.x + sh, b.y + sh, b.w, b.h, std::max(2, std::min(b.w, b.h) / 8), SDL_Color{ 0, 0, 0, (Uint8)(50 + 40 * lift) });
    }
    const SDL_Rect x = XformRect(d.x, d.y, d.w, d.h);
    SDL_RenderCopyEx(gRen, pic, nullptr, &x, -90.0 * turns, nullptr, SDL_FLIP_NONE);
    const int rr = std::max(2, std::min(b.w, b.h) / 8);
    if (hover) StrokeRectRounded((float)b.x - 2.f, (float)b.y - 2.f - up, (float)b.w + 4.f, (float)b.h + 4.f, (float)rr + 2.f, 2.f,
                                 SDL_Color{ 255, 255, 255, 210 });
    if (sel) StrokeRectRounded((float)b.x - 3.f, (float)b.y - 3.f - up, (float)b.w + 6.f, (float)b.h + 6.f, (float)rr + 3.f, 2.5f, gTheme.gold);
}

static void MapDrawBlock(const SDL_Rect& b, int group, bool sel, bool hover, float lift) {
    const SDL_Color gc = Mix(MAP_GROUP_SDL_COL[std::max(0, std::min(6, group))], SDL_Color{ 255, 255, 255, 255 }, 0.12f);
    const int rr = std::max(1, std::min(b.w, b.h) / 4);
    const int sh = std::max(1, std::min(b.w, b.h) / 10) + (int)lroundf(lift * 6.f);
    const int up = (int)lroundf(lift * 7.f);
    FillRectRounded(b.x + sh, b.y + sh, b.w, b.h, rr, SDL_Color{ 0, 0, 0, (Uint8)(70 + 40 * lift) });
    const SDL_Rect r{ b.x, b.y - up, b.w, b.h };
    FillRectRounded(r.x, r.y, r.w, r.h, rr, Alpha(gc, sel || hover ? 250 : 228));
    if (r.h >= 6) FillRectRounded(r.x + 1, r.y + 1, r.w - 2, std::max(2, r.h / 3), std::max(1, rr - 1),
                                  Alpha(Mix(gc, SDL_Color{ 255, 255, 255, 255 }, 0.45f), 120));
    DrawRectRounded(r.x, r.y, r.w, r.h, rr, Alpha(Mix(gc, SDL_Color{ 0, 0, 0, 255 }, 0.45f), 235));
    if (group == 0 && std::min(r.w, r.h) >= 18)
        Art::Icon(Art::Id::IcoHome, r.x + r.w / 2, r.y + r.h / 2, std::min(r.w, r.h) * 6 / 10, SDL_Color{ 255, 255, 255, 215 });
    if (hover) StrokeRectRounded((float)r.x - 2.f, (float)r.y - 2.f, (float)r.w + 4.f, (float)r.h + 4.f, (float)rr + 2.f, 2.f,
                                 SDL_Color{ 255, 255, 255, 210 });
    if (sel) StrokeRectRounded((float)r.x - 3.f, (float)r.y - 3.f, (float)r.w + 6.f, (float)r.h + 6.f, (float)rr + 3.f, 2.5f, gTheme.gold);
}

// The player's hand over the map: the hand and the wrist, the arm fading out
// above. (gx, gy) is where the fingers meet; dh the whole picture's height.
static void MapDrawHand(bool closed, int gx, int gy, int dh) {
    // Where the fingers meet in each skin tone's photo, across its width.
    static const float kOpenX[6] = { 0.46f, 0.54f, 0.37f, 0.38f, 0.40f, 0.43f };
    static const float kCloseX[6] = { 0.62f, 0.59f, 0.37f, 0.46f, 0.43f, 0.52f };
    const int tone = std::max(0, std::min(5, Art::SkinTone()));
    int hw = 0, hh = 0;
    SDL_Texture* ht = Art::Get(closed ? Art::HandClose(tone) : Art::HandOpen(tone), &hw, &hh);
    if (!ht || hh <= 0) return;
    const float gxf = closed ? kCloseX[tone] : kOpenX[tone], gyf = closed ? 0.975f : 0.92f;
    const int dw = hw * dh / hh;
    const int left = gx - (int)lroundf(gxf * dw), top = gy - (int)lroundf(gyf * dh);
    // The arm from 40% of the picture down: fading in over thin strips, then
    // the rest (the hand) in one piece.
    const int y0 = hh * 2 / 5, y1 = hh * 3 / 5, strips = 12;
    auto piece = [&](int s0, int s1, float alpha) {
        const int d0 = top + s0 * dh / hh, d1 = top + s1 * dh / hh;
        if (d1 <= d0) return;
        const SDL_Rect src{ 0, s0, hw, s1 - s0 };
        DrawTexture(ht, &src, SDL_Rect{ left, d0, dw, d1 - d0 }, (uint8_t)lroundf(255.f * alpha));
    };
    for (int k = 0; k < strips; k++)
        piece(y0 + (y1 - y0) * k / strips, y0 + (y1 - y0) * (k + 1) / strips, (k + 0.5f) / strips);
    piece(y1, hh, 1.f);
}

// The objects, drawn once into a picture of the whole island at this zoom
// (again only when they change, or when one is in the hand): a frame then
// draws that one picture, however many objects there are.
static SDL_Texture* sObjLayer = nullptr;
static int sObjLayerPx = 0, sObjLayerSkip = -2;
static uint32_t sObjLayerSig = 0;
static std::vector<const MapObjRect*> sObjOrder;          // back to front
constexpr int kLayerPad = 2;                              // tiles around the island
static SDL_BlendMode MapLayerBlend() {
    // What was drawn into it is premultiplied: laid on as such.
    static const SDL_BlendMode m = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD,
                                                              SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD);
    return m;
}
static void MapObjLayer(int px) {
    const int skip = sMapCarry ? sCarrySlot : -1;
    if (sObjLayer && sObjLayerPx == px && sObjLayerSig == sMapObjSig && sObjLayerSkip == skip) return;
    const int W = (MAP_WIDTH + 2 * kLayerPad) * px, H = (MAP_HEIGHT + 2 * kLayerPad) * px;
    if (!sObjLayer || sObjLayerPx != px) {
        if (sObjLayer) SDL_DestroyTexture(sObjLayer);
        sObjLayer = SDL_CreateTexture(gRen, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, W, H);
        if (!sObjLayer) return;
        SDL_SetTextureBlendMode(sObjLayer, MapLayerBlend());
        SDL_SetTextureScaleMode(sObjLayer, SDL_ScaleModeNearest);
    }
    sObjLayerPx = px; sObjLayerSig = sMapObjSig; sObjLayerSkip = skip;
    sObjOrder.clear();
    for (const auto& o : sMapObjs) sObjOrder.push_back(&o);
    std::stable_sort(sObjOrder.begin(), sObjOrder.end(), [](const MapObjRect* a, const MapObjRect* b) { return a->y + a->h < b->y + b->h; });
    SDL_Texture* prev = SDL_GetRenderTarget(gRen);
    SDL_Rect clip{};
    const bool clipped = SDL_RenderIsClipEnabled(gRen) == SDL_TRUE;
    if (clipped) SDL_RenderGetClipRect(gRen, &clip);
    SDL_SetRenderTarget(gRen, sObjLayer);
    SDL_RenderSetClipRect(gRen, nullptr);
    SDL_SetRenderDrawBlendMode(gRen, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(gRen, 0, 0, 0, 0);
    SDL_RenderClear(gRen);
    SDL_SetRenderDrawBlendMode(gRen, SDL_BLENDMODE_BLEND);
    PushXformIdentity();
    for (const MapObjRect* o : sObjOrder) {
        if (o->slot == skip) continue;
        const SDL_Rect b{ (o->x + kLayerPad) * px, (o->y + kLayerPad) * px, o->w * px, o->h * px };
        int pw = 0, ph = 0;
        if (SDL_Texture* top = MapActorTop(o->actor, &pw, &ph)) { MapDrawTop(b, top, o->turns, false, false, 0.f); continue; }
        SDL_Texture* pic = Art::ItemPicture(MapActorArt(o->actor), &pw, &ph);
        if (pic && pw > 0 && ph > 0) MapDrawPicture(b, pic, pw, ph, o->group, false, false, 0.f);
        else MapDrawBlock(b, o->group, false, false, 0.f);
    }
    PopXform();
    SDL_SetRenderTarget(gRen, prev);
    if (clipped) SDL_RenderSetClipRect(gRen, &clip); else SDL_RenderSetClipRect(gRen, nullptr);
}

static void MapCanvas(SDL_Rect c, SDL_Rect hit) {
    const Input::Frame& in = Input::Last();
    const bool f = Focus::Is(kMapCanvasId);
    if (!f) { sMapExplore = false; MapCarryCancel(); }
    MapBake();
    MapObjects();
    const MapObjRect* hov = sMapExplore ? MapHovered() : nullptr;
    Focus::Node n;
    n.id = kMapCanvasId; n.r = hit; n.radius = 0;
    n.flags = (uint16_t)(Focus::NoPlate | Focus::OwnSound | Focus::Default | (sMapExplore ? Focus::Capture : 0));
    n.hint = !sMapExplore ? Tx("map.hint.explore")
           : gMapMode == MapMode::Place ? Tx("map.hint.place")
           : gMapTool == MapTool::Paint ? Tx("map.hint.paint")
           : sMapCarry ? Tx("map.hint.drop")
           : hov ? Tx("map.hint.pick") : Tx("map.hint.look");
    n.move = [](int dx, int dy) {
        gMapCursorX = std::max(0, std::min(MAP_WIDTH - 1, gMapCursorX + dx));
        gMapCursorY = std::max(0, std::min(MAP_HEIGHT - 1, gMapCursorY + dy));
        MapFollow();
        if (gMapStroking && gMapTool == MapTool::Paint && (Input::Last().held & Input::A)) MapPaintLineTo(gMapCursorX, gMapCursorY);
        Sound::Play(Sound::Voice::Tick, { 0.3f + 0.4f * gMapCursorX / (float)MAP_WIDTH, false, 0.45f });
    };
    n.press = [c]() {
        const Input::Frame& i2 = Input::Last();
        auto toFinger = [&]() { MapCursorToFinger(c, i2.x, i2.y); };
        if (!sMapExplore) {
            // The first press lifts the veil (a tap also puts the hand there).
            sMapExplore = true;
            Sound::Play(Sound::Voice::Open);
            if (i2.tap) toFinger();
            return;
        }
        if (i2.tap) toFinger();                         // a finger: the tile under it
        MapAct();
        if (i2.tap && gMapStroking) MapStrokeEnd();
    };
    Focus::Add(std::move(n));
    // Exploring, the sticks move the hand freely, faster the further they are
    // pushed (and the left stick stops counting as the D-pad for that).
    if (f && sMapExplore) {
        Input::StickAsAnalog();
        const float zf = 1.f / sqrtf((float)std::max(1, gMapZoom));
        auto speed = [zf](float v) {                    // tiles a second
            if (v == 0.f) return 0.f;
            const float a = fabsf(v);
            return (v < 0 ? -1.f : 1.f) * (2.5f + 37.5f * zf * a * a);
        };
        const float vx = speed(in.lx) + 1.5f * speed(in.rx), vy = speed(in.ly) + 1.5f * speed(in.ry);
        if (vx != 0.f || vy != 0.f) {
            sMapStickX += vx * Motion::Dt();
            sMapStickY += vy * Motion::Dt();
            const int mx = (int)sMapStickX, my = (int)sMapStickY;
            if (mx || my) {
                sMapStickX -= mx; sMapStickY -= my;
                gMapCursorX = std::max(0, std::min(MAP_WIDTH - 1, gMapCursorX + mx));
                gMapCursorY = std::max(0, std::min(MAP_HEIGHT - 1, gMapCursorY + my));
                MapFollow();
                if (gMapStroking && (in.held & Input::A)) MapPaintLineTo(gMapCursorX, gMapCursorY);
                // A soft tick when the hand comes onto an object.
                const MapObjRect* now = MapHovered();
                if (now && (!hov || now->slot != hov->slot))
                    Sound::Play(Sound::Voice::Tick, { 0.3f + 0.4f * gMapCursorX / (float)MAP_WIDTH, false, 0.5f });
            }
        } else {
            sMapStickX = sMapStickY = 0.f;
        }
        hov = MapHovered();
    }
    // A finger dragging on the canvas pans it (with the whole island in view,
    // the hand follows the finger instead).
    if (in.pressed) {
        sMapDragInside = in.x >= hit.x && in.y >= hit.y && in.x < hit.x + hit.w && in.y < hit.y + hit.h;
        sMapPanX = sMapPanY = 0.f;
    }
    if (sMapDragInside && in.touching && in.dragging && (in.dx != 0 || in.dy != 0)) {
        int cols, rows;
        MapMetrics(cols, rows);
        if (cols >= MAP_WIDTH && rows >= MAP_HEIGHT) { if (sMapExplore) MapCursorToFinger(c, in.x, in.y); }
        else MapPanBy(in.dx, in.dy);
    }
    if (in.released) sMapDragInside = false;
    // A let go ends a paint stroke (one undo step).
    if (gMapStroking && !(in.held & Input::A) && !in.touching) MapStrokeEnd();
    if (gMapStroking) MapBake();                         // the stroke shows as it is painted

    int cols, rows;
    MapMetrics(cols, rows);
    const int px = MapPx();
    const SDL_Rect area = c;                             // edge to edge
    // The view and the hand glide to where they should be (a finger, or reduced
    // motion, moves them at once).
    const bool snap = Motion::Reduced() || sMapDragInside || !f || in.tap;
    auto glide = [snap](float& v, int to, float rate) {
        if (v < 0.f || snap) { v = (float)to; return; }
        v += (to - v) * std::min(1.f, Motion::Dt() * rate);
        if (fabsf(to - v) < 0.02f) v = (float)to;
    };
    glide(sViewXf, gMapViewX, 11.f); glide(sViewYf, gMapViewY, 11.f);
    glide(sCurXf, gMapCursorX, 24.f); glide(sCurYf, gMapCursorY, 24.f);
    // The view in steps of one baked pixel, so the objects sit on the picture.
    float vx = roundf(sViewXf * kMapTilePx) / kMapTilePx, vy = roundf(sViewYf * kMapTilePx) / kMapTilePx;
    // No further than the range, to the pixel (the canvas need not be a whole
    // number of tiles).
    MapClampViewF(vx, vy);
    auto sx = [&](float tx) { return area.x + (int)lroundf((tx - vx) * px); };
    auto sy = [&](float ty) { return area.y + (int)lroundf((ty - vy) * px); };

    SDL_Rect outerClip{};
    const bool outerClipped = SDL_RenderIsClipEnabled(gRen) == SDL_TRUE;
    if (outerClipped) SDL_RenderGetClipRect(gRen, &outerClip);
    SDL_Rect clip = XformRect(area.x, area.y, area.w, area.h);
    if (outerClipped && !SDL_IntersectRect(&clip, &outerClip, &clip)) clip = SDL_Rect{ clip.x, clip.y, 0, 0 };
    SDL_RenderSetClipRect(gRen, &clip);
    FillRectSquare(c.x, c.y, c.w, c.h, sMapSea);         // the sea beyond the island's edge
    if (sMapTex) {
        // The part of the island in view (the view may begin off its edge).
        const float ix0 = std::max(0.f, vx), iy0 = std::max(0.f, vy);
        const float ix1 = std::min((float)MAP_WIDTH, vx + c.w / (float)px), iy1 = std::min((float)MAP_HEIGHT, vy + c.h / (float)px);
        const SDL_Rect src{ (int)lroundf(ix0 * kMapTilePx), (int)lroundf(iy0 * kMapTilePx), (int)lroundf((ix1 - ix0) * kMapTilePx),
                            (int)lroundf((iy1 - iy0) * kMapTilePx) };
        const SDL_Rect dstIsland{ sx(ix0), sy(iy0), sx(ix1) - sx(ix0), sy(iy1) - sy(iy0) };
        // Square tiles stay crisp drawn bigger than baked; smaller, smoothed.
        SDL_SetTextureScaleMode(sMapTex, px >= kMapTilePx ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
        if (src.w > 0 && src.h > 0) DrawTexture(sMapTex, &src, dstIsland);
        SDL_SetTextureScaleMode(sMapTex, SDL_ScaleModeLinear);
    }
    // Close up, a faint grid of the tiles.
    if (px >= 15) {
        const SDL_Color gl{ 0, 0, 0, 26 };
        for (int x = (int)floorf(vx); x <= (int)floorf(vx) + cols + 1; x++) FillRectSquare(sx((float)x), area.y, 1, area.h, gl);
        for (int y = (int)floorf(vy); y <= (int)floorf(vy) + rows + 1; y++) FillRectSquare(area.x, sy((float)y), area.w, 1, gl);
    }
    // The objects: their pictures at every zoom, the ones in front drawn last,
    // all in one picture (MapObjLayer); the highlighted and the chosen one get
    // their outline over it.
    {
        MapObjLayer(px);
        if (sObjLayer) {
            const int W = (MAP_WIDTH + 2 * kLayerPad) * px, H = (MAP_HEIGHT + 2 * kLayerPad) * px;
            DrawTexture(sObjLayer, nullptr, SDL_Rect{ sx((float)-kLayerPad), sy((float)-kLayerPad), W, H });
        }
        for (const auto& o : sMapObjs) {
            const bool sel = o.slot == gMapSelSlot && !sMapCarry, hv = hov && hov->slot == o.slot;
            if (!sel && !hv) continue;
            const SDL_Rect b{ sx((float)o.x), sy((float)o.y), o.w * px, o.h * px };
            const int rr = std::max(2, std::min(b.w, b.h) / 3);
            FillRectRounded(b.x, b.y, b.w, b.h, rr, Alpha(MAP_GROUP_SDL_COL[std::max(0, std::min(6, o.group))], 60));
            if (hv) StrokeRectRounded((float)b.x - 2.f, (float)b.y - 2.f, (float)b.w + 4.f, (float)b.h + 4.f, (float)rr + 2.f, 2.f,
                                      SDL_Color{ 255, 255, 255, 210 });
            if (sel) StrokeRectRounded((float)b.x - 3.f, (float)b.y - 3.f, (float)b.w + 6.f, (float)b.h + 6.f, (float)rr + 3.f, 2.5f, gTheme.gold);
        }
    }
    const float bob = Motion::Reduced() ? 0.f : 0.5f + 0.5f * sinf((float)Motion::Now() * 5.f);
    // What the hand holds: where it would land, and the block itself, lifted.
    float lift = 0.f;
    if (sMapCarry) {
        const MapData::ActorInfo* info = MapData::ActorLookup(MapActorAt(sCarrySlot));
        const MapFootprint fp = MapActorRect(info, (float)sCarryRot);
        const int ax = std::max(0, std::min(MAP_WIDTH - 1, gMapCursorX + sCarryDX));
        const int ay = std::max(0, std::min(MAP_HEIGHT - 1, gMapCursorY + sCarryDY));
        const SDL_Rect land{ sx((float)(ax + fp.x0)), sy((float)(ay + fp.y0)), fp.w * px, fp.h * px };
        const int lr = std::max(2, std::min(land.w, land.h) / 4);
        FillRectRounded(land.x, land.y, land.w, land.h, lr, Alpha(gTheme.gold, 60));
        StrokeRectRounded((float)land.x, (float)land.y, (float)land.w, (float)land.h, (float)lr, 1.5f, Alpha(gTheme.gold, 200));
        lift = 0.75f + 0.25f * bob;
        const SDL_Rect held{ sx(sCurXf + sCarryDX + fp.x0), sy(sCurYf + sCarryDY + fp.y0), fp.w * px, fp.h * px };
        int pw = 0, ph = 0;
        if (SDL_Texture* top = MapActorTop(MapActorAt(sCarrySlot), &pw, &ph)) MapDrawTop(held, top, ((sCarryRot / 90) % 4 + 4) % 4, true, false, lift);
        else if (SDL_Texture* pic = Art::ItemPicture(MapActorArt(MapActorAt(sCarrySlot)), &pw, &ph); pic && pw > 0 && ph > 0)
            MapDrawPicture(held, pic, pw, ph, info ? (int)info->group : 6, true, false, lift);
        else MapDrawBlock(held, info ? (int)info->group : 6, true, false, lift);
    }
    // What is being placed, under the hand.
    if (gMapMode == MapMode::Place) {
        const MapData::ActorInfo* info = MapData::ActorLookup(gMapPlaceActor);
        const MapFootprint fp = MapActorRect(info, 0.f);
        const int gx = sx(sCurXf + fp.x0), gy = sy(sCurYf + fp.y0);
        FillRectRounded(gx, gy, fp.w * px, fp.h * px, std::max(1, px / 2), Alpha(gTheme.gold, (int)(70 + 70 * bob)));
        StrokeRectRounded((float)gx, (float)gy, (float)(fp.w * px), (float)(fp.h * px), (float)std::max(1, px / 2), 2.f, gTheme.gold);
        int pw = 0, ph = 0;
        if (SDL_Texture* top = MapActorTop(gMapPlaceActor, &pw, &ph)) MapDrawTop(SDL_Rect{ gx, gy, fp.w * px, fp.h * px }, top, 0, false, false, 0.5f + 0.4f * bob);
        else if (SDL_Texture* pic = Art::ItemPicture(MapActorArt(gMapPlaceActor), &pw, &ph))
            if (pw > 0 && ph > 0) MapDrawPicture(SDL_Rect{ gx, gy, fp.w * px, fp.h * px }, pic, pw, ph, info ? (int)info->group : 6, false, false, 0.5f + 0.4f * bob);
    }
    // The cursor (the brush in the paint tool): a soft glowing square.
    const int b = gMapTool == MapTool::Paint ? gMapBrush : 1, half = (b - 1) / 2;
    const int cx = sx(sCurXf - half), cy = sy(sCurYf - half);
    if (!sMapCarry) {
        const SDL_Color cc = f ? gTheme.gold : Alpha(gTheme.text, 160);
        FillRectRounded(cx - 3, cy - 3, b * px + 6, b * px + 6, 4, Alpha(cc, (int)(30 + 30 * bob)));
        StrokeRectRounded((float)cx - 1.f, (float)cy - 1.f, (float)(b * px + 2), (float)(b * px + 2), 3.f, 2.f, Alpha(cc, (int)(190 + 65 * bob)));
    }
    // The player's hand: open over the ground, closed on what it holds.
    const int handH = 124;
    if (sMapExplore && gMapTool != MapTool::Paint) {
        const bool closed = sMapCarry || gMapMode == MapMode::Place;
        const float sway = Motion::Reduced() || closed ? 0.f : sinf((float)Motion::Now() * 3.f) * 1.5f;
        MapDrawHand(closed, cx + px / 2, cy + px / 2 - (int)lroundf(lift * 7.f) + (int)sway, handH);
    }
    // What is under the hand, by name, with its group's colour.
    if (sMapExplore && gMapTool == MapTool::Select && gMapMode != MapMode::Place && (hov || sMapCarry)) {
        const MapData::ActorInfo* info = MapData::ActorLookup(MapActorAt(sMapCarry ? sCarrySlot : hov->slot));
        const std::string full = MapActorDisplayLabel(info);
        const std::string name = W::Ellipsize(full, Font::Body, 280);   // its width sizes the label
        const int g = info ? std::max(0, std::min(6, (int)info->group)) : 6;
        int apw = 0, aph = 0;
        SDL_Texture* apic = Art::ItemPicture(MapActorArt(MapActorAt(sMapCarry ? sCarrySlot : hov->slot)), &apw, &aph);
        const int lh = apic ? 44 : 34, lw = (apic ? 54 : 40) + TextW(name, Font::Body) + 18;
        int lx = cx + px / 2 - lw / 2, ly = cy + px + 12;
        // Clear of the floating tools: above the hand when the bar is below it.
        if (ly + lh > area.y + area.h - sMapSafeB - 6) ly = cy - handH * 55 / 100 - lh - 6;
        lx = std::max(area.x + 6, std::min(area.x + area.w - sMapSafeR - lw - 6, lx));
        ly = std::max(area.y + sMapSafeT + 6, ly);
        FillRectRounded(lx, ly + 3, lw, lh, lh / 2, SDL_Color{ 0, 0, 0, 70 });
        FillRectRounded(lx, ly, lw, lh, lh / 2, Alpha(gTheme.panel, 242));
        if (apic) {
            Art::DrawFit(apic, SDL_Rect{ lx + 8, ly + 4, 36, lh - 8 });
            W::Marquee(Ui::HashStr("map.hand.name"), full, lx + 50, ly + lh / 2, TextW(name, Font::Body), gTheme.text, Font::Body, true,
                       W::Anchor::Middle);
        } else {
            FillDisc((float)(lx + 20), (float)(ly + lh / 2), 6.f, MAP_GROUP_SDL_COL[g]);
            W::Marquee(Ui::HashStr("map.hand.name"), full, lx + 34, ly + lh / 2, TextW(name, Font::Body), gTheme.text, Font::Body, true,
                       W::Anchor::Middle);
        }
    }
    // Not exploring: a veil over the map; highlighted, it asks for A, with the
    // player's hand pointing at the question from the edge of the map.
    {
        static Motion::Tween veil;
        const float want = sMapExplore || !f ? 0.f : 1.f;
        if (veil.to != want) veil.To(want, 0.3f, Motion::Curve::Swift);
        const float v = Motion::Reduced() ? want : veil.Value();
        if (v > 0.01f) {
            FillRectSquare(area.x, area.y, area.w, area.h, Alpha(gTheme.bg, (int)(110 * v)));
            if (f && !sMapExplore) {
                const std::string lab = Tx("map.explore");
                const int gw = W::ButtonGlyphW("A"), ph = 54, pw = 22 + gw + 12 + TextW(lab, Font::Row) + 26;
                const int bx = area.x + area.w / 2 - pw / 2, by = area.y + area.h / 2 - ph / 2;
                PushXform(0, 0, 1, 0, 0, std::min(1.f, v));
                FillRectRounded(bx, by + 6, pw, ph, ph / 2, SDL_Color{ 0, 0, 0, 70 });
                FillRectRounded(bx, by, pw, ph, ph / 2, Alpha(gTheme.panel, 245));
                StrokeRectRounded((float)bx, (float)by, (float)pw, (float)ph, ph / 2.f, 1.5f, Alpha(gTheme.gold, 170));
                W::ButtonGlyph("A", bx + 22, by + ph / 2);
                DrawTextVC(lab, bx + 22 + gw + 12, by + ph / 2, gTheme.text, Font::Row);
                int hw = 0, hh = 0;
                if (SDL_Texture* ht = Art::Get(Art::HandPoint(Art::SkinTone()), &hw, &hh)) {
                    const float wob = Motion::Reduced() ? 0.f : sinf((float)Motion::Now() * 3.2f) * 7.f;
                    const int dw = 290, dh = hh * dw / std::max(1, hw);
                    DrawTexture(ht, nullptr, SDL_Rect{ bx + pw + 4 + (int)wob, by + ph / 2 - dh / 2 + 6, dw, dh });
                }
                PopXform();
            }
        }
    }
    // A band along the bottom for the button bar, the island fading under it.
    {
        const int band = 58, fade = 36, top = c.y + c.h - band - fade;
        for (int i = 0; i < fade; i++) FillRectSquare(c.x, top + i, c.w, 1, Alpha(gTheme.bg, (i + 1) * 220 / fade));
        FillRectSquare(c.x, c.y + c.h - band, c.w, band, Alpha(gTheme.bg, 220));
    }
    if (outerClipped) SDL_RenderSetClipRect(gRen, &outerClip); else SDL_RenderSetClipRect(gRen, nullptr);
    // Where the hand is, in a see-through pill (it sits on the map): it all
    // but fades away while the hand is near it, so nothing hides under it.
    {
        char pos[32];
        snprintf(pos, sizeof pos, "%d, %d", gMapCursorX, gMapCursorY);
        const SDL_Rect pr{ c.x + 16, c.y + c.h - 108, W::PillW(pos), 28 };
        const int hx = sx(sCurXf + 0.5f), hy = sy(sCurYf + 0.5f);
        const bool near = hx > pr.x - 70 && hx < pr.x + pr.w + 70 && hy > pr.y - 70 && hy < pr.y + pr.h + 50;
        static Motion::Tween fade;
        const float want = near ? 0.18f : 1.f;
        if (fade.to != want) fade.To(want, 0.25f, Motion::Curve::Swift);
        PushXform(0, 0, 1.f, 0, 0, Motion::Reduced() ? want : fade.Value());
        FillRectRounded(pr.x, pr.y, pr.w, pr.h, 14, Alpha(gTheme.bg, 110));
        W::Pill(pr.x, pr.y, pos, Ui::Tone::Neutral);
        PopXform();
    }
    // Zoomed in, the whole island small in a corner, with the part shown.
    if (gMapZoom > 1 && sMapTex) {
        const int mw = 150, mh = mw * MAP_HEIGHT / MAP_WIDTH, mx = c.x + c.w - mw - 16, my = c.y + c.h - mh - 80;
        // (the inspector ends well above it)
        FillRectRounded(mx - 5, my - 5, mw + 10, mh + 10, 12, Alpha(gTheme.bg, 230));
        DrawTexture(sMapTex, nullptr, SDL_Rect{ mx, my, mw, mh }, 240);
        const float kx = mw / (float)MAP_WIDTH, ky = mh / (float)MAP_HEIGHT;
        const float fx0 = std::max(0.f, vx), fy0 = std::max(0.f, vy);
        const float fx1 = std::min((float)MAP_WIDTH, vx + (c.w - sMapSafeR) / (float)px);
        const float fy1 = std::min((float)MAP_HEIGHT, vy + (c.h - sMapSafeB) / (float)px);
        StrokeRectRounded(mx + fx0 * kx, my + fy0 * ky, (fx1 - fx0) * kx, (fy1 - fy0) * ky, 3.f, 1.6f, gTheme.gold);
        FillDisc(mx + (gMapCursorX + 0.5f) * kx, my + (gMapCursorY + 0.5f) * ky, 2.5f, SDL_Color{ 255, 255, 255, 255 });
    }
    // Exploring: a Done button in the corner, for touch (B does the same).
    if (sMapExplore) {
        W::Button(Ui::HashStr("map.done"), { c.x + c.w - 16 - 120, c.y + 22, 120, 52 }, Tx("map.explore.done"), W::Kind::Neutral,
                  []() { MapCarryCancel(); sMapExplore = false; if (gMapStroking) MapStrokeEnd(); Focus::Set(kMapCanvasId); }, UiIcons::Id::Check);
    }
}

// The tools, floating along the top of the map: Select, Paint and Stats,
// Add an object, undo and redo, the picture of the island, the zoom.
static void MapToolbar(int x, int y, int maxW) {
    const int h = 56, sq = 52;
    std::vector<std::pair<std::string, int>> tools = { { Tx("map.tool.select"), -1 }, { Tx("map.tool.paint"), -1 }, { Tx("map.tool.stats"), -1 } };
    const std::vector<Art::Id> icons = { Art::Id::IcoSelect, Art::Id::IcoBucket, Art::Id::IcoZoom };
    const int chipsW = std::min(W::ChipsW(tools, icons), maxW - 3 * (sq + 8) - 230 - 60);
    const int addW = std::min(230, TextW(Tx("map.add"), Font::Body) + 22 + 10 + 44);
    char zoom[16];
    snprintf(zoom, sizeof zoom, "\xC3\x97%d", std::max(1, gMapZoom));
    const int zw = TextW(zoom, Font::Body);
    const int w = chipsW + 12 + addW + 16 + 3 * sq + 2 * 8 + 16 + zw + 20;
    FillRectRounded(x - 8, y - 8 + 6, w + 16, h + 16, 22, SDL_Color{ 0, 0, 0, 70 });
    FillRectRounded(x - 8, y - 8, w + 16, h + 16, 22, Alpha(Mix(gTheme.panel, gTheme.text, 0.02f), 236));
    DrawRectRounded(x - 8, y - 8, w + 16, h + 16, 22, Alpha(gTheme.border, 230));
    W::Chips(Ui::HashStr("map.tools"), x, y, tools, (int)gMapTool, [](int i) { if (gMapStroking) MapStrokeEnd(); MapSetTool((MapTool)i); }, chipsW,
             icons);
    int bx = x + chipsW + 12;
    W::Button(Ui::HashStr("map.add"), { bx, y + 2, addW, h - 4 }, Tx("map.add"), W::Kind::Soft, []() {
        gMapPickerFilter.clear(); MapRebuildPicker(); sMapPicker = true; Motion::Forget(Ui::HashStr("map.picker"));
    }, UiIcons::Id::Plus);
    bx += addW + 16;
    const int undoX = bx;
    W::Button(Ui::HashStr("map.undo"), { bx, y + 2, sq, h - 4 }, std::string(), W::Kind::Neutral, []() { MapUndo(); Sound::Play(Sound::Voice::Undo); },
              UiIcons::Id::Undo, !gMapUndo.empty(), Focus::OwnSound);
    bx += sq + 8;
    W::Button(Ui::HashStr("map.redo"), { bx, y + 2, sq, h - 4 }, std::string(), W::Kind::Neutral,
              []() { MapRedo(); Sound::Play(Sound::Voice::Undo, { 0.7f }); }, UiIcons::Id::Redo, !gMapRedo.empty(), Focus::OwnSound);
    bx += sq + 8;
    W::Button(Ui::HashStr("map.png"), { bx, y + 2, sq, h - 4 }, std::string(), W::Kind::Neutral, []() { MapExportPng(); }, UiIcons::Id::Download);
    bx += sq + 16;
    DrawTextVC(zoom, bx, y + h / 2, Alpha(gTheme.text, 190), Font::Body);
    // What a highlighted icon does, in words, under the bar.
    const int which = Focus::Is(Ui::HashStr("map.undo")) ? 0 : Focus::Is(Ui::HashStr("map.redo")) ? 1 : Focus::Is(Ui::HashStr("map.png")) ? 2 : -1;
    if (which >= 0) {
        static const char* const kSay[3] = { "hint.undo", "map.redo", "map.png" };
        const std::string t = Tx(kSay[which]);
        const int tw = TextW(t, Font::Xs) + 24;
        const int mid = undoX + which * (sq + 8) + sq / 2;
        const int tx = std::max(x, std::min(mid - tw / 2, x + w - tw));
        FillRectRounded(tx, y + h + 16, tw, 28, 14, Alpha(Mix(gTheme.panel, gTheme.text, 0.02f), 240));
        DrawRectRounded(tx, y + h + 16, tw, 28, 14, Alpha(gTheme.border, 220));
        DrawTextVC(t, tx + 12, y + h + 30, gTheme.text, Font::Xs);
    }
}

// What there is to inspect: the chosen object, the paint brush, the counts.
static bool MapHasInspector() {
    return gMapTool != MapTool::Select || (gMapSelSlot >= 0 && MapActorAt(gMapSelSlot));
}
// How tall the inspector is, by what it shows.
static int MapInspectorH() {
    if (gMapTool == MapTool::Paint) return 2 * W::kRowHSmall + 13 + 12 + 66;
    if (gMapTool == MapTool::Stats) return 7 * 34 + 70;
    const MapData::ActorInfo* info = MapData::ActorLookup(MapActorAt(gMapSelSlot));
    const int rows = info && (info->group == 0 || info->group == 1) ? 5 : 4;
    return 58 + rows * W::kRowHSmall + (rows - 1) + 12;
}
// Floating on the map, at the right.
static void MapInspector(int x, int y, int w) {
    const int ih = MapInspectorH();
    FillRectRounded(x - 14, y - 12 + 8, w + 28, ih + 28, 24, SDL_Color{ 0, 0, 0, 70 });
    FillRectRounded(x - 14, y - 12, w + 28, ih + 28, 24, Alpha(Mix(gTheme.panel, gTheme.text, 0.02f), 236));
    DrawRectRounded(x - 14, y - 12, w + 28, ih + 28, 24, Alpha(gTheme.border, 230));
    int cy = y;
    auto card = [&](int rows) { const SDL_Rect r{ x, cy, w, rows * W::kRowHSmall + std::max(0, rows - 1) + 12 }; W::Card(r); return r; };
    auto rowAt = [&](const SDL_Rect& cr, int i) { return SDL_Rect{ cr.x + 6, cr.y + 6 + i * (W::kRowHSmall + 1), cr.w - 12, W::kRowHSmall }; };
    if (gMapTool == MapTool::Paint) {
        const SDL_Rect cr = card(2);
        const MapData::TileInfo* ti = MapData::TileLookup(gMapPaintTile);
        std::vector<SDL_Color> tc;
        if (ti) tc.push_back(SDL_Color{ ti->r, ti->g, ti->b, 255 });
        W::RowItem(Ui::HashStr("map.tile"), rowAt(cr, 0), Art::ItemPicture(MapTileArt(gMapPaintTile)), { MapTileName(gMapPaintTile), Tx("map.tile") },
                   tc, 0, nullptr, []() { sMapTilePicker = true; Motion::Forget(Ui::HashStr("map.tilemenu")); }, Tx("hint.choose"));
        W::Hairline(cr.x + 24, cr.y + 6 + W::kRowHSmall, cr.w - 48);
        const int bi = gMapBrush == 1 ? 0 : gMapBrush == 3 ? 1 : 2;
        W::RowStepper(Ui::HashStr("map.brush"), rowAt(cr, 1), { Tx("map.brush"), "" }, std::to_string(gMapBrush) + " \xC3\x97 " + std::to_string(gMapBrush),
                      bi / 2.f, 0.5f, [bi](int d) { const int n = std::max(0, std::min(2, bi + d)); gMapBrush = n == 0 ? 1 : n == 1 ? 3 : 5; });
        cy = cr.y + cr.h + 12;
        cy = Ui::TextBlockC(Tx("map.paint.how"), x + w / 2, cy, w - 20, gTheme.faint, Font::Body, 3, 2) + 14;
    } else if (gMapTool == MapTool::Stats) {
        int counts[7] = {};
        for (const auto& o : sMapObjs) counts[std::max(0, std::min(6, o.group))]++;
        static const char* const kG[] = { "map.group.house", "map.group.facility", "map.group.deco", "map.group.room", "map.group.step", "map.group.ugc", "map.group.unknown" };
        const SDL_Rect cr{ x, cy, w, 7 * 34 + 70 };
        W::Card(cr);
        W::Eyebrow(x + 20, cy + 16, TokenFill(Tx("map.stats.objects"), "{n}", std::to_string(sMapObjs.size())), gTheme.faint);
        for (int g = 0; g < 7; g++) {
            const int ry = cy + 46 + g * 34;
            FillDisc((float)(x + 28), (float)(ry + 12), 6.f, MAP_GROUP_SDL_COL[g]);
            DrawText(Tx(kG[g]), x + 44, ry + 1, gTheme.text, Font::Body);
            DrawTextR(std::to_string(counts[g]), x + w - 20, ry + 12, gTheme.dim, Font::Body);
        }
        cy = cr.y + cr.h + 12;
    } else if (gMapSelSlot >= 0 && MapActorAt(gMapSelSlot)) {
        const MapData::ActorInfo* info = MapData::ActorLookup(MapActorAt(gMapSelSlot));
        const bool linked = info && (info->group == 0 || info->group == 1);
        const int rows = linked ? 5 : 4;
        int apw = 0, aph = 0;
        if (SDL_Texture* apic = Art::ItemPicture(MapActorArt(MapActorAt(gMapSelSlot)), &apw, &aph)) {
            FillRectRounded(x, cy - 10, 56, 56, 14, Alpha(gTheme.text, 12));
            Art::DrawFit(apic, SDL_Rect{ x + 4, cy - 6, 48, 48 });
            W::Marquee(Ui::HashStr("map.sel.name"), MapActorDisplayLabel(info), x + 68, cy + 18, w - 76, gTheme.text, Font::Row, true, W::Anchor::Middle);
            cy += 58;
        } else {
            W::Marquee(Ui::HashStr("map.sel.name"), MapActorDisplayLabel(info), x + 4, cy - 2, w - 10, gTheme.text, Font::Row, true);
            cy += 30;
        }
        const SDL_Rect cr = card(rows);
        const uint32_t hX = SaveEditor::Hash(MapKeys::GridPosX), hY = SaveEditor::Hash(MapKeys::GridPosY);
        const uint32_t hR = SaveEditor::Hash(MapKeys::RotY), hL = SaveEditor::Hash(MapKeys::LinkedMapId);
        const int ox = SaveEditor::GetIntAt(gMapSav, hX, gMapSelSlot, 0), oy = SaveEditor::GetIntAt(gMapSav, hY, gMapSelSlot, 0);
        const int rot = (int)std::round(SaveEditor::GetFloatAt(gMapSav, hR, gMapSelSlot, 0.0f));
        auto nudge = [](int field) { return [field](int d) { TMapInsNudge((field << 8) | (d > 0 ? 1 : 255)); MapFollow(); }; };
        int ri = 0;
        W::RowStepper(Ui::HashStr("map.x"), rowAt(cr, ri++), { Tx("map.x"), "" }, std::to_string(ox), ox / (float)(MAP_WIDTH - 1), 1.f / (MAP_WIDTH - 1), nudge(0));
        W::RowStepper(Ui::HashStr("map.y"), rowAt(cr, ri++), { Tx("map.y"), "" }, std::to_string(oy), oy / (float)(MAP_HEIGHT - 1), 1.f / (MAP_HEIGHT - 1), nudge(1));
        W::RowStepper(Ui::HashStr("map.rot"), rowAt(cr, ri++), { Tx("map.rot"), "" }, std::to_string(((rot % 360) + 360) % 360) + "\xC2\xB0", 0.5f, 0.f, nudge(2));
        if (linked) {
            const int lk = SaveEditor::GetIntAt(gMapSav, hL, gMapSelSlot, -1);
            W::RowStepper(Ui::HashStr("map.link"), rowAt(cr, ri++), { Tx("map.link"), "" }, std::to_string(lk), 0.5f, 0.f, nudge(3));
        }
        W::RowLink(Ui::HashStr("map.del"), rowAt(cr, ri++), { Tx("map.delete"), "" }, std::string(),
                   []() { sMapDelAsk = true; Motion::Forget(Ui::HashStr("map.delask")); }, UiIcons::Id::Trash, true);
        for (int i = 1; i < rows; i++) W::Hairline(cr.x + 24, cr.y + 6 + i * (W::kRowHSmall + 1) - 1, cr.w - 48);
        cy = cr.y + cr.h + 12;
    }
    (void)cy;
}

static void PageMap() {
    if (!gMapSav.loaded) {
        static double sTry = -10;
        if (Motion::Now() - sTry > 2.0) { sTry = Motion::Now(); std::string err; SaveEditor::Load(SAVE_MAP_SAV, gMapSav, err); }
    }
    MapBuildPaintTiles();
    ToastFromMsg(gMapMsg, gMapMsgCol);
    // The whole tab is the map; the tools float on it.
    const SDL_Rect area{ sEdSideW, 0, SCREEN_W - sEdSideW, SCREEN_H };
    if (!gMapSav.loaded) {
        DrawTextC(Tx("map.loading"), area.x + area.w / 2, area.h / 2, gTheme.dim, Font::Row);
        return;
    }
    sMapCW = area.w;
    sMapCH = area.h;
    constexpr int kTop = 16 + 56 + 22;                   // the toolbar's band
    constexpr int kInsW = 340;
    const bool inspector = MapHasInspector();
    sMapSafeT = kTop;
    sMapSafeR = inspector ? kInsW + 44 : 0;
    sMapSafeB = 72;
    // The inspector coming in (an object picked up, a tool chosen) may cover
    // the hand: the view makes room for it.
    {
        static int sSafeRWas = -1;
        if (sMapSafeR != sSafeRWas) { if (sSafeRWas >= 0) MapFollow(); sSafeRWas = sMapSafeR; }
    }
    const SDL_Rect hit{ area.x, area.y + kTop, area.w - (inspector ? kInsW + 44 : 0), area.h - kTop };
    MapCanvas(area, hit);
    W::BeginBlock({ area.x + 16, area.y + 16, area.w - 32, 56 });
    MapToolbar(area.x + 24, area.y + 20, area.w - 48);
    W::EndBlock();
    if (inspector) {
        W::BeginBlock({ area.x + area.w - 24 - kInsW, area.y + kTop + 10, kInsW, MapInspectorH() });
        MapInspector(area.x + area.w - 24 - kInsW, area.y + kTop + 10, kInsW);
        W::EndBlock();
    }
    // Exploring, B puts back what the hand holds first, then stops placing,
    // then stops exploring (the map stays highlighted, under its veil).
    const bool exploring = Focus::Is(kMapCanvasId) && sMapExplore;
    if (exploring) {
        Focus::SetBaseBack([]() {
            Sound::Play(Sound::Voice::Release);
            if (sMapCarry) { sMapCarry = false; return; }
            if (gMapMode == MapMode::Place) { gMapMode = MapMode::Idle; return; }
            if (gMapStroking) MapStrokeEnd();
            sMapExplore = false;
        }, Tx(sMapCarry ? "map.hint.putback" : gMapMode == MapMode::Place ? "hint.cancel" : "map.explore.stop"));
    }
    if (exploring && sMapCarry) Focus::Command(Input::X, Tx("map.hint.turn"), []() { MapCarryTurn(); });
    else if (!gMapUndo.empty()) Focus::Command(Input::X, Tx("hint.undo"), []() { MapUndo(); Sound::Play(Sound::Voice::Undo); });
    if (exploring && !sMapCarry && gMapMode != MapMode::Place && gMapTool != MapTool::Paint && !sMapObjs.empty())
        Focus::Command(Input::Y, Tx("map.hint.next"), []() { MapNextObject(); });
    Focus::Command(Input::ZL, Tx("map.zoom"), []() { MapZoomTo(gMapZoom - 1); });
    Focus::Command(Input::ZR, Tx("map.zoom"), []() { MapZoomTo(gMapZoom + 1); });

    if (sMapPicker) {
        std::vector<W::MenuItem> items;
        items.push_back({ gMapPickerFilter.empty() ? Tx("map.search") : "\xE2\x80\x9C" + gMapPickerFilter + "\xE2\x80\x9D", "", UiIcons::Id::Magnifier, false, []() {
            gMapPickerFilter = ShowKeyboard(Tx("map.search"), gMapPickerFilter, 30);
            MapRebuildPicker();
            Focus::Set(Ui::IdMix(Ui::HashStr("map.picker"), 1u));
        }, false });
        static const char* const kG[] = { "map.group.house", "map.group.facility", "map.group.deco", "map.group.room", "map.group.step", "map.group.ugc", "map.group.unknown" };
        for (uint32_t h : gMapPickerActors) {
            const MapData::ActorInfo* info = MapData::ActorLookup(h);
            items.push_back({ MapActorDisplayLabel(info), info ? Tx(kG[std::max(0, std::min(6, (int)info->group))]) : std::string(), UiIcons::Id::Count, false, [h]() {
                gMapPlaceActor = h;
                gMapMode = MapMode::Place;
                gMapTool = MapTool::Select;
                sMapPicker = false;
                Focus::Set(kMapCanvasId);
                sMapExplore = true;                       // straight onto the map to put it down
            }, false, {}, Art::Id::Count, MapActorArt(h) });
        }
        W::Menu(Ui::HashStr("map.picker"), Tx("map.add"), items, []() { sMapPicker = false; }, Tx("hint.close"), 0, Focus::RectOf(Ui::HashStr("map.add")));
    }
    if (sMapTilePicker) {
        std::vector<W::MenuItem> items;
        int cur = 0;
        for (size_t i = 0; i < gMapPaintTiles.size(); i++) {
            const uint32_t h = gMapPaintTiles[i];
            if (h == gMapPaintTile) cur = (int)i;
            const MapData::TileInfo* ti = MapData::TileLookup(h);
            (void)ti;
            items.push_back({ MapTileName(h), "", UiIcons::Id::Count, false, [h]() { gMapPaintTile = h; sMapTilePicker = false; }, h == gMapPaintTile,
                              {}, Art::Id::Count, MapTileArt(h) });
        }
        W::Menu(Ui::HashStr("map.tilemenu"), Tx("map.tile"), items, []() { sMapTilePicker = false; }, Tx("hint.close"), cur, Focus::RectOf(Ui::HashStr("map.tile")));
    }
    if (sMapDelAsk) {
        const MapData::ActorInfo* info = gMapSelSlot >= 0 ? MapData::ActorLookup(MapActorAt(gMapSelSlot)) : nullptr;
        W::Dialog(Ui::HashStr("map.delask"), UiIcons::Id::Trash, Ui::Tone::Bad, TokenFill(Tx("map.delete.ask"), "{name}", MapActorDisplayLabel(info)),
                  Tx("map.delete.body"),
                  { { Tx("picker.cancel"), W::Kind::Neutral, []() { sMapDelAsk = false; } },
                    { Tx("map.delete"), W::Kind::Danger, []() { sMapDelAsk = false; MapDeleteSelected(); } } },
                  0, []() { sMapDelAsk = false; }, Tx("hint.cancel"));
    }
}
