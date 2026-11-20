// page_miis.inc — TomoToolNX 2.0: the Miis place. The island's Miis in a
// list on the left (the highlight picks one), the chosen Mii on the right in
// sub-pages: Profile, Words, Relations, Items, Habits, House, Social. Every
// edit goes through 1.x's fields, undo slots and actions (MII_STATS_FIELDS,
// gMiiUndo*, gWordUndo, DoMiiImport, TDoMiiExport, TConfirmMiiDelete...), so
// what is written to Mii.sav does not change. Included by shell.inc in S2.

constexpr int kMiiListW = 252;
// The detail column, right of the list (it widens when the sidebar folds).
static int MiiDX() { return sEdX + kMiiListW + 18; }
static int MiiDW() { return sEdX + sEdW - MiiDX(); }

static Focus::Scroll sMiiListScroll, sMiiScroll;
static Ui::Id sMiiLastFocus = 0;
static bool sMiiImportAsk = false, sMiiDelAsk = false;
static std::string sMiiImportPath;
static int sMiiWordMenu = -1;

static Ui::Id MiiRowId(int slot) { return Ui::IdMix(Ui::HashStr("mii.row"), (uint32_t)slot); }
static const Ui::Id kMiiNewId = Ui::HashStr("mii.new");

static int MiiIdx() {
    if (gMiis.empty() || gMiiStatsMiiSel < 0 || gMiiStatsMiiSel >= (int)gMiis.size()) return -1;
    return gMiis[gMiiStatsMiiSel].slot - 1;
}

static void MiiSelect(int i) {
    if (i == gMiiStatsMiiSel || i < 0 || i >= (int)gMiis.size()) return;
    gMiiStatsMiiSel = i;
    gMiiBlankRowSel = false;
    // The undo slots belong to the Mii they were taken on.
    memset(gMiiUndoValid, 0, sizeof(gMiiUndoValid));
    gWordUndo.valid = false;
    gMiiRelSel = 0; gMiiRelScroll = 0; gBlPickerOpen = false;
    sMiiScroll.target = 0; sMiiScroll.offset = 0;
    W::ArriveContent(0);         // the list stays; the Mii's side changes
    W::ArriveContent(1);
}

static uint32_t MiiAvatarSeed(int slot, const std::string& name) {
    uint32_t h = 2166136261u ^ (uint32_t)slot;
    for (unsigned char c : name) h = (h ^ c) * 16777619u;
    return h;
}
// Which face the avatar shows: 0 a boy's, 1 a girl's (the Mii's gender in
// Mii.sav), -1 the initial when the save is not loaded. `slot` is 1-based.
static int MiiFace(int slot) {
    static const uint32_t H_GENDER = SaveEditor::Hash("Mii.MiiMisc.FaceInfo.Gender");
    static const uint32_t H_FEMALE = SaveEditor::Hash("Female");
    if (!gMiiSav.loaded || slot < 1) return -1;
    return SaveEditor::GetEnumAt(gMiiSav, H_GENDER, slot - 1) == H_FEMALE ? 1 : 0;
}

// The colour of a Mii's hair, from its CharInfoEx (hairColor0 at 0x46, one of
// the game's 100 colours; layout and colours from tlmodding/charinfo-ex, MIT).
// Transparent when the save is not loaded or the colour is black anyway.
static SDL_Color MiiHairColor(int slot) {
    static const uint32_t kPalette[100] = {
        0x2D2828, 0x402010, 0x5C180A, 0x7C3A14, 0x787880, 0x4E3E10, 0x885818, 0xD0A04A, 0x000000, 0x6C7070,
        0x663C2C, 0x605E30, 0x4654A8, 0x387058, 0x603810, 0xA81008, 0x203068, 0xA86000, 0x787068, 0xD85208,
        0xF00C08, 0xF54848, 0xF09A74, 0x8C5040, 0x842626, 0xFF7366, 0xFFA6A6, 0xFFC0BA, 0x732E3B, 0x991F3D,
        0x8A173E, 0xB53E42, 0xC71E56, 0xB05381, 0xC7546E, 0xFA7597, 0xFCACC9, 0xFFC9D8, 0x311C40, 0x37283D,
        0x4C184D, 0x6F42B3, 0x855CB8, 0xC083CC, 0xA893C9, 0xC5ACE6, 0xEEBEFA, 0xD2C5ED, 0x191F40, 0x123F66,
        0x2A82D4, 0x57B4F2, 0x7AC5DE, 0x89A6FA, 0x84BDFA, 0xA1E3FF, 0x0B2E36, 0x013D3B, 0x0D4F59, 0x236663,
        0x307E8C, 0x4FAEB0, 0x7AC49E, 0x7FD4C0, 0x87E5B6, 0x0A4A35, 0x437A00, 0x027562, 0x369970, 0x4BAD1A,
        0x92BF0A, 0x63C788, 0x9EE042, 0x96DE7E, 0xBBF2AA, 0x99932B, 0xA69563, 0xCCC039, 0xCCB987, 0xD9CC82,
        0xD5D96F, 0xD5E683, 0xD8FA9D, 0x7D4500, 0xE6BB7A, 0xFEE24A, 0xFADE82, 0xF7EA9C, 0xFAF89B, 0xA64D1E,
        0xFF960D, 0xD19B69, 0xFFB266, 0xFFC28C, 0xE5CFB1, 0x414141, 0x9B9B9B, 0xBEBEBE, 0xDCD7CD, 0xFFFFFF,
    };
    static const uint32_t H_CI = SaveEditor::Hash("Mii.CharInfoEx");
    if (!gMiiSav.loaded || slot < 1) return SDL_Color{ 0, 0, 0, 0 };
    size_t n = 0;
    const uint8_t* ci = SaveEditor::GetBinAt(gMiiSav, H_CI, slot - 1, &n);
    if (!ci || n <= 0x46 || ci[0x46] >= 100) return SDL_Color{ 0, 0, 0, 0 };
    const uint32_t v = kPalette[ci[0x46]];
    const SDL_Color c{ (Uint8)(v >> 16), (Uint8)(v >> 8), (Uint8)v, 255 };
    return ColorLuma(c) < 24 ? SDL_Color{ 0, 0, 0, 0 } : c;
}

