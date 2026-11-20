// page_miis2.inc — TomoToolNX 2.0: the Mii sub-pages after Profile and Words:
// Relations, Items, Habits, House, Social, and finding Miis on TomodachiShare.
// The writes are 1.x's own (relation pairs, belongings arrays, habit flags,
// HousingDo*, ShareDoImport). Included by shell.inc inside namespace S2.

static Focus::Scroll sMii2Scroll;

// ── Relations ────────────────────────────────────────────────────────────────
struct MiiRel { int pair; bool selfA; int other; uint32_t type; int meter; };
static std::vector<MiiRel> MiiRelations(int miiIdx) {
    static const uint32_t H_IDA = 0xf7420afbu, H_IDB = 0x4071f71cu, H_BASE = 0x8b41897eu, H_METER = 0x42c2fc2fu;
    std::vector<MiiRel> out;
    const int n = SaveEditor::ArraySize(gMiiSav, H_IDA);
    for (int pi = 0; pi < n; pi++) {
        const int a = SaveEditor::GetIntAt(gMiiSav, H_IDA, pi), b = SaveEditor::GetIntAt(gMiiSav, H_IDB, pi);
        if (a < 0 || b < 0 || (a != miiIdx && b != miiIdx)) continue;
        const bool selfA = a == miiIdx;
        const int myDir = selfA ? pi * 2 : pi * 2 + 1;
        out.push_back({ pi, selfA, selfA ? b : a, SaveEditor::GetEnumAt(gMiiSav, H_BASE, myDir, 0x0784a8dcu),
                        SaveEditor::GetIntAt(gMiiSav, H_METER, myDir) });
    }
    return out;
}
static std::string RelLabel(uint32_t type) {
    const std::string name = RelTypeName(type);
    std::string key = "mii.rel.";
    for (char c : name) if (c != '-') key += c;
    return Tx(key.c_str());
}
static std::string MiiNameAt(int idx) {
    static const uint32_t H_NAME = 0x2499bfdau;
    const std::string n = SaveEditor::GetWStr32At(gMiiSav, H_NAME, idx);
    return n.empty() ? std::string("?") : n;
}

static int sRelOpen = -1;      // the pair whose sheet is open
static void RelationSheet(int miiIdx) {
    const auto rels = MiiRelations(miiIdx);
    const MiiRel* r = nullptr;
    for (const auto& x : rels) if (x.pair == sRelOpen) r = &x;
    if (!r) { sRelOpen = -1; return; }
    const Ui::Id id = Ui::HashStr("rel.sheet");
    const double since = Motion::FirstSeen(id), now = Motion::Now();
    if (now - since < 0.0001) { Sound::Play(Sound::Voice::Open); Focus::Set(Ui::HashStr("rel.type")); }
    gMiiRelPairIdx = r->pair; gMiiRelSelfA = r->selfA;
    const int w = 680, h = 120 + 3 * W::kRowH + 2 + 12 + 24, x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
    Focus::PushLayer(Focus::Layer::Sheet, []() { sRelOpen = -1; Motion::Forget(Ui::HashStr("rel.sheet")); }, Tx("hint.close"), SDL_Rect{ x, y, w, h });
    DimScreen((int)(130 * std::min(1.f, (float)((now - since) / 0.16))));
    const float e = Motion::Reduced() ? 1.f : Motion::Ease(Motion::Curve::Spring, std::min(1.f, (float)((now - since) / 0.42)));
    PushXform(0, 14.f * (1 - e), 0.98f + 0.02f * e, SCREEN_W / 2.f, SCREEN_H / 2.f, std::min(1.f, e * 1.4f));
    FillRectRounded(x, y + 10, w, h, 26, SDL_Color{ 0, 0, 0, 60 });
    FillRectRounded(x, y, w, h, 26, Mix(gTheme.panel, gTheme.text, 0.03f));
    DrawRectRounded(x, y, w, h, 26, Alpha(gTheme.border, 230));
    const std::string me = MiiNameAt(miiIdx), other = MiiNameAt(r->other);
    W::Avatar(x + 50, y + 56, 24, me, MiiAvatarSeed(miiIdx + 1, me), MiiFace(miiIdx + 1), MiiHairColor(miiIdx + 1), MiiPicture(miiIdx + 1));
    W::Avatar(x + 92, y + 56, 24, other, MiiAvatarSeed(r->other + 1, other), MiiFace(r->other + 1), MiiHairColor(r->other + 1), MiiPicture(r->other + 1));
    W::Marquee(Ui::HashStr("rel.sheet.title"), me + " & " + other, x + 130, y + 30, w - 160, gTheme.text, Font::Title, true);
    DrawText(RelLabel(r->type), x + 130, y + 30 + TextH(Font::Title), RelTypeColor(r->type), Font::Body);
    int ti = 0;
    for (int k = 0; k < RELATION_TYPE_COUNT; k++) if (RELATION_TYPE_NAMES[k].hash == r->type) ti = k;
    const int ry0 = y + 104;
    const SDL_Rect rowT{ x + 10, ry0, w - 20, W::kRowH };
    const int pair = r->pair; const bool selfA = r->selfA;
    W::RowStepper(Ui::HashStr("rel.type"), rowT, { Tx("mii.rel.type"), Tx("mii.rel.type.desc") }, RelLabel(r->type),
                  (float)ti / (RELATION_TYPE_COUNT - 1), 1.f / (RELATION_TYPE_COUNT - 1), [ti, pair, selfA](int d) {
                      static const uint32_t H_BASE = 0x8b41897eu, H_METER = 0x42c2fc2fu;
                      const int myDir = selfA ? pair * 2 : pair * 2 + 1, otDir = selfA ? pair * 2 + 1 : pair * 2;
                      const int nk = std::max(0, std::min(RELATION_TYPE_COUNT - 1, ti + d));
                      const uint32_t nh = RELATION_TYPE_NAMES[nk].hash;
                      SaveEditor::SetEnumAt(gMiiSav, H_BASE, myDir, nh);
                      SaveEditor::SetEnumAt(gMiiSav, H_BASE, otDir, RelTypeCounterpart(nh));
                      if (RelMeterFixed(nh)) { SaveEditor::SetIntAt(gMiiSav, H_METER, myDir, 100); SaveEditor::SetIntAt(gMiiSav, H_METER, otDir, 100); }
                      gMiiSavDirty = true;
                  });
    W::Hairline(x + 28, ry0 + W::kRowH, w - 56);
    const bool fixed = RelMeterFixed(r->type);
    if (fixed) {
        W::RowLink(Ui::HashStr("rel.meter"), { x + 10, ry0 + W::kRowH + 1, w - 20, W::kRowH }, { Tx("mii.rel.meter"), Tx("mii.rel.meter.fixed") },
                   "100", nullptr, UiIcons::Id::Count);
    } else {
        W::RowSlider(Ui::HashStr("rel.meter"), { x + 10, ry0 + W::kRowH + 1, w - 20, W::kRowH }, { Tx("mii.rel.meter"), Tx("mii.rel.meter.desc") },
                     std::max(0.f, std::min(1.f, r->meter / 100.f)), 0.1f, std::to_string(r->meter), [](int d) { TRelMeterAdj(d * 10); });
    }
    W::Hairline(x + 28, ry0 + 2 * W::kRowH + 1, w - 56);
    {
        static const uint32_t H_TST = 0x1a892e50u;
        const uint64_t st = SaveEditor::GetUInt64At(gMiiSav, H_TST, pair);
        std::string shown = Tx("player.donation.never");
        if (st) { time_t t = (time_t)st; struct tm tmv; if (localtime_r(&t, &tmv)) { char b[24]; strftime(b, sizeof b, "%Y-%m-%d", &tmv); shown = b; } }
        W::RowLink(Ui::HashStr("rel.since"), { x + 10, ry0 + 2 * W::kRowH + 2, w - 20, W::kRowH }, { Tx("mii.rel.since"), Tx("mii.rel.since.desc") },
                   shown, [pair]() {
                       static const uint32_t H_TST = 0x1a892e50u;
                       const uint64_t cur = SaveEditor::GetUInt64At(gMiiSav, H_TST, pair);
                       char init[36] = "";
                       if (cur > 0) { time_t t = (time_t)cur; struct tm* ti = localtime(&t); snprintf(init, sizeof init, "%04d-%02d-%02d", ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday); }
                       const std::string raw = ShowKeyboard(Tx("mii.rel.since.guide"), init, 10);
                       int yy = 0, mo = 0, dd = 0;
                       if (sscanf(raw.c_str(), "%d-%d-%d", &yy, &mo, &dd) == 3 && yy > 1900 && mo >= 1 && mo <= 12 && dd >= 1 && dd <= 31) {
                           struct tm t = {}; t.tm_year = yy - 1900; t.tm_mon = mo - 1; t.tm_mday = dd; t.tm_hour = 12;
                           const time_t secs = mktime(&t);
                           if (secs >= 0) { SaveEditor::SetUInt64At(gMiiSav, H_TST, pair, (uint64_t)secs); gMiiSavDirty = true; }
                       } else if (!raw.empty()) {
                           Sound::Play(Sound::Voice::Err);
                           Ui::ToastShow(Tx("mii.rel.since.bad"), Ui::Tone::Bad);
                       }
                   }, UiIcons::Id::Pencil);
    }
    PopXform();
    Focus::PopLayer();
}

static void MiiRelationsPage(int miiIdx, int x, int y, int w, const SDL_Rect& view) {
    const auto rels = MiiRelations(miiIdx);
    Focus::BeginScroll(Ui::HashStr("mii.rels"), view, sMii2Scroll);
    int yy = y - (int)sMii2Scroll.offset;
    W::Eyebrow(x + 6, yy, TokenFill(Tx("mii.rel.count"), "{n}", std::to_string(rels.size())), gTheme.faint);
    yy += 22;
    if (rels.empty()) {
        W::Card({ x, yy, w, 120 });
        DrawTextC(Tx("mii.no.relations"), x + w / 2, yy + 60, gTheme.dim, Font::Row);
        yy += 120;
    } else {
        const int rowH = 64;
        W::Card({ x, yy, w, (int)rels.size() * rowH + 12 });
        int ry = yy + 6;
        for (size_t i = 0; i < rels.size(); i++) {
            const MiiRel& r = rels[i];
            const SDL_Rect rr{ x + 6, ry, w - 12, rowH };
            const Ui::Id rid = Ui::IdMix(Ui::HashStr("mii.rel"), (uint32_t)r.pair);
            Focus::Node n;
            n.id = rid; n.r = rr; n.radius = 14; n.hint = Tx("hint.open");
            const int pair = r.pair;
            n.press = [pair]() { sRelOpen = pair; Motion::Forget(Ui::HashStr("rel.sheet")); };
            Focus::Add(std::move(n));
            if (rr.y + rr.h >= view.y && rr.y <= view.y + view.h) {
                W::BeginPress(rid, rr);
                Focus::DrawPlate(rid);
                const std::string other = MiiNameAt(r.other);
                W::Avatar(rr.x + 32, rr.y + rowH / 2, 20, other, MiiAvatarSeed(r.other + 1, other), MiiFace(r.other + 1), MiiHairColor(r.other + 1), MiiPicture(r.other + 1));
                W::Marquee(rid, other, rr.x + 64, rr.y + 8, w - 300, gTheme.text, Font::Row, Focus::Is(rid));
                const SDL_Color tc = RelTypeColor(r.type);
                FillDisc((float)(rr.x + 70), (float)(rr.y + 14 + TextH(Font::Row)), 4.f, tc);
                DrawText(RelLabel(r.type), rr.x + 80, rr.y + 4 + TextH(Font::Row), gTheme.dim, Font::Body);
                // The meter as a short bar.
                const int bw = 120, bx = rr.x + rr.w - 50 - bw;
                FillRectRounded(bx, rr.y + rowH / 2 - 4, bw, 8, 4, Alpha(gTheme.text, 20));
                FillRectRounded(bx, rr.y + rowH / 2 - 4, std::max(8, bw * std::max(0, std::min(100, r.meter)) / 100), 8, 4, tc);
                UiIcons::Draw(UiIcons::Id::ChevronRight, rr.x + rr.w - 24, rr.y + rowH / 2, 20, gTheme.faint);
                W::EndPress();
            }
            ry += rowH;
        }
        yy = ry + 6;
    }
    Focus::EndScroll((float)(yy + (int)sMii2Scroll.offset - y + 20));
    if (sRelOpen >= 0) RelationSheet(miiIdx);
}