// The Mii's real picture, from the Mii parts pack (mii_art.h), with its face
// paint when it has one; nullptr without a pack, and the drawn face stays.
static SDL_Texture* MiiPicture(int slot) {
    static const uint32_t H_CI = SaveEditor::Hash("Mii.CharInfoEx");
    static const uint32_t H_PAINT = SaveEditor::Hash("Mii.MiiMisc.FaceInfo.FacePaintIndex");
    static const uint32_t H_PAINT_FRONT = SaveEditor::Hash("Mii.MiiMisc.FaceInfo.IsFacePaintFront");
    if (!gMiiSav.loaded || slot < 1 || !MiiArt::Available()) return nullptr;
    size_t n = 0;
    const uint8_t* ci = SaveEditor::GetBinAt(gMiiSav, H_CI, slot - 1, &n);
    if (!ci) return nullptr;
    const MiiArt::FacePaint* paint = nullptr;
    const int32_t pi = SaveEditor::GetIntAt(gMiiSav, H_PAINT, slot - 1, -1);
    if (pi >= 0 && pi < 1000) {
        char path[64];
        snprintf(path, sizeof path, "%s/UgcFacePaint%03d.canvas.zs", SAVE_UGC_DIR, (int)pi);
        paint = MiiArt::LoadFacePaint(path);
    }
    return MiiArt::Portrait(ci, n, paint, paint && SaveEditor::GetBoolAt(gMiiSav, H_PAINT_FRONT, slot - 1, false));
}

static int MiiField(const char* label) {
    for (int i = 0; i < MII_STATS_FIELD_COUNT; i++) if (strcmp(MII_STATS_FIELDS[i].label, label) == 0) return i;
    return 0;
}

// ── Every Mii at once ────────────────────────────────────────────────────────
static bool sMiiOverview = false;
static void MiiOpenOverview() {
    if (gMiis.empty()) return;
    sMiiOverview = true;
    Motion::Forget(Ui::HashStr("ov.sheet"));
}
static void MiiOverviewSheet() {
    static const uint32_t H_LV = SaveEditor::Hash("Mii.MiiMisc.SatisfyInfo.Level");
    Overview o;
    o.title = Tx("tab.mii");
    o.sub = CountText((int)gMiis.size(), "bk.mii1", "bk.miis");
    o.count = (int)gMiis.size();
    o.current = gMiiStatsMiiSel;
    o.cols = 8;
    o.cellH = 134;
    o.draw = [](int i, SDL_Rect r, bool f, SDL_Color) {
        if (i < 0 || i >= (int)gMiis.size()) return;
        const auto& m = gMiis[i];
        W::Avatar(r.x + r.w / 2, r.y + 50, 34, m.name, MiiAvatarSeed(m.slot, m.name), MiiFace(m.slot), MiiHairColor(m.slot), MiiPicture(m.slot));
        W::Marquee(Ui::IdMix(Ui::HashStr("mii.ov.cell"), (uint32_t)i), m.name.empty() ? std::string("?") : m.name, r.x + r.w / 2, r.y + 100, r.w - 14, f ? gTheme.text : Mix(gTheme.text, gTheme.dim, 0.25f), Font::Body, f, W::Anchor::Centre);
        const int lv = gMiiSav.loaded ? SaveEditor::GetIntAt(gMiiSav, H_LV, m.slot - 1) + 1 : 0;
        DrawTextC(Tx("mii.level.short") + " " + std::to_string(lv), r.x + r.w / 2, r.y + 120, gTheme.faint, Font::Xs);
    };
    o.pick = [](int i) {
        if (i < 0 || i >= (int)gMiis.size()) return;
        MiiSelect(i);
        Focus::Set(MiiRowId(gMiis[i].slot));
        Sound::Play(Sound::Voice::Select);
    };
    o.close = []() { sMiiOverview = false; };
    OverviewSheet(o);
}

// ── The list ─────────────────────────────────────────────────────────────────
static void MiiList(int top) {
    const int x = sEdX, w = kMiiListW;
    // + New Mii, and every Mii at once.
    W::Button(kMiiNewId, { x, top, w - 60, 50 }, Tx("mii.new"), W::Kind::Soft, []() { TDoCreateBlankMii(0); W::Arrive(false); },
              UiIcons::Id::Plus);
    W::Button(Ui::HashStr("mii.overview"), { x + w - 50, top, 50, 50 }, std::string(), W::Kind::Neutral, []() { MiiOpenOverview(); },
              UiIcons::Id::Grid, !gMiis.empty());
    const SDL_Rect view{ x - 6, top + 62, w + 12, SCREEN_H - 62 - (top + 62) };
    const int rowH = 68;
    Focus::BeginScroll(Ui::HashStr("mii.list"), view, sMiiListScroll);
    static const uint32_t H_LV = SaveEditor::Hash("Mii.MiiMisc.SatisfyInfo.Level");
    for (int i = 0; i < (int)gMiis.size(); i++) {
        const auto& m = gMiis[i];
        const SDL_Rect r{ x, view.y + 6 + i * rowH - (int)sMiiListScroll.offset, w, rowH - 6 };
        const Ui::Id id = MiiRowId(m.slot);
        Focus::Node n;
        n.id = id; n.r = r; n.radius = 16; n.hint = Tx("hint.open");
        const int idx = i;
        n.press = [idx]() { MiiSelect(idx); if (!Input::Last().tap) Focus::Set(0); };
        if (i == gMiiStatsMiiSel) n.flags = Focus::Default;
        Focus::Add(std::move(n));
        if (r.y + r.h < view.y || r.y > view.y + view.h) continue;
        const bool sel = i == gMiiStatsMiiSel, f = Focus::Is(id);
        W::BeginPress(id, r);
        if (sel) {
            FillRectRounded(r.x, r.y, r.w, r.h, 16, Alpha(gTheme.gold, f ? 70 : 44));
            DrawRectRounded(r.x, r.y, r.w, r.h, 16, Alpha(gTheme.gold, 90));
        } else Focus::DrawPlate(id);
        W::Avatar(r.x + 30, r.y + r.h / 2, 21, m.name, MiiAvatarSeed(m.slot, m.name), MiiFace(m.slot), MiiHairColor(m.slot), MiiPicture(m.slot));
        // Name and level, both centred inside the row (the level used to hang
        // below it: the line box of this font is twice its size).
        const int cy = r.y + r.h / 2;
        W::Marquee(id, m.name.empty() ? std::string("?") : m.name, r.x + 60, cy - 11, r.w - 74, sel || f ? gTheme.text : Mix(gTheme.text, gTheme.dim, 0.3f), Font::Row, f, W::Anchor::Middle);
        const int lv = gMiiSav.loaded ? SaveEditor::GetIntAt(gMiiSav, H_LV, m.slot - 1) + 1 : 0;
        DrawTextVC(Tx("mii.level.short") + " " + std::to_string(lv), r.x + 60, cy + 15, gTheme.faint, Font::Xs);
        W::EndPress();
    }
    Focus::EndScroll((float)(gMiis.size() * rowH + 12));
}