// ── Items ────────────────────────────────────────────────────────────────────
// The picture and the real colours of an item (tools/assets/build_items.py).
// kind: 0 a piece of clothing, 1 an outfit set, 2 a pocket good.
static const ItemArtInfo* ItemArtOf(int kind, uint32_t hash) {
    static std::unordered_map<uint64_t, int> idx;
    if (idx.empty())
        for (int i = 0; i < ITEM_ART_COUNT; i++) idx[((uint64_t)ITEM_ART[i].kind << 32) | ITEM_ART[i].hash] = i;
    const auto it = idx.find(((uint64_t)kind << 32) | hash);
    return it == idx.end() ? nullptr : &ITEM_ART[it->second];
}
static SDL_Texture* ItemPic(int kind, uint32_t hash) {
    const ItemArtInfo* a = ItemArtOf(kind, hash);
    return a ? Art::ItemPicture(a->asset) : nullptr;
}
static std::vector<SDL_Color> ItemColours(int kind, uint32_t hash, int count) {
    std::vector<SDL_Color> v;
    const ItemArtInfo* a = ItemArtOf(kind, hash);
    for (int i = 0; i < count && i < 8; i++) {
        const uint32_t c = a && i < a->colours ? a->rgb[i] : 0x8a8a90u;    // not measured: grey
        v.push_back({ (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c, 255 });
    }
    return v;
}

static int sBlAsk = -1;     // which wardrobe action is waiting for a yes

static void BlApply(int blSel, int miiIdx, uint32_t newHash) {
    if (blSel < 8) {
        const auto& ws = BL_WORN_DEFS[blSel];
        const uint32_t old = SaveEditor::GetAnyEnumAt(gMiiSav, ws.kh, miiIdx, BL_H_INVALID);
        SaveEditor::SetAnyEnumAt(gMiiSav, ws.kh, miiIdx, newHash);
        if (newHash != old) SaveEditor::SetIntAt(gMiiSav, ws.ch, miiIdx, 0);
    } else if (blSel == 8) {
        const uint32_t old = SaveEditor::GetAnyEnumAt(gMiiSav, BL_COORD_KH, miiIdx, BL_H_INVALID);
        SaveEditor::SetAnyEnumAt(gMiiSav, BL_COORD_KH, miiIdx, newHash);
        if (newHash != old) SaveEditor::SetIntAt(gMiiSav, BL_COORD_CH, miiIdx, 0);
    } else if (blSel >= 9 && blSel < 21) {
        const int ai = miiIdx * BL_GOODS_SLOTS_MII + (blSel - 9);
        if (newHash == 0u) {
            SaveEditor::SetUIntAt(gMiiSav, BL_H_GOODS_ID, ai, 0u);
            SaveEditor::SetUInt64At(gMiiSav, BL_H_GOODS_TIME, ai, 0ull);
            SaveEditor::SetIntAt(gMiiSav, BL_H_GOODS_UGC, ai, -1);
        } else {
            SaveEditor::SetUIntAt(gMiiSav, BL_H_GOODS_ID, ai, newHash);
            if (!SaveEditor::GetUInt64At(gMiiSav, BL_H_GOODS_TIME, ai)) SaveEditor::SetUInt64At(gMiiSav, BL_H_GOODS_TIME, ai, 1735689600ull);
            SaveEditor::SetIntAt(gMiiSav, BL_H_GOODS_UGC, ai, -1);
        }
    }
    gMiiSavDirty = true;
}

static void BlPicker(int miiIdx) {
    std::vector<W::MenuItem> items;
    items.push_back({ gBlFilter.empty() ? Tx("mii.items.search") : "\xE2\x80\x9C" + gBlFilter + "\xE2\x80\x9D", "", UiIcons::Id::Magnifier, false, []() {
        std::string q = ShowKeyboard(Tx("mii.items.search"), gBlFilter, 30);
        for (auto& c : q) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        gBlFilter = q;
        BlRebuildFilter();
        Focus::Set(Ui::IdMix(Ui::HashStr("bl.picker"), 1u));
    }, false });
    const int blSel = gBlSel;
    const int kind = blSel < 8 ? 0 : blSel == 8 ? 1 : 2;
    for (int i = 0; i < (int)gBlFiltered.size(); i++) {
        const int real = gBlFiltered[i];
        if (real < 0 || real >= (int)gBlPickerHashes.size()) continue;
        const uint32_t h = gBlPickerHashes[real];
        const ItemArtInfo* art = h && h != BL_H_INVALID ? ItemArtOf(kind, h) : nullptr;
        items.push_back({ gBlPickerLabels[real], "", UiIcons::Id::Count, false, [blSel, miiIdx, h]() {
            BlApply(blSel, miiIdx, h);
            gBlPickerOpen = false;
        }, false, {}, Art::Id::Count, art ? art->asset : nullptr });
    }
    W::Menu(Ui::HashStr("bl.picker"), Tx("mii.items.pick"), items, []() { gBlPickerOpen = false; }, Tx("hint.close"),
            std::min((int)items.size() - 1, gBlPickerSel + 1), Focus::RectOf(Ui::IdMix(Ui::HashStr("bl.row"), (uint32_t)blSel)));
}

// The pocket: twelve things carried, as a grid of pictures.
static int MiiPocketGrid(int miiIdx, int x, int y, int w, const std::function<std::function<void()>(int)>& pick) {
    constexpr int kCols = 6, kRows = 2, kGap = 10, kTileH = 124;
    W::Eyebrow(x + 6, y, Tx("mii.items.pocket"), gTheme.faint);
    const int top = y + 22;
    const int h = kRows * kTileH + (kRows - 1) * kGap + 24;
    W::Card({ x, top, w, h });
    const int tw = (w - 24 - (kCols - 1) * kGap) / kCols;
    for (int p = 0; p < 12; p++) {
        const SDL_Rect tr{ x + 12 + (p % kCols) * (tw + kGap), top + 12 + (p / kCols) * (kTileH + kGap), tw, kTileH };
        const int ai = miiIdx * BL_GOODS_SLOTS_MII + p;
        const uint32_t gh = SaveEditor::GetUIntAt(gMiiSav, BL_H_GOODS_ID, ai);
        const Ui::Id rid = Ui::IdMix(Ui::HashStr("bl.row"), (uint32_t)(9 + p));
        Focus::Node n;
        n.id = rid; n.r = tr; n.radius = 16; n.press = pick(9 + p); n.hint = Tx("mii.items.pick");
        Focus::Add(std::move(n));
        W::BeginPress(rid, tr);
        Focus::DrawPlate(rid);
        const bool f = Focus::Is(rid);
        const int ps = std::min(tr.w - 18, 78);
        const SDL_Rect pb{ tr.x + (tr.w - ps) / 2, tr.y + 10, ps, ps };
        if (gh) {
            FillRectRounded(pb.x, pb.y, pb.w, pb.h, 14, Alpha(gTheme.text, f ? 26 : 14));
            if (SDL_Texture* pt = ItemPic(2, gh)) {
                const int g = f ? 2 : 6;
                Art::DrawFit(pt, SDL_Rect{ pb.x + g, pb.y + g, pb.w - 2 * g, pb.h - 2 * g });
            } else {
                UiIcons::Draw(UiIcons::Id::Sparkle, pb.x + pb.w / 2, pb.y + pb.h / 2, 30, Alpha(gTheme.text, 150));
            }
        } else {
            DashRectRounded((float)pb.x + 1.f, (float)pb.y + 1.f, (float)pb.w - 2.f, (float)pb.h - 2.f, 13.f, 1.5f, 6.f, 5.f,
                            Alpha(gTheme.text, f ? 110 : 60));
            UiIcons::Draw(UiIcons::Id::Plus, pb.x + pb.w / 2, pb.y + pb.h / 2, 22, Alpha(gTheme.text, f ? 170 : 100));
        }
        const std::string name = gh ? std::string(BlGoodsLabel(gh)) : Tx("mii.items.empty");
        W::Marquee(rid, name, tr.x + tr.w / 2, pb.y + ps + 16, tr.w - 10, gh ? (f ? gTheme.text : gTheme.dim) : gTheme.faint, Font::Xs, f, W::Anchor::Centre);
        W::EndPress();
        if (gh && Focus::Is(rid))
            Focus::Command(Input::X, Tx("hint.clear"), [p, miiIdx]() { BlApply(9 + p, miiIdx, 0u); Sound::Play(Sound::Voice::Delete); });
    }
    return top + h + 26;
}

static void MiiItemsPage(int miiIdx, int x, int y, int w, const SDL_Rect& view) {
    Focus::BeginScroll(Ui::HashStr("mii.items"), view, sMii2Scroll);
    MiiCard c{ x, y - (int)sMii2Scroll.offset, w, 0 };
    auto pick = [miiIdx](int sel) {
        return [sel, miiIdx]() { gBlSel = sel; BlOpenPicker(sel, miiIdx); Motion::Forget(Ui::HashStr("bl.picker")); };
    };
    auto clearHint = [&](int sel) {
        const Ui::Id rid = Ui::IdMix(Ui::HashStr("bl.row"), (uint32_t)sel);
        if (!Focus::Is(rid)) return;
        Focus::Command(Input::X, Tx("hint.clear"), [sel, miiIdx]() { BlApply(sel, miiIdx, sel < 9 ? BL_H_INVALID : 0u); Sound::Play(Sound::Voice::Delete); });
    };
    // Wearing: the eight clothing slots and the outfit set, colour on ◀ ▶.
    c.Begin(Tx("mii.items.wearing"), 9);
    for (int s = 0; s < 9; s++) {
        const uint32_t kh = s < 8 ? BL_WORN_DEFS[s].kh : BL_COORD_KH, ch = s < 8 ? BL_WORN_DEFS[s].ch : BL_COORD_CH;
        const uint32_t ih = SaveEditor::GetAnyEnumAt(gMiiSav, kh, miiIdx, BL_H_INVALID);
        const bool has = ih && ih != BL_H_INVALID;
        int cc = 1;
        if (has) {
            if (s < 8) { for (int k = 0; k < BL_CLOTH_COUNT; k++) if (BL_CLOTH_ITEMS[k].nameHash == ih) { cc = BL_CLOTH_ITEMS[k].colorCount; break; } }
            else       { for (int k = 0; k < BL_COORD_COUNT; k++) if (BL_COORD_ITEMS[k].keyHash == ih) { cc = BL_COORD_ITEMS[k].colorCount; break; } }
        }
        const int col = std::max(0, std::min(std::max(0, cc - 1), SaveEditor::GetIntAt(gMiiSav, ch, miiIdx)));
        const std::string item = has ? std::string(s < 8 ? BlClothLabel(ih) : BlCoordLabel(ih)) : Tx("mii.items.none");
        const std::string slotName = Tx((std::string("mii.worn.") + (s < 8 ? BL_WORN_DEFS[s].name : "Coord")).c_str());
        const Ui::Id rid = Ui::IdMix(Ui::HashStr("bl.row"), (uint32_t)s);
        const int kind = s < 8 ? 0 : 1;
        // The piece's picture, and its colours as they really look.
        W::RowItem(rid, c.Row(s == 8), has ? ItemPic(kind, ih) : nullptr, { slotName, item },
                   has ? ItemColours(kind, ih, cc) : std::vector<SDL_Color>(), col,
                   cc > 1 ? std::function<void(int)>([miiIdx, ch, cc, col](int d) {
                       SaveEditor::SetIntAt(gMiiSav, ch, miiIdx, std::max(0, std::min(cc - 1, col + d)));
                       gMiiSavDirty = true;
                   }) : nullptr,
                   pick(s), Tx("mii.items.pick"));
        if (has) clearHint(s);
    }
    c.y = c.End();
    // Pocket: twelve things carried, as pictures.
    c.y = MiiPocketGrid(miiIdx, x, c.y, w, pick);
    // Wardrobe: own every piece, or none.
    c.Begin(Tx("mii.items.wardrobe"), 4);
    static const char* const kAct[] = { "mii.items.clothes.all", "mii.items.clothes.none", "mii.items.sets.all", "mii.items.sets.none" };
    for (int a = 0; a < 4; a++) {
        const int aa = a;
        W::RowLink(Ui::IdMix(Ui::HashStr("bl.act"), (uint32_t)a), c.Row(a == 3), { Tx(kAct[a]), std::string() }, std::string(),
                   [aa]() { sBlAsk = aa; Motion::Forget(Ui::HashStr("bl.ask")); }, a % 2 ? UiIcons::Id::Close : UiIcons::Id::Check, a % 2 == 1);
    }
    c.y = c.End();
    Focus::EndScroll((float)(c.y + (int)sMii2Scroll.offset - y));
    if (gBlPickerOpen) BlPicker(miiIdx);
    if (sBlAsk >= 0) {
        const int act = sBlAsk;
        W::Dialog(Ui::HashStr("bl.ask"), act % 2 ? UiIcons::Id::Close : UiIcons::Id::Check, act % 2 ? Ui::Tone::Bad : Ui::Tone::Accent,
                  Tx(kAct[act]), Tx("mii.items.ask.body"),
                  { { Tx("picker.cancel"), W::Kind::Neutral, []() { sBlAsk = -1; } },
                    { Tx("hint.select"), act % 2 ? W::Kind::Danger : W::Kind::Primary, [act, miiIdx]() {
                        sBlAsk = -1;
                        // 1.x's four ownership actions, as they were.
                        if (act == 0) {
                            const int total = SaveEditor::ArraySize(gMiiSav, BL_H_CLOTH_OWN);
                            for (int i = 0; i < BL_CLOTH_COUNT; i++) {
                                const int idx = miiIdx * BL_SLOTS_PER_MII + BL_CLOTH_ITEMS[i].index;
                                if (idx < total) { const int cc = BL_CLOTH_ITEMS[i].colorCount; SaveEditor::SetUIntAt(gMiiSav, BL_H_CLOTH_OWN, idx, cc >= 32 ? 0xFFFFFFFFu : ((1u << cc) - 1u)); }
                            }
                        } else if (act == 1) {
                            const int total = SaveEditor::ArraySize(gMiiSav, BL_H_CLOTH_OWN);
                            for (int i = 0; i < BL_SLOTS_PER_MII; i++) { const int idx = miiIdx * BL_SLOTS_PER_MII + i; if (idx < total) SaveEditor::SetUIntAt(gMiiSav, BL_H_CLOTH_OWN, idx, 0u); }
                        } else if (act == 2) {
                            const int total = SaveEditor::ArraySize(gMiiSav, BL_H_COORD_OWN);
                            for (int i = 0; i < BL_COORD_COUNT; i++) {
                                const int idx = miiIdx * BL_COORD_SLOTS_MII + BL_COORD_ITEMS[i].saveIndex;
                                if (idx < total) { const int cc = BL_COORD_ITEMS[i].colorCount; SaveEditor::SetUIntAt(gMiiSav, BL_H_COORD_OWN, idx, cc >= 32 ? 0xFFFFFFFFu : ((1u << cc) - 1u)); }
                            }
                        } else {
                            const int total = SaveEditor::ArraySize(gMiiSav, BL_H_COORD_OWN);
                            for (int i = 0; i < BL_COORD_SLOTS_MII; i++) { const int idx = miiIdx * BL_COORD_SLOTS_MII + i; if (idx < total) SaveEditor::SetUIntAt(gMiiSav, BL_H_COORD_OWN, idx, 0u); }
                        }
                        gMiiSavDirty = true;
                        Sound::Play(Sound::Voice::Done);
                        Ui::ToastShow(Tx(kAct[act]), Ui::Tone::Good);
                    } } },
                  0, []() { sBlAsk = -1; }, Tx("hint.cancel"));
    }
}

// ── Habits ───────────────────────────────────────────────────────────────────
static bool sHabitClearAsk = false;
static void MiiHabitsPage(int miiIdx, int x, int y, int w, const SDL_Rect& view) {
    EnsureHabitHashes();
    std::vector<std::pair<std::string, int>> cats;
    for (int c = 0; c < HABIT_CAT_COUNT; c++) cats.push_back({ HABIT_CAT_LABEL[c], -1 });
    W::Chips(Ui::HashStr("hab.cats"), x, y, cats, gHabitCatSel, [](int i) { gHabitCatSel = i; gHabitItemSel = 0; sMii2Scroll.target = 0; }, w);
    const std::vector<int> items = HabitsInCategory(gHabitCatSel);
    const SDL_Rect v2{ view.x, y + 56 + 12, view.w, view.y + view.h - (y + 56 + 12) };
    Focus::BeginScroll(Ui::HashStr("mii.habits"), v2, sMii2Scroll);
    MiiCard c{ x, y + 56 + 16 - (int)sMii2Scroll.offset, w, 0 };
    c.Begin(Tx("mii.habits.title"), (int)items.size() + 1);
    for (size_t k = 0; k < items.size(); k++) {
        const int hi = items[k];
        const bool own = SaveEditor::GetBoolAt(gMiiSav, gHabitHashes[hi].isOwn, miiIdx, false);
        const bool active = SaveEditor::GetBoolAt(gMiiSav, gHabitHashes[hi].isChecked, miiIdx, false);
        const Ui::Id rid = Ui::IdMix(Ui::HashStr("hab.row"), (uint32_t)hi);
        const SDL_Rect r = c.Row();
        W::RowLink(rid, r, { HABITS[hi].label, Tx(active ? "mii.habit.active" : own ? "mii.habit.own" : "mii.habit.none") }, std::string(),
                   [hi, items, miiIdx]() {
                       // A: this one is the habit shown (one per category), and owned.
                       for (int idx : items) {
                           const bool on = idx == hi;
                           SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[idx].isChecked, miiIdx, on);
                           if (on) {
                               SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[idx].isOwn, miiIdx, true);
                               SaveEditor::SetEnumAt(gMiiSav, gHabitHashes[idx].state, miiIdx, gHabitStateOwnH);
                           }
                       }
                       gMiiSavDirty = true;
                   }, active ? UiIcons::Id::Check : UiIcons::Id::Count);
        if (active) W::Pill(r.x + r.w - 60 - W::PillW(Tx("mii.habit.active.short"), true), r.y + (r.h - 28) / 2, Tx("mii.habit.active.short"), Ui::Tone::Accent, true);
        else if (own) W::Pill(r.x + r.w - 26 - W::PillW(Tx("mii.habit.own.short")), r.y + (r.h - 28) / 2, Tx("mii.habit.own.short"), Ui::Tone::Neutral);
        if (Focus::Is(rid)) {
            Focus::Command(Input::X, Tx(own ? "mii.habit.drop" : "mii.habit.take"), [hi, miiIdx, own]() {
                if (own) {
                    const uint32_t st = SaveEditor::GetEnumAt(gMiiSav, gHabitHashes[hi].state, miiIdx, gHabitStateNeverOwnedH);
                    SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[hi].isOwn, miiIdx, false);
                    SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[hi].isChecked, miiIdx, false);
                    if (st == gHabitStateOwnH) SaveEditor::SetEnumAt(gMiiSav, gHabitHashes[hi].state, miiIdx, gHabitStateUnownH);
                    Sound::Play(Sound::Voice::Off);
                } else {
                    SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[hi].isOwn, miiIdx, true);
                    SaveEditor::SetEnumAt(gMiiSav, gHabitHashes[hi].state, miiIdx, gHabitStateOwnH);
                    Sound::Play(Sound::Voice::On);
                }
                gMiiSavDirty = true;
            });
        }
    }
    W::RowLink(Ui::HashStr("hab.clear"), c.Row(true), { Tx("mii.habits.clear"), HABIT_CAT_LABEL[gHabitCatSel] }, std::string(),
               []() { sHabitClearAsk = true; Motion::Forget(Ui::HashStr("hab.ask")); }, UiIcons::Id::Trash, true);
    c.y = c.End();
    Focus::EndScroll((float)(c.y + (int)sMii2Scroll.offset - v2.y));
    if (sHabitClearAsk) {
        W::Dialog(Ui::HashStr("hab.ask"), UiIcons::Id::Trash, Ui::Tone::Bad, Tx("mii.habits.clear"), HABIT_CAT_LABEL[gHabitCatSel],
                  { { Tx("picker.cancel"), W::Kind::Neutral, []() { sHabitClearAsk = false; } },
                    { Tx("mii.habits.clear.go"), W::Kind::Danger, [items, miiIdx]() {
                        sHabitClearAsk = false;
                        for (int idx : items) {
                            SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[idx].isChecked, miiIdx, false);
                            SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[idx].isOwn, miiIdx, false);
                            SaveEditor::SetEnumAt(gMiiSav, gHabitHashes[idx].state, miiIdx, gHabitStateNeverOwnedH);
                        }
                        gMiiSavDirty = true;
                        Sound::Play(Sound::Voice::Delete);
                    } } },
                  0, []() { sHabitClearAsk = false; }, Tx("hint.cancel"));
    }
}

// ── House ────────────────────────────────────────────────────────────────────
// Made for islands with many Miis: the Mii shown comes first, where it lives
// and with whom, with "Move to" (every house in a list that can be searched,
// with who lives there and the rooms left; a full house asks whom to swap
// with) and "Move out". Below, every house as a card, two columns of them:
// A picks a Mii up, A on another swaps them, "Move here" puts them in.
static int sHouseAsk = -1, sHouseAskKind = 0;   // 1 vacate, 2 delete
static int sHouseMoveFor = -1;                  // the Mii the "Move to" list is for
static int sHouseSwapIn = -1;                   // a full house: whom to swap with
static std::string sHouseFilter;

struct HouseGroup { int id; std::string label; std::vector<const HousingRow*> res; };

// A house by its own name ("House 3" when it has none). The rows' labels
// carry a resident count in English for the old screen; the cards show the
// count as a pill.
static std::string HouseLabelOf(int houseId) {
    for (const auto& r : gHousingRows)
        if (r.kind == HousingRowKind::Header && r.headerHouseId == houseId && !r.name.empty()) return r.name;
    return TokenFill(Tx("mii.house.num"), "{n}", std::to_string(houseId));
}

static std::string LowerAscii(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return s;
}

// Move `mii` (a slot index) into a house: the lowest free room, or into
// `room` (whoever lives there goes where `mii` lived).
static void HouseMove(int mii, int houseId, int room) {
    HousingDoPick(mii);
    if (gHousingPicked != mii) HousingDoPick(mii);      // it toggled off: pick again
    const int r = room >= 0 ? room : HousingLowestFreeRoom(houseId, mii, -1);
    HousingDoDrop(houseId, r < 0 ? 0 : r);
    HousingRebuild();
    Sound::Play(Sound::Voice::Land);
}