// ── Profile ──────────────────────────────────────────────────────────────────
struct MiiUndoRow { Ui::Id id; int field; };
static std::vector<MiiUndoRow> sMiiUndoRows;

static void MiiAdjust(int fi, int delta) {
    const int miiIdx = MiiIdx();
    if (miiIdx < 0) return;
    const auto& fd = MII_STATS_FIELDS[fi];
    const uint32_t h = SaveEditor::Hash(fd.fieldName);
    if (fd.isUInt) {
        const uint32_t v = SaveEditor::GetUIntAt(gMiiSav, h, miiIdx);
        if (!gMiiUndoValid[fi]) { gMiiUndoVal[fi] = (int32_t)v; gMiiUndoValid[fi] = true; }
        int64_t nv = (int64_t)v + delta;
        if (nv < 0) nv = 0;
        if (fd.minVal >= 0 && nv < fd.minVal) nv = fd.minVal;
        if (fd.maxVal != -1 && nv > fd.maxVal) nv = fd.maxVal;
        SaveEditor::SetUIntAt(gMiiSav, h, miiIdx, (uint32_t)nv);
    } else {
        int32_t v = SaveEditor::GetIntAt(gMiiSav, h, miiIdx);
        if (!gMiiUndoValid[fi]) { gMiiUndoVal[fi] = v; gMiiUndoValid[fi] = true; }
        v += delta;
        if (fd.minVal != -1 && v < fd.minVal) v = fd.minVal;
        if (fd.maxVal != -1 && v > fd.maxVal) v = fd.maxVal;
        SaveEditor::SetIntAt(gMiiSav, h, miiIdx, v);
    }
    gMiiSavDirty = true;
}

static int MiiGet(int fi) {
    const int miiIdx = MiiIdx();
    if (miiIdx < 0) return 0;
    const auto& fd = MII_STATS_FIELDS[fi];
    const uint32_t h = SaveEditor::Hash(fd.fieldName);
    return fd.isUInt ? (int)SaveEditor::GetUIntAt(gMiiSav, h, miiIdx) : SaveEditor::GetIntAt(gMiiSav, h, miiIdx);
}

struct MiiCard {
    int x, y, w, rowY;
    void Begin(const std::string& name, int rows) {
        W::Eyebrow(x + 6, y, name, gTheme.faint);
        W::Card({ x, y + 22, w, rows * W::kRowH + std::max(0, rows - 1) + 12 });
        rowY = y + 22 + 6;
    }
    SDL_Rect Row(bool last = false) {
        SDL_Rect r{ x + 6, rowY, w - 12, W::kRowH };
        rowY += W::kRowH;
        if (!last) { W::Hairline(x + 24, rowY, w - 48); rowY += 1; }
        return r;
    }
    int End() const { return rowY + 6 + 26; }
};

static void MiiNumberRow(MiiCard& c, const char* id, int fi, int lo, int hi, int stepBy, bool slider, bool last) {
    const auto& fd = MII_STATS_FIELDS[fi];
    const int v = MiiGet(fi);
    const int shown = v + fd.dispOffset;
    const Ui::Id rid = Ui::HashStr(id);
    const bool atLo = fd.minVal != -1 ? v <= fd.minVal : (!fd.isUInt ? false : v <= 0);
    const bool atHi = fd.maxVal != -1 && v >= fd.maxVal;
    float frac = hi > lo ? (float)(v - lo) / (hi - lo) : 0.5f;
    frac = atLo ? 0.f : atHi ? 1.f : std::max(0.02f, std::min(0.98f, frac));
    const W::RowText t{ MiiLabelT(fd.label), MiiDescT(fd.label) };
    // A (or a tap on the number) types it.
    auto type = [fi]() {
        const auto& f = MII_STATS_FIELDS[fi];
        const int was = MiiGet(fi) + f.dispOffset;
        const std::string s = ShowNumberKeyboard(MiiLabelT(f.label), std::to_string(was), 10);
        if (s.empty()) return;
        const long v = strtol(s.c_str(), nullptr, 10);
        if (v != was) { MiiAdjust(fi, (int)(v - was)); Sound::Play(Sound::Voice::Ok, { 0.5f, false, 0.6f }); }
    };
    if (slider) {
        W::RowSlider(rid, c.Row(last), t, frac, hi > lo ? (float)stepBy / (hi - lo) : 0.f, std::to_string(shown),
                     [fi, stepBy](int d) { MiiAdjust(fi, d * stepBy); }, type);
    } else {
        W::RowStepper(rid, c.Row(last), t, std::to_string(shown), frac, hi > lo ? (float)stepBy / (hi - lo) : 0.f,
                      [fi, stepBy](int d) { MiiAdjust(fi, d * stepBy); }, type, Tx("hint.type"));
    }
    sMiiUndoRows.push_back({ rid, fi });
}