static void MiiHousePage(int miiIdx, int x, int y, int w, const SDL_Rect& view) {
    static uint32_t sRebuiltFor = 0;
    if (sRebuiltFor != (uint32_t)gMiis.size() * 7919u + (gMiiSavDirty ? 1 : 0)) { HousingRebuild(); sRebuiltFor = (uint32_t)gMiis.size() * 7919u + (gMiiSavDirty ? 1 : 0); }
    if (!HousingAvailable()) {
        W::Card({ x, y, w, 140 });
        DrawTextC(Tx("mii.house.none"), x + w / 2, y + 70, gTheme.dim, Font::Row);
        return;
    }
    // The houses, from the rows HousingRebuild made.
    std::vector<HouseGroup> houses;
    HouseGroup unhoused{ -1, std::string(), {} };
    for (size_t i = 0; i < gHousingRows.size(); i++) {
        const HousingRow& r = gHousingRows[i];
        if (r.kind == HousingRowKind::Header) {
            // Its own name (the count is the card's pill); "No house" for the rest.
            if (r.headerHouseId >= 0) houses.push_back({ r.headerHouseId, HouseLabelOf(r.headerHouseId), {} });
        } else if (r.kind == HousingRowKind::Resident && !houses.empty()) {
            houses.back().res.push_back(&r);
        } else if (r.kind == HousingRowKind::Unhoused) {
            unhoused.res.push_back(&r);
        }
    }
    const bool picking = gHousingPicked >= 0;
    const int myHouse = SaveEditor::GetIntAt(gMiiSav, HASH_HOUSE_MAPID, miiIdx);
    const int myRoom = SaveEditor::GetIntAt(gMiiSav, HASH_ROOM_INDEX, miiIdx);
    Focus::BeginScroll(Ui::HashStr("mii.house"), view, sMii2Scroll);
    int yy = y - (int)sMii2Scroll.offset;

    // ── This Mii ──
    {
        const SDL_Rect card{ x, yy, w, 118 };
        W::Card(card);
        const std::string me = MiiNameAt(miiIdx);
        W::Avatar(x + 50, yy + 59, 32, me, MiiAvatarSeed(miiIdx + 1, me), MiiFace(miiIdx + 1), MiiHairColor(miiIdx + 1), MiiPicture(miiIdx + 1));
        const int tx = x + 96, bw = 176;
        const int textW = w - (tx - x) - 2 * bw - 40;
        W::Marquee(Ui::HashStr("house.me.name"), me, tx, yy + 18, textW, gTheme.text, Font::Row, Focus::CurrentIn(card));
        const std::string where = myHouse >= 0
            ? TokenFill(TokenFill(Tx("mii.house.lives"), "{house}", HouseLabelOf(myHouse)), "{n}", std::to_string(myRoom + 1))
            : Tx("mii.house.homeless");
        W::Marquee(Ui::HashStr("house.me.where"), where, tx, yy + 20 + TextH(Font::Row), textW, myHouse >= 0 ? gTheme.dim : gTheme.warn, Font::Body, Focus::CurrentIn(card));
        // Who else lives there, as little faces.
        int hx = tx;
        const int hy = yy + 92;
        for (const auto& h : houses) {
            if (h.id != myHouse) continue;
            int shown = 0;
            for (const HousingRow* r : h.res) {
                if (r->miiSlotIdx == miiIdx) continue;
                if (shown == 7) { DrawTextVC("+" + std::to_string((int)h.res.size() - 1 - shown), hx + 4, hy, gTheme.faint, Font::Xs); break; }
                const std::string nm = MiiNameAt(r->miiSlotIdx);
                W::Avatar(hx + 12, hy, 12, nm, MiiAvatarSeed(r->miiSlotIdx + 1, nm), MiiFace(r->miiSlotIdx + 1), MiiHairColor(r->miiSlotIdx + 1), MiiPicture(r->miiSlotIdx + 1));
                hx += 28; shown++;
            }
        }
        const int by = yy + (118 - 50) / 2;
        W::Button(Ui::HashStr("house.moveto"), { x + w - 18 - bw, by, bw, 50 }, Tx("mii.house.moveto"), W::Kind::Primary,
                  [miiIdx]() { sHouseMoveFor = miiIdx; sHouseFilter.clear(); Motion::Forget(Ui::HashStr("house.movemenu")); }, UiIcons::Id::Home);
        W::Button(Ui::HashStr("house.moveout"), { x + w - 18 - 2 * bw - 10, by, bw, 50 }, Tx("mii.house.moveout"), W::Kind::Neutral,
                  [miiIdx]() { HousingWriteLoc(miiIdx, -1, -1); HousingRebuild(); gMiiSavDirty = true; Sound::Play(Sound::Voice::Release); },
                  UiIcons::Id::Exit, myHouse >= 0);
        yy += 118 + 22;
    }

    // ── Every house ──
    {
        W::Eyebrow(x + 6, yy, TokenFill(Tx("mii.house.all"), "{n}", std::to_string(houses.size())), gTheme.faint);
        if (picking) {
            const std::string who = MiiNameAt(gHousingPicked);
            W::MarqueeR(Ui::HashStr("house.moving"), TokenFill(Tx("mii.house.moving"), "{name}", who), x + w - 196, yy + 8, w - 420, gTheme.gold,
                        Font::Body, true);
        }
        W::Button(Ui::HashStr("house.new"), { x + w - 180, yy - 10, 180, 40 }, Tx("mii.house.new"), W::Kind::Soft,
                  []() { HousingDoNewHouse(); HousingRebuild(); }, UiIcons::Id::Plus);
        yy += 40;
    }
    constexpr int kGap = 14, kHead = 52, kRowH = 48;
    const int colW = (w - kGap) / 2;
    int colY[2] = { yy, yy };
    auto drawGroup = [&](const HouseGroup& g, bool isHouse) {
        const int col = colY[0] <= colY[1] ? 0 : 1;
        const int cx = x + col * (colW + kGap), cy = colY[col];
        const int cap = isHouse ? std::max(1, HousingRoomCapHeuristic(g.id)) : 0;
        const int freeRoom = isHouse ? HousingLowestFreeRoom(g.id, gHousingPicked, -1) : -1;
        const bool showHere = isHouse && picking;
        const int rows = (int)g.res.size() + (showHere || g.res.empty() ? 1 : 0);
        const int ch = kHead + rows * kRowH + 10;
        colY[col] += ch + kGap;
        const SDL_Rect card{ cx, cy, colW, ch };
        if (cy + ch < view.y - 40 || cy > view.y + view.h + 40) {
            // Out of sight: only the highlight's places, nothing drawn.
            for (size_t k = 0; k < g.res.size(); k++) {
                Focus::Node n;
                n.id = Ui::IdMix(Ui::HashStr("house.res"), (uint32_t)g.res[k]->miiSlotIdx);
                n.r = SDL_Rect{ cx + 8, cy + kHead + (int)k * kRowH, colW - 16, kRowH - 4 };
                n.press = [s = g.res[k]->miiSlotIdx]() { HousingDoPick(s); };
                Focus::Add(std::move(n));
            }
            return;
        }
        W::Card(card);
        const std::string title = isHouse ? g.label : Tx("mii.house.nohouse");
        // How full it is (a pill), then its two actions; the name in the room
        // left of them, running by while something in the card is highlighted.
        const std::string occ = isHouse ? std::to_string(g.res.size()) + "/" + std::to_string(cap) : std::string();
        const int pillX = isHouse ? cx + colW - 100 - W::PillW(occ, true) : cx + colW - 18;
        W::Marquee(Ui::IdMix(Ui::HashStr("house.title"), (uint32_t)(isHouse ? g.id : 0xFFFF)), title, cx + 18, cy + 15,
                   pillX - 12 - (cx + 18), gTheme.text, Font::Row, Focus::CurrentIn(card));
        if (isHouse) {
            const bool full = (int)g.res.size() >= cap;
            W::Pill(pillX, cy + 14, occ, full ? Ui::Tone::Warn : Ui::Tone::Neutral, true);
            const int sb = 36;
            W::Button(Ui::IdMix(Ui::HashStr("house.del"), (uint32_t)g.id), { cx + colW - 10 - sb, cy + 9, sb, sb }, std::string(), W::Kind::Neutral,
                      [id = g.id]() { sHouseAsk = id; sHouseAskKind = 2; Motion::Forget(Ui::HashStr("house.ask")); }, UiIcons::Id::Trash);
            W::Button(Ui::IdMix(Ui::HashStr("house.vac"), (uint32_t)g.id), { cx + colW - 10 - 2 * sb - 6, cy + 9, sb, sb }, std::string(), W::Kind::Neutral,
                      [id = g.id]() { sHouseAsk = id; sHouseAskKind = 1; Motion::Forget(Ui::HashStr("house.ask")); }, UiIcons::Id::Exit,
                      !g.res.empty());
        }
        int ry = cy + kHead;
        for (const HousingRow* r : g.res) {
            const SDL_Rect rr{ cx + 8, ry, colW - 16, kRowH - 4 };
            const Ui::Id rid = Ui::IdMix(Ui::HashStr("house.res"), (uint32_t)r->miiSlotIdx);
            const int slot = r->miiSlotIdx, h = r->houseId, room = r->roomIdx;
            const bool isRes = r->kind == HousingRowKind::Resident;
            Focus::Node n;
            n.id = rid; n.r = rr; n.radius = 14;
            n.hint = Tx(picking && gHousingPicked != slot ? (isRes ? "mii.house.swap" : "mii.house.pick") : "mii.house.pick");
            n.press = [slot, h, room, isRes]() {
                if (gHousingPicked >= 0 && gHousingPicked != slot && isRes) { HousingDoDrop(h, room); gMiiSavDirty = true; Sound::Play(Sound::Voice::Land); }
                else { HousingDoPick(slot); Sound::Play(Sound::Voice::Grab); }
                HousingRebuild();
            };
            n.flags = Focus::OwnSound;
            Focus::Add(std::move(n));
            W::BeginPress(rid, rr);
            const bool lifted = gHousingPicked == slot;
            if (lifted) {
                FillRectRounded(rr.x, rr.y, rr.w, rr.h, 14, Alpha(gTheme.gold, 60));
                DrawRectRounded(rr.x, rr.y, rr.w, rr.h, 14, Alpha(gTheme.gold, 170));
            }
            Focus::DrawPlate(rid);
            const std::string nm = MiiNameAt(slot);
            W::Avatar(rr.x + 24, rr.y + rr.h / 2, 16, nm, MiiAvatarSeed(slot + 1, nm), MiiFace(slot + 1), MiiHairColor(slot + 1), MiiPicture(slot + 1));
            const bool isMe = slot == miiIdx;
            W::Marquee(rid, nm, rr.x + 48, rr.y + rr.h / 2, rr.w - 110, isMe ? gTheme.gold : gTheme.text, Font::Body, Focus::Is(rid), W::Anchor::Middle);
            if (isRes)
                DrawTextR(TokenFill(Tx("mii.house.room"), "{n}", std::to_string(room + 1)), rr.x + rr.w - 12, rr.y + rr.h / 2,
                          r->conflict ? gTheme.red : gTheme.faint, Font::Xs);
            W::EndPress();
            ry += kRowH;
        }
        if (isHouse && (showHere || g.res.empty())) {
            const SDL_Rect rr{ cx + 8, ry, colW - 16, kRowH - 4 };
            const bool canHere = picking && freeRoom >= 0;
            W::Button(Ui::IdMix(Ui::HashStr("house.here"), (uint32_t)g.id), rr,
                      Tx(!picking ? "mii.house.empty" : canHere ? "mii.house.here" : "mii.house.full"),
                      canHere ? W::Kind::Soft : W::Kind::Neutral, [id = g.id]() {
                          if (gHousingPicked < 0) { Sound::Play(Sound::Voice::Info); Ui::ToastShow(Tx("mii.house.pickfirst")); return; }
                          const int free = HousingLowestFreeRoom(id, gHousingPicked, -1);
                          if (free < 0) { Sound::Play(Sound::Voice::Err); Ui::ToastShow(Tx("mii.house.fullswap")); return; }
                          HousingDoDrop(id, free);
                          gMiiSavDirty = true;
                          HousingRebuild();
                          Sound::Play(Sound::Voice::Land);
                      }, UiIcons::Id::Home, canHere);
        }
    };
    for (const auto& h : houses) drawGroup(h, true);
    if (!unhoused.res.empty()) drawGroup(unhoused, false);
    const int endY = std::max(colY[0], colY[1]);
    Focus::EndScroll((float)(endY + (int)sMii2Scroll.offset - y + 10));

    if (picking) Focus::Command(Input::Minus, Tx("mii.house.evict"), []() { HousingDoEvictPicked(); gMiiSavDirty = true; HousingRebuild(); });
    if (picking && !IsSidebarId(Focus::Current()))
        Focus::SetBaseBack([]() { Sound::Play(Sound::Voice::Release); HousingDoCancel(); }, Tx("hint.cancel"));

    // "Move to": every house, who lives there and the rooms left.
    if (sHouseMoveFor >= 0) {
        const int mover = sHouseMoveFor;
        std::vector<W::MenuItem> items;
        items.push_back({ sHouseFilter.empty() ? Tx("mii.house.search") : "\xE2\x80\x9C" + sHouseFilter + "\xE2\x80\x9D", "", UiIcons::Id::Magnifier, false, []() {
            sHouseFilter = LowerAscii(ShowKeyboard(Tx("mii.house.search"), sHouseFilter, 30));
            Focus::Set(Ui::IdMix(Ui::HashStr("house.movemenu"), 1u));
        }, false });
        int cur = 0;
        for (const auto& h : houses) {
            std::string who;
            bool match = sHouseFilter.empty() || LowerAscii(h.label).find(sHouseFilter) != std::string::npos;
            for (const HousingRow* r : h.res) {
                const std::string nm = MiiNameAt(r->miiSlotIdx);
                if (!who.empty()) who += ", ";
                who += nm;
                if (!match && LowerAscii(nm).find(sHouseFilter) != std::string::npos) match = true;
            }
            if (!match) continue;
            const int cap = std::max(1, HousingRoomCapHeuristic(h.id));
            const int left = cap - (int)h.res.size();
            const std::string sub = (left > 0 ? TokenFill(Tx("mii.house.left"), "{n}", std::to_string(left)) : Tx("mii.house.isfull")) +
                                    (who.empty() ? std::string() : "  \xC2\xB7  " + who);
            if (h.id == myHouse) cur = (int)items.size();
            items.push_back({ h.label, sub, UiIcons::Id::Home, false, [mover, id = h.id]() {
                if (HousingLowestFreeRoom(id, mover, -1) >= 0) { HouseMove(mover, id, -1); gMiiSavDirty = true; sHouseMoveFor = -1; }
                else { sHouseSwapIn = id; Motion::Forget(Ui::HashStr("house.swapmenu")); }
            }, h.id == myHouse });
        }
        W::Menu(Ui::HashStr("house.movemenu"), TokenFill(Tx("mii.house.movewho"), "{name}", MiiNameAt(mover)), items,
                []() { sHouseMoveFor = -1; }, Tx("hint.close"), cur, Focus::RectOf(Ui::HashStr("house.moveto")));
    }
    // A full house: whom to swap with (they move to where this Mii lived).
    if (sHouseSwapIn >= 0 && sHouseMoveFor >= 0) {
        const int mover = sHouseMoveFor, hid = sHouseSwapIn;
        std::vector<W::MenuItem> items;
        for (const auto& h : houses) {
            if (h.id != hid) continue;
            for (const HousingRow* r : h.res) {
                const int room = r->roomIdx;
                items.push_back({ MiiNameAt(r->miiSlotIdx), TokenFill(Tx("mii.house.room"), "{n}", std::to_string(room + 1)), UiIcons::Id::Users,
                                  false, [mover, hid, room]() { HouseMove(mover, hid, room); gMiiSavDirty = true; sHouseSwapIn = -1; sHouseMoveFor = -1; },
                                  false });
            }
        }
        W::Menu(Ui::HashStr("house.swapmenu"), TokenFill(Tx("mii.house.swapwho"), "{house}", HouseLabelOf(hid)), items,
                []() { sHouseSwapIn = -1; }, Tx("hint.back"), 0);
    }
    if (sHouseAsk >= 0) {
        const bool del = sHouseAskKind == 2;
        const int hid = sHouseAsk;
        W::Dialog(Ui::HashStr("house.ask"), del ? UiIcons::Id::Trash : UiIcons::Id::Exit, Ui::Tone::Bad, Tx(del ? "mii.house.delete" : "mii.house.vacate"),
                  Tx(del ? "mii.house.delete.body" : "mii.house.vacate.body"),
                  { { Tx("picker.cancel"), W::Kind::Neutral, []() { sHouseAsk = -1; } },
                    { Tx(del ? "mii.house.delete" : "mii.house.vacate"), W::Kind::Danger, [hid, del]() {
                        sHouseAsk = -1;
                        if (del) HousingDoDeleteHouse(hid); else HousingDoVacate(hid);
                        gMiiSavDirty = true;
                        HousingRebuild();
                        Sound::Play(Sound::Voice::Delete);
                    } } },
                  0, []() { sHouseAsk = -1; }, Tx("hint.cancel"));
    }
}

// ── Social: who this Mii knows, as a web ─────────────────────────────────────
static void MiiSocialPage(int miiIdx, int x, int y, int w, const SDL_Rect& view) {
    const IslandVibeResult vibe = ComputeIslandVibe();
    {
        const std::string t = Tx("mii.vibe.label") + ": " + Tx((std::string("vibe.") + vibe.labelKey).c_str());
        const int pw = W::PillW(t, true);
        W::Pill(x + w - pw, y, t, Ui::Tone::Neutral, true);
        FillDisc((float)(x + w - pw + 15), (float)(y + 14), 4.f, vibe.color);
    }
    const auto rels = MiiRelations(miiIdx);
    const int cx = x + w / 2, cy = y + 40 + (view.y + view.h - (y + 40)) / 2;
    const int R = std::min(w / 2 - 70, (view.y + view.h - (y + 40)) / 2 - 50);
    W::Card({ x, y + 40, w, view.y + view.h - (y + 40) });
    const float t = Motion::Reduced() ? 1.f : std::min(1.f, (float)((Motion::Now() - W::ArrivedAt()) / 0.8));
    const int n = (int)rels.size();
    // Many relations: two rings, smaller faces.
    const bool two = n > 28;
    const int perRing = two ? (n + 1) / 2 : n;
    const float avR = std::max(7.f, std::min(18.f, (float)M_PI * R / std::max(1, perRing) * 0.8f));
    for (int i = 0; i < n; i++) {
        const int ring = two ? i % 2 : 0, k = two ? i / 2 : i;
        const float rr = ring ? R * 0.68f : (float)R;
        const float a = (float)(k * 2 * M_PI / std::max(1, perRing)) - (float)M_PI / 2 + (ring ? (float)M_PI / std::max(1, perRing) : 0.f);
        const float e = Motion::Ease(Motion::Curve::Spring, std::max(0.f, std::min(1.f, t * 1.4f - k * 0.01f)));
        const float px = cx + cosf(a) * rr * e, py = cy + sinf(a) * rr * e;
        const SDL_Color tc = RelTypeColor(rels[i].type);
        // The line: thicker for a stronger bond.
        const float thick = 1.5f + 3.5f * std::max(0, std::min(100, rels[i].meter)) / 100.f;
        const int steps = n > 40 ? 12 : 24;
        for (int k = 0; k < steps; k++) {
            const float u = (k + 0.5f) / steps;
            FillDisc(cx + (px - cx) * u, cy + (py - cy) * u, thick / 2 + 0.5f, Alpha(tc, 150));
        }
        const std::string nm = MiiNameAt(rels[i].other);
        if (avR >= 12) W::Avatar((int)px, (int)py, (int)avR, nm, MiiAvatarSeed(rels[i].other + 1, nm), MiiFace(rels[i].other + 1), MiiHairColor(rels[i].other + 1), MiiPicture(rels[i].other + 1));
        else FillDisc(px, py, avR, W::AvatarColor(MiiAvatarSeed(rels[i].other + 1, nm)));
        if (n <= 16) W::Marquee(Ui::IdMix(Ui::HashStr("social.name"), (uint32_t)i), nm, (int)px, (int)py + (int)avR + 12, 110, gTheme.dim, Font::Xs, false, W::Anchor::Centre);
    }
    const std::string me = MiiNameAt(miiIdx);
    FillDisc((float)cx, (float)cy, 40.f, Alpha(gTheme.gold, 50));
    W::Avatar(cx, cy, 32, me, MiiAvatarSeed(miiIdx + 1, me), MiiFace(miiIdx + 1), MiiHairColor(miiIdx + 1), MiiPicture(miiIdx + 1));
    if (n == 0) DrawTextC(Tx("mii.no.relations"), cx, cy + 60, gTheme.dim, Font::Body);
    // The colours, explained.
    int lx = x + 22, ly = view.y + view.h - 34;
    static const uint32_t kKinds[] = { 0x354a0515u, 0xba939a42u, 0xc2d067a7u, 0xb7ce0c18u, 0xdcfc7603u, 0x1918f808u };
    for (uint32_t k : kKinds) {
        FillDisc((float)(lx + 6), (float)(ly + 10), 5.f, RelTypeColor(k));
        const std::string l = RelLabel(k);
        DrawText(l, lx + 16, ly, gTheme.faint, Font::Xs);
        lx += 26 + TextW(l, Font::Xs);
    }
}