// TomodachiShare's own picture (romfs:/gfx/logo_tomodachishare.png), fitted
// in a square `size` wide; the globe when the build has no picture.
static void DrawShareMark(int cx, int cy, int size) {
    if (Art::Has(Art::Id::LogoShare)) {
        Art::Draw(Art::Id::LogoShare, SDL_Rect{ cx - size / 2, cy - size / 2, size, size });
        return;
    }
    FillDisc((float)cx, (float)cy, size / 2.f, SDL_Color{ 247, 147, 30, 120 });
    UiIcons::Draw(UiIcons::Id::Globe, cx, cy, size / 2, gTheme.text);
}

// One of a Mii's file actions, as a tile: its colour in a disc with the icon
// (or a mark of its own), a short title and a line under it.
static void MiiActionTile(Ui::Id id, SDL_Rect r, UiIcons::Id icon, SDL_Color tint, const std::string& title, const std::string& sub,
                          std::function<void()> press, void (*mark)(int, int, int) = nullptr) {
    Focus::Node n;
    n.id = id; n.r = { r.x - 5, r.y - 5, r.w + 10, r.h + 10 }; n.radius = 25; n.flags = Focus::Ring; n.hint = Tx("hint.open");
    n.press = std::move(press);
    Focus::Add(std::move(n));
    W::BeginPress(id, r);
    Focus::DrawPlate(id);
    const bool f = Focus::Is(id);
    FillRectRounded(r.x, r.y + 3, r.w, r.h, 20, SDL_Color{ 0, 0, 0, 40 });
    FillRectRounded(r.x, r.y, r.w, r.h, 20, Mix(gTheme.panel, tint, f ? 0.18f : 0.09f));
    DrawRectRounded(r.x, r.y, r.w, r.h, 20, Alpha(tint, f ? 160 : 70));
    const int cx = r.x + 36, cy = r.y + r.h / 2;
    if (mark) mark(cx, cy, f ? 48 : 44);
    else {
        FillDisc((float)cx, (float)cy, 23.f, Alpha(tint, f ? 120 : 75));
        UiIcons::Draw(icon, cx, cy, 24, f ? gTheme.text : Mix(tint, gTheme.text, 0.55f));
    }
    const int tx = r.x + 70, tw = r.w - 70 - 14;
    W::Marquee(id, title, tx, cy - 12, tw, gTheme.text, Font::Row, f, W::Anchor::Middle);
    W::Marquee(Ui::IdMix(id, 0xDE5Cu), sub, tx, cy + 14, tw, gTheme.dim, Font::Xs, f, W::Anchor::Middle);
    W::EndPress();
}

static void MiiProfile(int x, int y, int w) {
    const int miiIdx = MiiIdx();
    static const uint32_t H_NAME = SaveEditor::Hash("Mii.Name.Name");
    static const uint32_t H_HOW = SaveEditor::Hash("Mii.Name.HowToCallName");
    const int top = y;
    // The Mii as a file, first: replace it, save it, or find another online.
    {
        constexpr int kGap = 12, kH = 84;
        const int tw = (w - 2 * kGap) / 3;
        MiiActionTile(Ui::HashStr("mii.import"), { x, y, tw, kH }, UiIcons::Id::Upload, gTheme.gold, Tx("mii.act.replace"),
                      Tx("mii.act.replace.sub"), []() {
                          OpenFileSheet(true, gBrowseLtdPath, Tx("mii.import"), [](const std::string& path) {
                              sMiiImportPath = path;
                              sMiiImportAsk = true;
                              Motion::Forget(Ui::HashStr("mii.importask"));
                          });
                      });
        MiiActionTile(Ui::HashStr("mii.export"), { x + tw + kGap, y, tw, kH }, UiIcons::Id::Download, Ui::ToneColor(Ui::Tone::Good),
                      Tx("mii.act.save"), Tx("mii.act.save.sub"), []() { TDoMiiExport(0); });
        MiiActionTile(Ui::HashStr("mii.share"), { x + 2 * (tw + kGap), y, w - 2 * (tw + kGap), kH }, UiIcons::Id::Globe,
                      SDL_Color{ 247, 147, 30, 255 }, Tx("mii.act.share"), Tx("mii.act.share.sub"),
                      []() { TOpenMiiShareBrowser(0); W::Arrive(false); }, DrawShareMark);
        y += kH + 24;
    }
    MiiCard c{ x, y, w, 0 };
    // Name
    c.Begin(Tx("mii.card.name"), 2);
    {
        const std::string name = SaveEditor::GetWStr32At(gMiiSav, H_NAME, miiIdx);
        W::RowLink(Ui::HashStr("mii.name"), c.Row(), { MiiLabelT("Name"), MiiDescT("Name") }, name, [miiIdx]() {
            const std::string cur = SaveEditor::GetWStr32At(gMiiSav, H_NAME, miiIdx);
            const std::string nv = ShowKeyboard(MiiLabelT("Name"), cur, 30);
            if (nv != cur) {
                SaveEditor::SetWStr32At(gMiiSav, H_NAME, miiIdx, nv);
                gMiiSavDirty = true;
                for (auto& m : gMiis) if (m.slot - 1 == miiIdx) m.name = nv;   // the list shows it at once
            }
        }, UiIcons::Id::Pencil, false, 0, !name.empty());
        const std::string how = SaveEditor::GetWStr64At(gMiiSav, H_HOW, miiIdx);
        W::RowLink(Ui::HashStr("mii.how"), c.Row(true), { Tx("pl.pron"), Tx("pl.pron.desc") }, how.empty() ? Tx("pl.pron.same") : how,
                   [miiIdx]() {
                       const std::string cur = SaveEditor::GetWStr64At(gMiiSav, H_HOW, miiIdx);
                       const std::string nv = ShowKeyboard(Tx("pl.pron"), cur, 63);
                       if (nv != cur) { SaveEditor::SetWStr64At(gMiiSav, H_HOW, miiIdx, nv); gMiiSavDirty = true; }
                   }, UiIcons::Id::Pencil, false, 0, !how.empty());
        c.y = c.End();
    }
    // Personality: 1 to 10 each.
    c.Begin(Tx("mii.card.personality"), 5);
    MiiNumberRow(c, "mii.act", MiiField("Activeness"), 1, 10, 1, true, false);
    MiiNumberRow(c, "mii.aud", MiiField("Audacity"), 1, 10, 1, true, false);
    MiiNumberRow(c, "mii.com", MiiField("Commonsense"), 1, 10, 1, true, false);
    MiiNumberRow(c, "mii.gai", MiiField("Gaiety"), 1, 10, 1, true, false);
    MiiNumberRow(c, "mii.soc", MiiField("Sociability"), 1, 10, 1, true, true);
    c.y = c.End();
    // Life on the island.
    c.Begin(Tx("mii.card.life"), 6);
    MiiNumberRow(c, "mii.lv", MiiField("Level"), 0, 99, 1, false, false);
    MiiNumberRow(c, "mii.lvm", MiiField("Lv Meter"), 0, 100, 10, true, false);
    MiiNumberRow(c, "mii.bond", MiiField("Bond Meter"), 0, 100, 10, true, false);
    {
        const int fi = MiiField("Mood");
        const uint32_t fh = SaveEditor::Hash(MII_STATS_FIELDS[fi].fieldName);
        const uint32_t cur = SaveEditor::GetEnumAt(gMiiSav, fh, miiIdx);
        int ci = 0;
        for (int k = 0; k < FEELING_LABEL_COUNT; k++) if (FEELING_LABELS[k].hash == cur) ci = k;
        const Ui::Id rid = Ui::HashStr("mii.mood");
        std::vector<std::string> moods;
        for (int k = 0; k < FEELING_LABEL_COUNT; k++) moods.push_back(Tx((std::string("mii.mood.") + FEELING_LABELS[k].label).c_str()));
        W::RowChoice(rid, c.Row(), { MiiLabelT("Mood"), MiiDescT("Mood") }, moods, ci, [fi, fh, cur, ci, miiIdx](int d) {
                          if (!gMiiUndoValid[fi]) { gMiiUndoVal[fi] = (int32_t)cur; gMiiUndoValid[fi] = true; }
                          const int n = std::max(0, std::min(FEELING_LABEL_COUNT - 1, ci + d));
                          SaveEditor::SetEnumAt(gMiiSav, fh, miiIdx, FEELING_LABELS[n].hash);
                          // So the new mood sticks in the game (1.x's fix).
                          RelMoodPersistAndCalmFights(miiIdx);
                          gMiiSavDirty = true;
                      });
        sMiiUndoRows.push_back({ rid, fi });
    }
    MiiNumberRow(c, "mii.full", MiiField("Fullness"), 0, 200, 10, false, false);
    MiiNumberRow(c, "mii.money", MiiField("Money"), 0, 99999, 100, false, true);
    c.y = c.End();
    // Birthday.
    c.Begin(Tx("mii.card.birthday"), 2);
    {
        // The birthday on one row, a calendar behind it (◀ ▶: a day).
        const int fD = MiiField("Bday Day"), fM = MiiField("Bday Month"), fY = MiiField("Birth Year");
        const int d = MiiGet(fD) + MII_STATS_FIELDS[fD].dispOffset, mo = MiiGet(fM) + MII_STATS_FIELDS[fM].dispOffset;
        const int yr = MiiGet(fY) + MII_STATS_FIELDS[fY].dispOffset;
        auto setDate = [fD, fM, fY](int ny, int nm, int nd) {
            const int dy = ny - (MiiGet(fY) + MII_STATS_FIELDS[fY].dispOffset);
            const int dm = nm - (MiiGet(fM) + MII_STATS_FIELDS[fM].dispOffset);
            const int dd = nd - (MiiGet(fD) + MII_STATS_FIELDS[fD].dispOffset);
            if (dy) MiiAdjust(fY, dy);
            if (dm) MiiAdjust(fM, dm);
            if (dd) MiiAdjust(fD, dd);
        };
        const Ui::Id rid = Ui::HashStr("mii.bday");
        W::RowStepper(rid, c.Row(false), { Tx("pl.bday"), Tx("pl.bday.desc") },
                      (mo >= 1 && mo <= 12) ? DateText(yr, mo, d) : std::to_string(d) + "/" + std::to_string(mo) + "/" + std::to_string(yr),
                      0.5f, 0.f, [d, mo, yr, setDate](int dd) {
                          static const int k[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
                          auto dim = [](int y_, int m_) { return m_ == 2 && (y_ % 4 == 0 && (y_ % 100 != 0 || y_ % 400 == 0)) ? 29 : k[m_ - 1]; };
                          int nd = d + dd, nm = std::max(1, std::min(12, mo)), ny = yr;
                          if (nd < 1) { if (--nm < 1) { nm = 12; ny--; } nd = dim(ny, nm); }
                          else if (nd > dim(ny, nm)) { nd = 1; if (++nm > 12) { nm = 1; ny++; } }
                          setDate(ny, nm, nd);
                      },
                      [d, mo, yr, setDate]() {
                          W::OpenCalendar(Ui::HashStr("mii.bday"), Tx("pl.bday"), yr, mo, d, std::min(1900, yr), std::max(2100, yr), setDate);
                      },
                      Tx("hint.date"));
        sMiiUndoRows.push_back({ rid, fY });
        sMiiUndoRows.push_back({ rid, fM });
        sMiiUndoRows.push_back({ rid, fD });
    }
    MiiNumberRow(c, "mii.age", MiiField("Direct Age"), -1, 99, 1, false, true);
    c.y = c.End();
    // Deleting it, on its own at the end.
    c.Begin(Tx("mii.card.danger"), 1);
    W::RowLink(Ui::HashStr("mii.delete"), c.Row(true), { Tx("mii.delete"), MiiDescT("Delete Mii") }, std::string(), []() {
        TOpenMiiDeleteConfirm(0);
        gMiiDeleteConfirmOpen = false;   // the 2.0 dialog asks
        sMiiDelAsk = true;
        Motion::Forget(Ui::HashStr("mii.delask"));
    }, UiIcons::Id::Trash, true);
    c.y = c.End();
    sMiiScroll.content = (float)(c.y - top);   // (top already moved with the scroll: adding it again let a finger drag on forever)
}

// ── Words ────────────────────────────────────────────────────────────────────
static void MiiWords(int x, int y, int w) {
    const int miiIdx = MiiIdx();
    static const uint32_t H_KIND = SaveEditor::Hash("Mii.MiiMisc.WordInfo.WordArray.WordKind");
    static const uint32_t H_TXT  = SaveEditor::Hash("Mii.MiiMisc.WordInfo.WordArray.WordText");
    static const uint32_t H_HOW  = SaveEditor::Hash("Mii.MiiMisc.WordInfo.WordArray.WordHowToCall");
    static const uint32_t H_REG  = SaveEditor::Hash("Mii.MiiMisc.WordInfo.WordArray.WordRegionLanguageID");
    static const uint32_t H_INV  = SaveEditor::Hash("Invalid");
    static const uint32_t H_USEN = SaveEditor::Hash("USen");
    MiiCard c{ x, y, w, 0 };
    c.Begin(Tx("mii.card.words"), 12);
    for (int s = 0; s < 12; s++) {
        const int wIdx = miiIdx * 12 + s;
        const uint32_t kind = SaveEditor::GetAnyEnumAt(gMiiSav, H_KIND, wIdx, H_INV);
        int ki = WordKindIndex(kind);
        if (ki < 0) ki = 0;
        const std::string text = SaveEditor::GetWStr64At(gMiiSav, H_TXT, wIdx);
        const std::string how = SaveEditor::GetWStr64At(gMiiSav, H_HOW, wIdx);
        const Ui::Id rid = Ui::IdMix(Ui::HashStr("mii.word"), (uint32_t)s);
        std::string desc = how.empty() ? std::string() : Tx("pl.pron") + ": " + how;
        W::RowStepper(rid, c.Row(s == 11), { text.empty() ? Tx("mii.word.empty") : text, desc },
                      Tx((std::string("mii.wordkind.") + WORD_KIND_LIST[ki].name).c_str()), (float)ki / (WORD_KIND_COUNT - 1),
                      1.f / (WORD_KIND_COUNT - 1),
                      [s, wIdx, kind, ki, text, how](int d) {
                          gMiiWordsSlotSel = s;
                          if (!gWordUndo.valid) { gWordUndo.kind = kind; gWordUndo.text = text; gWordUndo.how = how; gWordUndo.valid = true; }
                          const int nk = std::max(0, std::min(WORD_KIND_COUNT - 1, ki + d));
                          SaveEditor::SetAnyEnumAt(gMiiSav, H_KIND, wIdx, SaveEditor::Hash(WORD_KIND_LIST[nk].name));
                          if (kind == H_INV && nk != 0) SaveEditor::SetAnyEnumAt(gMiiSav, H_REG, wIdx, H_USEN);
                          gMiiSavDirty = true;
                      },
                      [s]() { if (gMiiWordsSlotSel != s) gWordUndo.valid = false; gMiiWordsSlotSel = s; sMiiWordMenu = s; Motion::Forget(Ui::HashStr("mii.wordmenu")); },
                      Tx("hint.edit"));
        if (Focus::Is(rid) && gWordUndo.valid && gMiiWordsSlotSel == s)
            Focus::Command(Input::X, Tx("hint.undo"), []() { TUndoWords(0); Sound::Play(Sound::Voice::Undo); });
        // Moving to another word forgets the previous one's undo (1.x did too).
        if (Focus::Is(rid) && gMiiWordsSlotSel != s) { gMiiWordsSlotSel = s; gWordUndo.valid = false; }
    }
    c.y = c.End();
    sMiiScroll.content = (float)(c.y - y);   // (y already moved with the scroll: adding it again let a finger drag on forever)
    if (sMiiWordMenu >= 0) {
        const int s = sMiiWordMenu;
        const int wIdx = miiIdx * 12 + s;
        std::vector<W::MenuItem> items;
        items.push_back({ Tx("mii.word.text"), "", UiIcons::Id::Pencil, false, [wIdx]() {
            sMiiWordMenu = -1;
            const uint32_t kind = SaveEditor::GetAnyEnumAt(gMiiSav, H_KIND, wIdx, H_INV);
            const std::string cur = SaveEditor::GetWStr64At(gMiiSav, H_TXT, wIdx);
            const std::string nv = ShowKeyboard(Tx("mii.word.text"), cur, 63);
            if (nv == cur) return;
            if (!gWordUndo.valid) { gWordUndo.kind = kind; gWordUndo.text = cur; gWordUndo.how = SaveEditor::GetWStr64At(gMiiSav, H_HOW, wIdx); gWordUndo.valid = true; }
            SaveEditor::SetWStr64At(gMiiSav, H_TXT, wIdx, nv);
            if (!nv.empty() && kind == H_INV) {
                SaveEditor::SetAnyEnumAt(gMiiSav, H_KIND, wIdx, SaveEditor::Hash("Phrase"));
                SaveEditor::SetAnyEnumAt(gMiiSav, H_REG, wIdx, H_USEN);
            }
            gMiiSavDirty = true;
        }, false });
        items.push_back({ Tx("mii.word.how"), "", UiIcons::Id::Chat, false, [wIdx]() {
            sMiiWordMenu = -1;
            const uint32_t kind = SaveEditor::GetAnyEnumAt(gMiiSav, H_KIND, wIdx, H_INV);
            const std::string cur = SaveEditor::GetWStr64At(gMiiSav, H_HOW, wIdx);
            const std::string nv = ShowKeyboard(Tx("mii.word.how"), cur, 63);
            if (nv == cur) return;
            if (!gWordUndo.valid) { gWordUndo.kind = kind; gWordUndo.text = SaveEditor::GetWStr64At(gMiiSav, H_TXT, wIdx); gWordUndo.how = cur; gWordUndo.valid = true; }
            SaveEditor::SetWStr64At(gMiiSav, H_HOW, wIdx, nv);
            gMiiSavDirty = true;
        }, false });
        W::Menu(Ui::HashStr("mii.wordmenu"), Tx("mii.word") + " " + std::to_string(s + 1), items, []() { sMiiWordMenu = -1; },
                Tx("hint.close"), 0, Focus::RectOf(Ui::IdMix(Ui::HashStr("mii.word"), (uint32_t)s)));
    }
}

// ── The page ─────────────────────────────────────────────────────────────────
static void MiiSubPage(int sub, int x, int y, int w);   // the other sub-pages (page_miis2.inc)

static const char* const kMiiTabs[] = { "mii.tab.profile", "mii.tab.words", "mii.tab.relations", "mii.tab.items",
                                        "mii.tab.habits", "mii.tab.house", "mii.tab.social" };
constexpr int kMiiTabN = 7;

static void MiiSetTab(int t) {
    t = (t % kMiiTabN + kMiiTabN) % kMiiTabN;
    if ((int)gMiiStatsSubTab == t) return;
    gMiiStatsSubTab = (MiiStatsSubTab)t;
    gSocialScroll = 0; gBlPickerOpen = false;
    if (gMiiStatsSubTab != MiiStatsSubTab::Social) gSocialExpanded = false;
    if (gMiiStatsSubTab == MiiStatsSubTab::Housing) { HousingRebuild(); gHousingNavSel = 0; gHousingScrollY = 0; gHousingScrollYF = 0.f; gHousingSyLast = 0; }
    sMiiScroll.target = 0; sMiiScroll.offset = 0;
    W::ArriveContent();          // only the new tab's content comes in
}

static void PageMiis() {
    if (!gMiiSav.loaded) {
        static double sTry = -10;
        if (Motion::Now() - sTry > 2.0) {
            sTry = Motion::Now();
            std::string err;
            if (!SaveEditor::Load(SAVE_MII_SAV, gMiiSav, err)) gMiiStatsMsg = "Load error: " + err;
            std::string mapErr;
            if (!gMapSav.loaded) SaveEditor::Load(SAVE_MAP_SAV, gMapSav, mapErr);
        }
    }
    ToastFromMsg(gMiiStatsMsg, gMiiStatsMsgCol);
    if (gMiiStatsMiiSel >= (int)gMiis.size()) gMiiStatsMiiSel = std::max(0, (int)gMiis.size() - 1);
    // The highlight picks the Mii in the list; coming back from the right
    // with ◀ lands on the Mii already shown.
    {
        const Ui::Id cur = Focus::Current();
        int curRow = -1;
        bool lastWasRow = false;
        for (int i = 0; i < (int)gMiis.size(); i++) {
            if (cur == MiiRowId(gMiis[i].slot)) curRow = i;
            if (sMiiLastFocus == MiiRowId(gMiis[i].slot)) lastWasRow = true;
        }
        if (curRow >= 0 && curRow != gMiiStatsMiiSel) {
            if (!lastWasRow && sMiiLastFocus != 0 && !IsSidebarId(sMiiLastFocus) && !Input::Last().tap && gMiiStatsMiiSel < (int)gMiis.size())
                Focus::Set(MiiRowId(gMiis[gMiiStatsMiiSel].slot));
            else MiiSelect(curRow);
        }
    }
    const int top = 34;
    MiiList(top);
    if (!gMiiSav.loaded || gMiis.empty()) {
        const SDL_Rect cc{ MiiDX(), top, MiiDW(), 200 };
        W::Card(cc);
        DrawTextC(gMiiSav.loaded ? Tx("mii.none") : (gMiiStatsMsg.empty() ? Tx("player.loading") : gMiiStatsMsg), cc.x + cc.w / 2, cc.y + 100,
                  gTheme.dim, Font::Row);
        sMiiLastFocus = Focus::Current();
        return;
    }
    const auto& m = gMiis[gMiiStatsMiiSel];
    const int miiIdx = m.slot - 1;
    // Who: avatar, name, a line about them.
    W::BeginContentBlock({ MiiDX(), top, MiiDW(), 70 }, 1);
    W::Avatar(MiiDX() + 32, top + 32, 32, m.name, MiiAvatarSeed(m.slot, m.name), MiiFace(m.slot), MiiHairColor(m.slot), MiiPicture(m.slot));
    static const uint32_t H_LV = SaveEditor::Hash("Mii.MiiMisc.SatisfyInfo.Level");
    static const uint32_t H_FEEL = SaveEditor::Hash("Mii.Feeling.Type");
    const int lv = SaveEditor::GetIntAt(gMiiSav, H_LV, miiIdx) + 1;
    const char* feel = FeelingName(SaveEditor::GetEnumAt(gMiiSav, H_FEEL, miiIdx));
    std::string sub = Tx("mii.level.short") + " " + std::to_string(lv);
    if (feel) sub += "  \xC2\xB7  " + Tx((std::string("mii.mood.") + feel).c_str());
    sub += "  \xC2\xB7  " + Tx("mii.slot") + " " + std::to_string(m.slot);
    W::Marquee(Ui::HashStr("mii.head.name"), m.name, MiiDX() + 80, top + 2, MiiDW() - 200, gTheme.text, Font::Title, true);
    W::Marquee(Ui::HashStr("mii.head.sub"), sub, MiiDX() + 80, top + 4 + TextH(Font::Title), MiiDW() - 100, gTheme.dim, Font::Body, true);
    if (gMiiSavDirty) W::Pill(MiiDX() + MiiDW() - W::PillW(Tx("status.unsaved"), true), top + 6, Tx("status.unsaved"), Ui::Tone::Warn, true);
    W::EndBlock();
    // The sub-pages.
    const bool browse = gMiiStatsSubTab == MiiStatsSubTab::Browse;
    std::vector<std::pair<std::string, int>> chips;
    for (int i = 0; i < kMiiTabN; i++) chips.push_back({ Tx(kMiiTabs[i]), -1 });
    const int cy = top + 80;
    W::BeginBlock({ MiiDX(), cy, MiiDW(), 56 });
    W::Chips(Ui::HashStr("mii.tabs"), MiiDX(), cy, chips, browse ? -1 : (int)gMiiStatsSubTab, [](int i) { MiiSetTab(i); }, MiiDW());
    W::EndBlock();
    const int by = cy + 56 + 14;
    const SDL_Rect view{ MiiDX() - 8, by - 4, MiiDW() + 16, SCREEN_H - 62 - (by - 4) };
    sMiiUndoRows.clear();
    W::BeginContentBlock({ MiiDX(), by, MiiDW(), 300 });
    if (gMiiStatsSubTab == MiiStatsSubTab::Stats || gMiiStatsSubTab == MiiStatsSubTab::Words) {
        Focus::BeginScroll(Ui::HashStr("mii.body"), view, sMiiScroll);
        if (gMiiStatsSubTab == MiiStatsSubTab::Stats) MiiProfile(MiiDX(), by - (int)sMiiScroll.offset, MiiDW());
        else MiiWords(MiiDX(), by - (int)sMiiScroll.offset, MiiDW());
        Focus::EndScroll(sMiiScroll.content);
    } else {
        MiiSubPage((int)gMiiStatsSubTab, MiiDX(), by, MiiDW());
    }
    W::EndBlock();
    // X: undo the highlighted Profile row.
    {
        // X: what the highlighted row shows goes back (a birthday is three fields).
        std::vector<int> fis;
        for (const auto& u : sMiiUndoRows)
            if (Focus::Is(u.id) && u.field >= 0 && u.field < 16 && gMiiUndoValid[u.field]) fis.push_back(u.field);
        if (!fis.empty())
            Focus::Command(Input::X, Tx("hint.undo"), [fis]() {
                for (int fi : fis) { gMiiStatsFieldSel = fi; TUndoMii(0); }
                Sound::Play(Sound::Voice::Undo);
            });
    }
    // ZL / ZR: the previous / next sub-page. Y: every Mii at once.
    Focus::Command(Input::ZL, Tx("hint.subpage"), []() { MiiSetTab((int)gMiiStatsSubTab - 1); Sound::Play(Sound::Voice::Select); });
    Focus::Command(Input::ZR, Tx("hint.subpage"), []() { MiiSetTab((int)gMiiStatsSubTab + 1); Sound::Play(Sound::Voice::Select); });
    Focus::Command(Input::Y, Tx("hint.overview"), []() { MiiOpenOverview(); });

    if (sMiiImportAsk) {
        const size_t sl = sMiiImportPath.rfind('/');
        const std::string file = sl == std::string::npos ? sMiiImportPath : sMiiImportPath.substr(sl + 1);
        W::Dialog(Ui::HashStr("mii.importask"), UiIcons::Id::Upload, Ui::Tone::Warn, TokenFill(Tx("mii.import.ask"), "{name}", m.name),
                  file + "\n" + Tx("mii.import.ask.body"),
                  { { Tx("picker.cancel"), W::Kind::Neutral, []() { sMiiImportAsk = false; } },
                    { Tx("mii.import.go"), W::Kind::Primary, []() {
                        sMiiImportAsk = false;
                        BusyFrame(Tx("tex.importing"));
                        DoMiiImport(sMiiImportPath);
                    } } },
                  0, []() { sMiiImportAsk = false; }, Tx("hint.cancel"));
    }
    if (sMiiDelAsk) {
        W::Dialog(Ui::HashStr("mii.delask"), UiIcons::Id::Trash, Ui::Tone::Bad, TokenFill(Tx("mii.delete.ask"), "{name}", m.name),
                  Tx("mii.delete.ask.body"),
                  { { Tx("picker.cancel"), W::Kind::Neutral, []() { sMiiDelAsk = false; TCancelMiiDelete(0); } },
                    { Tx("mii.delete"), W::Kind::Danger, []() {
                        sMiiDelAsk = false;
                        gMiiDeleteConfirmOpen = true;
                        TConfirmMiiDelete(0);
                        if (gMiiStatsMsgCol.g > 160 && gMiiStatsMsgCol.r < 140) {
                            Ui::ToastShow(gMiiStatsMsg, Ui::Tone::Good);
                            gMiiStatsMsg.clear();
                            Sound::Play(Sound::Voice::Delete);
                        }
                        W::Arrive(false);
                    } } },
                  0, []() { sMiiDelAsk = false; TCancelMiiDelete(0); }, Tx("hint.cancel"));
    }
    if (sFileSheet) FileSheet();
    if (sMiiOverview) MiiOverviewSheet();
    sMiiLastFocus = Focus::Current();
}