// ── TomodachiShare ───────────────────────────────────────────────────────────
static bool sShareAsk = false;
static void MiiSharePage(int x, int y, int w, const SDL_Rect& view) {
    SharePump();
    ToastFromMsg(gShareStatusMsg, gShareStatusCol);
    // Back, the search (whatever room is left), the order, and the pages as
    // one group at the right: laid out from the column's width, so nothing
    // runs into anything.
    const int bw = 52, bar = 10;   // button width, the space between them
    static const char* const kSort[] = { "mii.share.sort.new", "mii.share.sort.likes", "mii.share.sort.old" };
    const std::string sortLabel = Tx(kSort[gShareSort % 3]);
    const std::string pg = std::to_string(gSharePage) + " / " + std::to_string(std::max(1, gShareLastPage));
    const int pagerW = bw + TextW(pg, Font::Body) + 28 + bw;
    const int sortW = std::min(190, TextW(sortLabel, Font::Body) + 22 + 10 + 40);
    const int searchW = std::max(120, w - bw - sortW - pagerW - 3 * bar);
    int bx = x;
    W::Button(Ui::HashStr("share.back"), { bx, y, bw, 50 }, std::string(), W::Kind::Neutral, []() { MiiSetTab(0); }, UiIcons::Id::ChevronLeft);
    bx += bw + bar;
    W::Button(Ui::HashStr("share.search"), { bx, y, searchW, 50 }, gShareQuery.empty() ? Tx("mii.share.search") : "\xE2\x80\x9C" + gShareQuery + "\xE2\x80\x9D",
              W::Kind::Neutral, []() {
                  const std::string nq = ShowKeyboard(Tx("mii.share.search"), gShareQuery, 60);
                  if (nq != gShareQuery) { gShareQuery = nq; gSharePage = 1; ShareLoadPage(); }
              }, UiIcons::Id::Magnifier);
    bx += searchW + bar;
    W::Button(Ui::HashStr("share.sort"), { bx, y, sortW, 50 }, sortLabel, W::Kind::Neutral,
              []() { gShareSort = (gShareSort + 1) % 3; gSharePage = 1; ShareLoadPage(); }, UiIcons::Id::Funnel);
    // The pages: ‹ 1 / 143 › in one pill.
    const int px = x + w - pagerW;
    FillRectRounded(px, y, pagerW, 50, 16, Alpha(gTheme.panel, 200));
    DrawRectRounded(px, y, pagerW, 50, 16, Alpha(gTheme.text, 22));
    W::Button(Ui::HashStr("share.prev"), { px, y, bw, 50 }, std::string(), W::Kind::Neutral,
              []() { if (gSharePage > 1 && !gShareListLoading) { gSharePage--; ShareLoadPage(); } }, UiIcons::Id::ChevronLeft, gSharePage > 1);
    DrawTextC(pg, px + pagerW / 2, y + 25, gTheme.dim, Font::Body);
    W::Button(Ui::HashStr("share.next"), { px + pagerW - bw, y, bw, 50 }, std::string(), W::Kind::Neutral,
              []() { if (gSharePage < gShareLastPage && !gShareListLoading) { gSharePage++; ShareLoadPage(); } }, UiIcons::Id::ChevronRight,
              gSharePage < gShareLastPage);
    const int gy = y + 66;
    const SDL_Rect v2{ view.x, gy - 4, view.w, view.y + view.h - (gy - 4) };
    if (gShareListLoading && gShareMiis.empty()) {
        W::Spinner(x + w / 2, gy + 120, 26, gTheme.gold);
        return;
    }
    Focus::BeginScroll(Ui::HashStr("share.grid"), v2, sMii2Scroll);
    const int cols = 4, gap = 16, tw = (w - (cols - 1) * gap) / cols, th = tw + 52;
    for (int i = 0; i < (int)gShareMiis.size(); i++) {
        const auto& m = gShareMiis[i];
        const SDL_Rect r{ x + (i % cols) * (tw + gap), gy + (i / cols) * (th + gap) - (int)sMii2Scroll.offset, tw, th };
        const Ui::Id id = Ui::IdMix(Ui::HashStr("share.card"), (uint32_t)m.id);
        Focus::Node n;
        n.id = id; n.r = { r.x - 5, r.y - 5, r.w + 10, r.h + 10 }; n.radius = 22; n.hint = Tx("hint.open");
        const int idx = i;
        n.press = [idx]() { gShareSel = idx; ShareOpenDetail(); sShareAsk = true; Motion::Forget(Ui::HashStr("share.ask")); };
        n.flags = (uint16_t)(Focus::Ring | (i == 0 ? Focus::Default : 0));
        Focus::Add(std::move(n));
        if (r.y + r.h < v2.y || r.y > v2.y + v2.h) continue;
        W::BeginPress(id, r);
        Focus::DrawPlate(id);
        const bool f = Focus::Is(id);
        const SDL_Color face = Mix(gTheme.panel, gTheme.text, f ? 0.07f : 0.025f);
        FillRectRounded(r.x, r.y, r.w, r.h, 18, face);
        DrawRectRounded(r.x, r.y, r.w, r.h, 18, f ? Alpha(gTheme.gold, 150) : Alpha(gTheme.border, 200));
        const SDL_Rect box{ r.x + 8, r.y + 8, r.w - 16, r.w - 16 };
        FillRectRounded(box.x, box.y, box.w, box.h, 12, Mix(face, gTheme.text, 0.05f));
        if (m.thumb) { int ow = 0, oh = 0; SDL_QueryTexture(m.thumb, nullptr, nullptr, &ow, &oh); DrawPicture(m.thumb, ow, oh, box, 12, face); }
        else W::Spinner(box.x + box.w / 2, box.y + box.h / 2, 12, Alpha(gTheme.text, 90));
        W::Marquee(id, m.name, r.x + r.w / 2, r.y + r.w + 4, r.w - 16, gTheme.text, Font::Body, f, W::Anchor::Centre);
        W::Marquee(Ui::IdMix(id, 0xDE5Cu), "\xE2\x99\xA5 " + std::to_string(m.likes) + " \xC2\xB7 " + m.author, r.x + r.w / 2, r.y + r.w + 26, r.w - 16, gTheme.faint, Font::Xs, f, W::Anchor::Centre);
        W::EndPress();
    }
    const int rows = ((int)gShareMiis.size() + cols - 1) / cols;
    Focus::EndScroll((float)(rows * (th + gap) + 10));
    if (sShareAsk && gShareSel >= 0 && gShareSel < (int)gShareMiis.size()) {
        const auto& m = gShareMiis[gShareSel];
        const std::string target = gMiiStatsMiiSel < (int)gMiis.size() ? gMiis[gMiiStatsMiiSel].name : std::string("?");
        W::Dialog(Ui::HashStr("share.ask"), UiIcons::Id::Download, Ui::Tone::Warn, TokenFill(Tx("mii.share.ask"), "{name}", m.name),
                  TokenFill(Tx("mii.share.ask.body"), "{name}", target),
                  { { Tx("picker.cancel"), W::Kind::Neutral, []() { sShareAsk = false; ShareCloseDetail(); } },
                    { Tx("mii.import.go"), W::Kind::Primary, []() {
                        sShareAsk = false;
                        if (!gMiis.empty() && gMiiStatsMiiSel < (int)gMiis.size()) gShareImportSlot = gMiis[gMiiStatsMiiSel].slot;
                        ShareDoImport();
                        ShareCloseDetail();
                    }, (bool)m.allowCopying || m.fromSav } },
                  0, []() { sShareAsk = false; ShareCloseDetail(); }, Tx("hint.cancel"));
    }
}

// ── Dispatch ─────────────────────────────────────────────────────────────────
static void MiiSubPage(int sub, int x, int y, int w) {
    const int miiIdx = MiiIdx();
    const SDL_Rect view{ x - 8, y - 4, w + 16, SCREEN_H - 62 - (y - 4) };
    static int sLastSub = -1;
    if (sLastSub != sub) { sLastSub = sub; sMii2Scroll = Focus::Scroll{}; }
    switch ((MiiStatsSubTab)sub) {
        case MiiStatsSubTab::Relations:  MiiRelationsPage(miiIdx, x, y, w, view); break;
        case MiiStatsSubTab::Belongings: MiiItemsPage(miiIdx, x, y, w, view); break;
        case MiiStatsSubTab::Habits:     MiiHabitsPage(miiIdx, x, y, w, view); break;
        case MiiStatsSubTab::Housing:    MiiHousePage(miiIdx, x, y, w, view); break;
        case MiiStatsSubTab::Social:     MiiSocialPage(miiIdx, x, y, w, view); break;
        case MiiStatsSubTab::Browse:     MiiSharePage(x, y, w, view); break;
        default: break;
    }
}
