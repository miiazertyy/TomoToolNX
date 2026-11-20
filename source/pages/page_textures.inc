// page_textures.inc — TomoToolNX 2.0: the Textures place. The island's
// pictures (food, clothes, goods, objects...) as a grid of thumbnails with
// kind chips and a search; a texture opens a page with its big picture and
// what can be done with it. The work itself is 1.x's (DoOnSwitchImport,
// DoUgcImportLtd, the exports, TConfirmBgRemove, TConfirmUgcDelete), so the
// files written do not change. Included by shell.inc inside namespace S2.

// ── Messages from the 1.x actions, as toasts ─────────────────────────────────
static void ToastFromMsg(std::string& msg, SDL_Color col) {
    if (msg.empty()) return;
    const bool bad = col.r > 180 && col.g < 140;
    const bool good = col.g > 160 && col.r < 140;
    Ui::ToastShow(msg, bad ? Ui::Tone::Bad : good ? Ui::Tone::Good : Ui::Tone::Neutral, bad ? 5.f : 3.5f);
    Sound::Play(bad ? Sound::Voice::Err : good ? Sound::Voice::Done : Sound::Voice::Info);
    msg.clear();
}

// ── Picking a file (pictures, .ltd files) ────────────────────────────────────
static bool sFileSheet = false, sUf2Help = false;
static bool sFileForMii = false;
static std::string sFileTitle;
static std::function<void(const std::string&)> sFilePick;

static void OpenFileSheet(bool forMii, const std::string& start, const std::string& title, std::function<void(const std::string&)> pick) {
    gBrowseForMii = forMii;
    gBrowseForExportDir = false;
    sFileForMii = forMii;
    std::string sp = start.empty() ? std::string("/") : start;
    struct stat st{};
    if (stat(sp.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) sp = "/";
    BrowseRefresh(sp);
    sFileTitle = title;
    sFilePick = std::move(pick);
    sFileSheet = true;
    Motion::Forget(Ui::HashStr("files.sheet"));
}

static void FileSheet() {
    std::vector<W::MenuItem> items;
    for (const auto& e : gBrowseEntries) {
        const bool up = e.name == "[..]";
        const std::string name = e.name;
        if (e.isDir) {
            items.push_back({ up ? std::string("..") : name, "", up ? UiIcons::Id::ChevronLeft : UiIcons::Id::Folder, false, [name, up]() {
                std::string np;
                if (up) {
                    const size_t p = gBrowseCurPath.rfind('/');
                    np = (p == 0 || p == std::string::npos) ? "/" : gBrowseCurPath.substr(0, p);
                } else {
                    np = (gBrowseCurPath == "/" ? "" : gBrowseCurPath) + "/" + name;
                }
                BrowseRefresh(np);
                Focus::Set(Ui::IdMix(Ui::HashStr("files.sheet"), 0u));
            }, false });
        } else {
            const bool uf2 = IsUf2Name(name);
            items.push_back({ name, uf2 ? Tx("browse.uf2.tag") : std::string(), uf2 ? UiIcons::Id::Alert : UiIcons::Id::Image, false,
                              [name, uf2]() {
                                  if (uf2) { sUf2Help = true; Motion::Forget(Ui::HashStr("files.uf2")); return; }
                                  const std::string full = (gBrowseCurPath == "/" ? "" : gBrowseCurPath) + "/" + name;
                                  if (sFileForMii) gBrowseLtdPath = gBrowseCurPath; else gBrowsePngPath = gBrowseCurPath;
                                  sFileSheet = false;
                                  auto pick = sFilePick;
                                  if (pick) pick(full);
                              }, false });
        }
    }
    if (items.empty()) items.push_back({ Tx("browse.empty"), "", UiIcons::Id::Count, false, nullptr, false });
    W::Menu(Ui::HashStr("files.sheet"), sFileTitle + "  \xC2\xB7  " + gBrowseCurPath, items, []() { sFileSheet = false; }, Tx("hint.cancel"));
    if (sUf2Help) {
        W::Dialog(Ui::HashStr("files.uf2"), UiIcons::Id::Alert, Ui::Tone::Warn, Tx("browse.uf2.title"),
                  Tx("browse.uf2.l1") + "\n" + Tx("browse.uf2.l2"),
                  { { Tx("browse.uf2.ok"), W::Kind::Primary, []() { sUf2Help = false; } } }, 0, []() { sUf2Help = false; }, Tx("hint.close"));
    }
}

// ── Thumbnails ───────────────────────────────────────────────────────────────
// Decoded a couple per frame for the tiles on screen, shrunk to 128 px, kept
// for the most recently seen 160 textures, dropped when the list is rescanned.
struct TexThumb { SDL_Texture* tex = nullptr; int w = 0, h = 0; uint32_t used = 0; };
static std::unordered_map<std::string, TexThumb> sThumbs;
static uint32_t sThumbClock = 0, sThumbGen = 0xffffffffu;
static int sThumbBudget = 0;

static void ThumbsDrop() {
    for (auto& kv : sThumbs) if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    sThumbs.clear();
}

static void Shrink(RgbaImage& img, int maxSide) {
    while (img.width > maxSide || img.height > maxSide) {
        const int nw = std::max(1, img.width / 2), nh = std::max(1, img.height / 2);
        std::vector<uint8_t> out((size_t)nw * nh * 4);
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++)
                for (int c = 0; c < 4; c++) {
                    const int x0 = std::min(img.width - 1, x * 2), x1 = std::min(img.width - 1, x * 2 + 1);
                    const int y0 = std::min(img.height - 1, y * 2), y1 = std::min(img.height - 1, y * 2 + 1);
                    const int s = img.pixels[((size_t)y0 * img.width + x0) * 4 + c] + img.pixels[((size_t)y0 * img.width + x1) * 4 + c] +
                                  img.pixels[((size_t)y1 * img.width + x0) * 4 + c] + img.pixels[((size_t)y1 * img.width + x1) * 4 + c];
                    out[((size_t)y * nw + x) * 4 + c] = (uint8_t)((s + 2) / 4);
                }
        img.width = nw; img.height = nh; img.pixels.swap(out);
    }
}

static const TexThumb* ThumbFor(const UgcTextureEntry& e) {
    if (sThumbGen != gUgcGen) { ThumbsDrop(); sThumbGen = gUgcGen; }
    auto it = sThumbs.find(e.stem);
    if (it != sThumbs.end()) { it->second.used = sThumbClock; return it->second.tex ? &it->second : nullptr; }
    if (sThumbBudget <= 0) return nullptr;
    sThumbBudget--;
    TexThumb t;
    t.used = sThumbClock;
    RgbaImage img;
    std::string err = e.hasThumb() ? TextureProcessor::DecodeFile(e.thumbPath, img, false) : std::string("no thumb");
    if (!err.empty() || img.width <= 0) { img = RgbaImage(); err = TextureProcessor::DecodeFile(e.ugctexPath, img, false); }
    if (err.empty() && img.width > 0) {
        Shrink(img, 128);
        if (SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(img.pixels.data(), img.width, img.height, 32, img.width * 4,
                                                              SDL_PIXELFORMAT_RGBA32)) {
            t.tex = SDL_CreateTextureFromSurface(gRen, s);
            SDL_FreeSurface(s);
            t.w = img.width; t.h = img.height;
        }
    }
    if (sThumbs.size() >= 160) {
        auto old = sThumbs.begin();
        for (auto i = sThumbs.begin(); i != sThumbs.end(); ++i) if (i->second.used < old->second.used) old = i;
        if (old->second.tex) SDL_DestroyTexture(old->second.tex);
        sThumbs.erase(old);
    }
    TexThumb& slot = sThumbs[e.stem];
    slot = t;
    return slot.tex ? &slot : nullptr;
}

// Transparent parts show a quiet checkerboard.
static SDL_Texture* Checker() {
    static SDL_Texture* tex = nullptr;
    if (tex) return tex;
    const int n = 64;
    std::vector<uint32_t> px((size_t)n * n);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) px[(size_t)y * n + x] = (((x / 8) + (y / 8)) & 1) ? 0xff3a3a44u : 0xff2c2c34u;
    tex = SDL_CreateTexture(gRen, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, n, n);
    if (tex) SDL_UpdateTexture(tex, nullptr, px.data(), n * 4);
    return tex;
}

static void DrawPicture(SDL_Texture* tex, int tw, int th, SDL_Rect box, int radius, SDL_Color around) {
    if (SDL_Texture* ch = Checker()) {
        // Tile the checkerboard over the box.
        for (int y = 0; y < box.h; y += 64)
            for (int x = 0; x < box.w; x += 64) {
                const int w = std::min(64, box.w - x), h = std::min(64, box.h - y);
                const SDL_Rect src{ 0, 0, w, h };
                DrawTexture(ch, &src, SDL_Rect{ box.x + x, box.y + y, w, h });
            }
    }
    if (tex && tw > 0 && th > 0) {
        const float s = std::min((float)box.w / tw, (float)box.h / th);
        const int w = (int)(tw * s), h = (int)(th * s);
        DrawTexture(tex, nullptr, SDL_Rect{ box.x + (box.w - w) / 2, box.y + (box.h - h) / 2, w, h });
    }
    MaskCorners(box.x, box.y, box.w, box.h, radius, around);
}

// ── The page ─────────────────────────────────────────────────────────────────
static bool sTexDetail = false;
static bool sTexNewMenu = false, sTexDelAsk = false, sTexBgAsk = false;
static Focus::Scroll sTexScroll;

static std::string TexKindLabel(int kind) { return kind >= 0 && kind < gUgcKindCount ? Tx(gUgcKinds[kind].labelKey) : std::string(); }
static std::string TexName(const UgcTextureEntry& e) {
    const std::string n = MiiManager::GetUgcName(e.stem);
    return n.empty() ? FormatStem(e.stem) : n;
}
static Ui::Id TexTileId(const std::string& stem) { return Ui::IdMix(Ui::HashStr("tex.tile"), Ui::HashStr(stem.c_str())); }

static void OpenTexture(int idx) {
    if (idx < 0 || idx >= (int)gEntries.size()) return;
    gEntrySel = idx;
    gUgcBlankRowSel = false;
    LoadPreview(gEntries[idx]);
    sTexDetail = true;
    W::Arrive();
    Focus::Set(Ui::HashStr("tex.d.rename"));
}
static void CloseTexture() {
    sTexDetail = false;
    W::Arrive(false);
    if (gEntrySel >= 0 && gEntrySel < (int)gEntries.size()) Focus::Set(TexTileId(gEntries[gEntrySel].stem));
}

// The game's picture for a kind of texture (the atelier's unlock art).
static Art::Id TexKindArt(int kind) {
    switch (kind) {
        case 0:  return Art::Id::KindFood;
        case 1:  return Art::Id::KindClothes;
        case 2:  return Art::Id::KindGoods;
        case 3:  return Art::Id::KindInterior;
        case 4:  return Art::Id::KindExterior;
        case 5:  return Art::Id::KindObj;
        case 6:  return Art::Id::KindFloor;
        case 7:  return Art::Id::ArtAtelier;
        default: return Art::Id::Count;
    }
}

static void TexNewTile(SDL_Rect r, bool landing, bool visible) {
    const Ui::Id id = Ui::HashStr("tex.new");
    Focus::Node n;
    n.id = id; n.r = { r.x - 5, r.y - 5, r.w + 10, r.h + 10 }; n.radius = 23; n.hint = Tx("hint.open");
    n.flags = (uint16_t)(Focus::Ring | (landing ? Focus::Default : 0));
    n.press = []() { sTexNewMenu = true; Motion::Forget(Ui::HashStr("tex.newmenu")); };
    Focus::Add(std::move(n));
    if (!visible) return;
    W::BeginPress(id, r);
    Focus::DrawPlate(id);
    const bool f = Focus::Is(id);
    FillRectRounded(r.x, r.y, r.w, r.h, 18, Alpha(gTheme.panel, f ? 200 : 110));
    // Dashes all the way round, corners included.
    DashRectRounded((float)r.x + 1, (float)r.y + 1, (float)r.w - 2, (float)r.h - 2, 17.f, 2.f, 8.f, 8.f,
                    f ? Alpha(gTheme.gold, 170) : Alpha(gTheme.text, 60));
    FillDisc((float)(r.x + r.w / 2), (float)(r.y + r.w / 2 - 6), 30.f, Alpha(gTheme.gold, f ? 70 : 30));
    UiIcons::Draw(UiIcons::Id::Plus, r.x + r.w / 2, r.y + r.w / 2 - 6, 30, f ? gTheme.gold : gTheme.dim);
    W::Marquee(id, Tx("tex.new"), r.x + r.w / 2, r.y + r.h - 26, r.w - 16, f ? gTheme.text : gTheme.dim, Font::Body, f, W::Anchor::Centre);
    W::EndPress();
}

static void TexTile(SDL_Rect r, int idx, bool visible, bool landing) {
    const UgcTextureEntry& e = gEntries[idx];
    const Ui::Id id = TexTileId(e.stem);
    Focus::Node n;
    n.id = id; n.r = { r.x - 5, r.y - 5, r.w + 10, r.h + 10 }; n.radius = 23; n.hint = Tx("hint.open");
    n.press = [idx]() { OpenTexture(idx); };
    n.flags = (uint16_t)(Focus::Ring | (landing ? Focus::Default : 0));   // Default: where the highlight lands here
    Focus::Add(std::move(n));
    if (!visible) return;
    W::BeginPress(id, r);
    Focus::DrawPlate(id);
    const bool f = Focus::Is(id);
    const SDL_Color face = Mix(gTheme.panel, gTheme.text, f ? 0.07f : 0.025f);
    FillRectRounded(r.x, r.y, r.w, r.h, 18, face);
    DrawRectRounded(r.x, r.y, r.w, r.h, 18, f ? Alpha(gTheme.gold, 150) : Alpha(gTheme.border, 200));
    const SDL_Rect box{ r.x + 8, r.y + 8, r.w - 16, r.w - 16 };
    const TexThumb* t = ThumbFor(e);
    DrawPicture(t ? t->tex : nullptr, t ? t->w : 0, t ? t->h : 0, box, 12, face);
    if (!t) W::Spinner(box.x + box.w / 2, box.y + box.h / 2, 12, Alpha(gTheme.text, 90));
    W::Marquee(id, TexName(e), r.x + r.w / 2, r.y + r.w + 2, r.w - 16, f ? gTheme.text : Mix(gTheme.text, gTheme.dim, 0.3f), Font::Body, f, W::Anchor::Centre);
    W::Marquee(Ui::IdMix(id, 0xDE5Cu), TexKindLabel(UgcKindOfStem(e.stem)), r.x + r.w / 2, r.y + r.w + 22, r.w - 16, gTheme.faint, Font::Xs, f, W::Anchor::Centre);
    W::EndPress();
}

// ── Every texture at once ────────────────────────────────────────────────────
static bool sTexOverview = false;
static void TexOpenOverview() {
    if (gFilteredEntries.empty()) return;
    sTexOverview = true;
    Motion::Forget(Ui::HashStr("ov.sheet"));
}
static void TexOverviewSheet() {
    Overview o;
    o.title = gUgcKindFilter >= 0 ? TexKindLabel(gUgcKindFilter) : Tx("tab.textures");
    o.sub = CountText((int)gFilteredEntries.size(), "bk.tex1", "bk.texs");
    o.count = (int)gFilteredEntries.size();
    for (int i = 0; i < o.count; i++) if (gFilteredEntries[i] == gEntrySel) o.current = i;
    o.cols = 9;
    o.below = 26;
    sThumbBudget = std::max(sThumbBudget, 3);
    sThumbClock++;
    o.draw = [](int i, SDL_Rect r, bool f, SDL_Color face) {
        if (i < 0 || i >= (int)gFilteredEntries.size()) return;
        const int idx = gFilteredEntries[i];
        if (idx < 0 || idx >= (int)gEntries.size()) return;
        const UgcTextureEntry& e = gEntries[idx];
        const SDL_Rect box{ r.x + 7, r.y + 7, r.w - 14, r.w - 14 };
        const TexThumb* t = ThumbFor(e);
        DrawPicture(t ? t->tex : nullptr, t ? t->w : 0, t ? t->h : 0, box, 10, face);
        if (!t) W::Spinner(box.x + box.w / 2, box.y + box.h / 2, 10, Alpha(gTheme.text, 90));
        W::Marquee(Ui::IdMix(Ui::HashStr("tex.ov.cell"), (uint32_t)i), TexName(e), r.x + r.w / 2, r.y + r.w + 4, r.w - 12, f ? gTheme.text : Mix(gTheme.text, gTheme.dim, 0.3f), Font::Xs, f, W::Anchor::Centre);
    };
    o.pick = [](int i) {
        if (i < 0 || i >= (int)gFilteredEntries.size()) return;
        const int idx = gFilteredEntries[i];
        if (idx >= 0 && idx < (int)gEntries.size()) Focus::Set(TexTileId(gEntries[idx].stem));
        OpenTexture(idx);
    };
    o.close = []() { sTexOverview = false; };
    OverviewSheet(o);
}

static void TexGrid(int top) {
    // Kinds with something in them, with counts; then a search.
    int counts[16] = {};
    for (const auto& e : gEntries) { const int k = UgcKindOfStem(e.stem); if (k >= 0 && k < 16) counts[k]++; }
    std::vector<std::pair<std::string, int>> chips;
    std::vector<int> chipKind;
    std::vector<Art::Id> chipArt;
    chips.push_back({ Tx("ugc.kind.all"), (int)gEntries.size() }); chipKind.push_back(-1); chipArt.push_back(Art::Id::IcoTextures);
    for (int k = 0; k < gUgcKindCount; k++) if (counts[k] > 0 || gUgcKindFilter == k) {
        chips.push_back({ Tx(gUgcKinds[k].labelKey), counts[k] });
        chipKind.push_back(k);
        chipArt.push_back(TexKindArt(k));
    }
    int sel = 0;
    for (int i = 0; i < (int)chipKind.size(); i++) if (chipKind[i] == gUgcKindFilter) sel = i;
    const int searchW = gUgcFilter.empty() ? 56 : 220;
    const int gridW = 56;
    W::BeginBlock({ sEdX, top, sEdW, 56 });
    W::Chips(Ui::HashStr("tex.chips"), sEdX, top, chips, sel, [chipKind](int i) {
        gUgcKindFilter = chipKind[i];
        RebuildUgcFilter();
        sTexScroll.target = 0;
    }, sEdW - searchW - 12 - gridW - 10, chipArt);
    // Every texture at once (Y does the same).
    W::Button(Ui::HashStr("tex.overview"), { sEdX + sEdW - searchW - 10 - gridW, top + 2, gridW, 52 }, std::string(), W::Kind::Neutral,
              []() { TexOpenOverview(); }, UiIcons::Id::Grid, !gFilteredEntries.empty());
    if (gUgcFilter.empty()) {
        W::Button(Ui::HashStr("tex.search"), { sEdX + sEdW - 56, top + 2, 56, 52 }, std::string(), W::Kind::Neutral, []() {
            std::string q = ShowKeyboard(Tx("tex.search.guide"), gUgcFilter, 30);
            for (auto& c : q) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            gUgcFilter = q;
            RebuildUgcFilter();
            sTexScroll.target = 0;
        }, UiIcons::Id::Magnifier);
    } else {
        W::Button(Ui::HashStr("tex.search"), { sEdX + sEdW - searchW, top + 2, searchW, 52 }, "\xE2\x80\x9C" + gUgcFilter + "\xE2\x80\x9D",
                  W::Kind::Soft, []() { gUgcFilter.clear(); RebuildUgcFilter(); Sound::Play(Sound::Voice::Undo); }, UiIcons::Id::Close);
    }
    W::EndBlock();

    const int gy = top + 56 + 18;
    const SDL_Rect view{ sEdX - 8, gy - 6, sEdW + 16, SCREEN_H - 62 - (gy - 6) };
    const int cols = 6, gap = 17;
    const int tw = (sEdW - (cols - 1) * gap) / cols, th = tw + 40;
    Focus::BeginScroll(Ui::HashStr("tex.grid"), view, sTexScroll);
    sThumbBudget = 2;
    sThumbClock++;
    const int n = (int)gFilteredEntries.size() + 1;   // "+ New" first
    for (int i = 0; i < n; i++) {
        const int row = i / cols, col = i % cols;
        const SDL_Rect r{ sEdX + col * (tw + gap), gy + row * (th + gap) - (int)sTexScroll.offset, tw, th };
        const bool visible = r.y + r.h > view.y - 40 && r.y < view.y + view.h + 40;
        // Rows arrive one after another (only near the top, where the eye is).
        const float delay = std::min(row, 4) * 0.05f;
        const float at = Motion::Reduced() ? 1.f : std::max(0.f, std::min(1.f, (float)((Motion::Now() - W::ArrivedAt() - delay) / 0.4)));
        PushXform(0, 10.f * (1 - Motion::Ease(Motion::Curve::Spring, at)), 1, 0, 0, std::min(1.f, at * 1.6f));
        if (i == 0) TexNewTile(r, n == 1, visible);
        else TexTile(r, gFilteredEntries[i - 1], visible, i == 1);
        PopXform();
    }
    const int rows = (n + cols - 1) / cols;
    Focus::EndScroll((float)(rows * (th + gap) + 10));
    if (gFilteredEntries.empty() && !gEntries.empty()) {
        DrawTextC(Tx("ugc.no.matches"), sEdX + sEdW / 2, gy + th + 60, gTheme.dim, Font::Row);
    }
}

// The game's own picture of a texture's item (its thumbnail), at full size,
// for the detail page.
static SDL_Texture* sDetailThumb = nullptr;
static std::string sDetailThumbStem;
static int sTexView = 0;                // 0: as in the game, 1: the texture itself
static uint32_t sDetailGen = 0xffffffffu;
static SDL_Texture* DetailThumb(const UgcTextureEntry& e) {
    if (sDetailThumbStem == e.stem && sDetailGen == gUgcGen) return sDetailThumb;
    if (sDetailThumb) { SDL_DestroyTexture(sDetailThumb); sDetailThumb = nullptr; }
    sDetailThumbStem = e.stem;
    sDetailGen = gUgcGen;
    RgbaImage img;
    if (e.hasThumb() && TextureProcessor::DecodeFile(e.thumbPath, img, false).empty() && img.width > 0)
        if (SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(img.pixels.data(), img.width, img.height, 32, img.width * 4, SDL_PIXELFORMAT_RGBA32)) {
            sDetailThumb = SDL_CreateTextureFromSurface(gRen, s);
            SDL_FreeSurface(s);
        }
    return sDetailThumb;
}

// ── Import a picture: before and after ──────────────────────────────────────
// Like ltd-save-editor's replace panel: the texture now, what the picture will
// become (PreviewTexture: the steps the import takes, at this slot's size and
// format), buttons to turn and mirror it, and the picture settings (the same
// as Settings → Pictures, and kept). Import writes it.
struct TexImportState {
    bool open = false;
    std::string path, name;
    RgbaImage src;
    uint32_t gen = 0;
    int rot = 0;                       // clockwise quarter turns, then...
    bool flip = false;                 // ...mirrored left to right
    TextureProcessor::UgctexLayout layout{ 512, 512, 128, 128, 16, TextureProcessor::TextureFormat::Bc1 };
};
static TexImportState sImp;
static TexPreviewWorker sImpWork;

static void TexImportOpen(const std::string& path) {
    if (gEntrySel < 0 || gEntrySel >= (int)gEntries.size()) return;
    RgbaImage img;
    const std::string err = TextureProcessor::LoadImageFile(path, img);
    if (!err.empty() || img.width <= 0 || img.height <= 0) { Ui::ToastShow(Tx("tex.imp.bad"), Ui::Tone::Bad); return; }
    sImpWork.Drop();                   // a run may still be reading the old picture
    sImp.src = std::move(img);
    sImp.gen++;
    sImp.rot = 0;
    sImp.flip = false;
    sImp.path = path;
    const size_t sl = path.rfind('/');
    sImp.name = sl == std::string::npos ? path : path.substr(sl + 1);
    sImp.layout = TextureProcessor::LayoutOfFile(gEntries[gEntrySel].ugctexPath);
    sImp.open = true;
    Motion::Forget(Ui::HashStr("tex.imp"));
}
static void TexImportClose() {
    sImp.open = false;
    Motion::Forget(Ui::HashStr("tex.imp"));
    // A run still going keeps reading the picture: it is let go at the next
    // open (Drop waits for the run), so closing never waits.
    if (!sImpWork.Busy()) { sImpWork.Drop(); sImp.src = RgbaImage(); }
}
// On the way out of the app.
static void TexImportStop() { sImpWork.Drop(); sImp.src = RgbaImage(); }
// The turns as ltd-save-editor's buttons apply them, one after another:
// the state stays "turn, then mirror".
static void TexImportTurn(int op) {
    if (op == 0) sImp.rot = (sImp.rot + (sImp.flip ? 1 : 3)) % 4;        // to the left
    else if (op == 1) sImp.rot = (sImp.rot + (sImp.flip ? 3 : 1)) % 4;   // to the right
    else if (op == 2) sImp.flip = !sImp.flip;                            // mirror
    else { sImp.flip = !sImp.flip; sImp.rot = (sImp.rot + 2) % 4; }      // upside down
    Sound::Play(Sound::Voice::Tick, { 0.3f + 0.15f * op });
}
static void TexImportGo() {
    const int rot = sImp.rot;
    const bool flip = sImp.flip;
    const std::string path = sImp.path;
    BusyFrame(Tx("tex.importing"));
    sImpWork.Stop();                   // a preview still being made reads the picture too
    DoOnSwitchImport(path, rot, flip, &sImp.src);
    TexImportClose();
}

static void TexImportSheet() {
    const Ui::Id id = Ui::HashStr("tex.imp");
    const double since = Motion::FirstSeen(id), now = Motion::Now();
    if (now - since < 0.0001) { Sound::Play(Sound::Voice::Open); Focus::Set(Ui::HashStr("tex.imp.go")); }
    const int w = 1140, h = 620, x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2 - 8;
    Focus::PushLayer(Focus::Layer::Sheet, []() { Sound::Play(Sound::Voice::Release); TexImportClose(); }, Tx("hint.cancel"), SDL_Rect{ x, y, w, h });
    DimScreen((int)(130 * std::min(1.f, (float)((now - since) / 0.16))));
    const float e = Motion::Reduced() ? 1.f : Motion::Ease(Motion::Curve::Spring, std::min(1.f, (float)((now - since) / 0.42)));
    PushXform(0, 14.f * (1 - e), 0.98f + 0.02f * e, SCREEN_W / 2.f, SCREEN_H / 2.f, std::min(1.f, e * 1.4f));
    FillRectRounded(x, y + 10, w, h, 26, SDL_Color{ 0, 0, 0, 60 });
    FillRectRounded(x, y, w, h, 26, Mix(gTheme.panel, gTheme.text, 0.03f));
    DrawRectRounded(x, y, w, h, 26, Alpha(gTheme.border, 230));
    W::IconTile(x + 28, y + 24, 52, UiIcons::Id::Upload, gTheme.gold);
    DrawTextVC(Tx("tex.imp.title"), x + 96, y + 40, gTheme.text, Font::Title);
    W::Marquee(Ui::HashStr("imp.name"), sImp.name, x + 96, y + 68, 520, gTheme.faint, Font::Body, true, W::Anchor::Middle);

    // Now → New.
    TextureProcessor::ImportOptions o = PictureOptions();
    o.rotate = sImp.rot;
    o.flipH = sImp.flip;
    const int LW = sImp.layout.Width, LH = sImp.layout.Height;
    SDL_Texture* nt = sImpWork.Update(&sImp.src, TexPreviewWorker::JobOf(o, LW, LH, sImp.layout.Format, sImp.gen));
    const int ps = 268, py = y + 134, px0 = x + 30, px1 = px0 + ps + 64;
    W::Eyebrow(px0, py - 26, Tx("tex.imp.now"), gTheme.faint);
    W::Eyebrow(px1, py - 26, Tx("tex.imp.new"), gTheme.faint);
    {
        int cw = 0, ch = 0;
        if (gPreviewTex) SDL_QueryTexture(gPreviewTex, nullptr, nullptr, &cw, &ch);
        DrawPreviewPicture(gPreviewTex, cw, ch, SDL_Rect{ px0, py, ps, ps }, 1.f);
    }
    UiIcons::Draw(UiIcons::Id::ChevronRight, px0 + ps + 32, py + ps / 2, 30, gTheme.faint);
    DrawPreviewPicture(nt, sImpWork.texW, sImpWork.texH, SDL_Rect{ px1, py, ps, ps }, nt ? sImpWork.FadeIn() : 0.f);
    if (sImpWork.Busy()) {
        FillRectRounded(px1 + ps / 2 - 70, py + ps / 2 - 18, 140, 36, 18, Alpha(gTheme.panel, 220));
        DrawTextC(Tx("settings.preview.working"), px1 + ps / 2, py + ps / 2, gTheme.text, Font::Body);
    }
    const std::string fmt = std::to_string(LW) + " \xC3\x97 " + std::to_string(LH) + "  \xC2\xB7  " +
                            (sImp.layout.Format == TextureProcessor::TextureFormat::Bc3 ? "BC3" : "BC1");
    DrawTextC(fmt, px1 + ps / 2, py + ps + 18, gTheme.faint, Font::Xs);
    DrawTextC(std::to_string(sImp.src.width) + " \xC3\x97 " + std::to_string(sImp.src.height), px0 + ps / 2, py + ps + 18, gTheme.faint, Font::Xs);

    // Turn and mirror, under the new picture.
    {
        struct T { UiIcons::Id icon; const char* tip; };
        static const T kT[4] = { { UiIcons::Id::RotateLeft, "tex.imp.rotl" }, { UiIcons::Id::RotateRight, "tex.imp.rotr" },
                                 { UiIcons::Id::FlipH, "tex.imp.fliph" }, { UiIcons::Id::FlipV, "tex.imp.flipv" } };
        const int bs = 54, gap = 12, bx = px1 + (ps - (4 * bs + 3 * gap)) / 2, by = py + ps + 44;
        Ui::Id focused = 0;
        for (int k = 0; k < 4; k++) {
            const Ui::Id bid = Ui::IdMix(id, 10u + (uint32_t)k);
            W::Button(bid, { bx + k * (bs + gap), by, bs, bs }, std::string(), W::Kind::Neutral, [k]() { TexImportTurn(k); }, kT[k].icon, true,
                      Focus::OwnSound);
            if (Focus::Is(bid)) focused = (Ui::Id)k + 1;
        }
        if (focused) DrawTextC(Tx(kT[focused - 1].tip), px1 + ps / 2, by + bs + 20, gTheme.dim, Font::Xs);
    }

    // The settings, on the right.
    const int rx = px1 + ps + 34, rw = x + w - 28 - rx, rowH = 68;
    const SDL_Rect card{ rx, py - 30, rw, 5 * rowH + 4 + 12 };
    W::Card(card);
    int ry = card.y + 6;
    PictureRows(Ui::HashStr("tex.imp.pic"), [&](int hh) { SDL_Rect r{ rx + 6, ry, rw - 12, hh }; ry += hh; return r; },
                [&]() { W::Hairline(rx + 24, ry, rw - 48); ry += 1; }, rowH);

    // Cancel / Import.
    const int bw = 200, bh = 56, by = y + h - 28 - bh;
    W::Button(Ui::HashStr("tex.imp.cancel"), { x + w - 28 - 2 * bw - 14, by, bw, bh }, Tx("picker.cancel"), W::Kind::Neutral,
              []() { Sound::Play(Sound::Voice::Release); TexImportClose(); }, UiIcons::Id::Close, true, Focus::OwnSound);
    W::Button(Ui::HashStr("tex.imp.go"), { x + w - 28 - bw, by, bw, bh }, Tx("tex.imp.go"), W::Kind::Primary, []() { TexImportGo(); },
              UiIcons::Id::Check);
    // L / R turn it, Y mirrors it, from anywhere in the sheet.
    Focus::Command(Input::L, Tx("tex.imp.rotl"), []() { TexImportTurn(0); });
    Focus::Command(Input::R, Tx("tex.imp.rotr"), []() { TexImportTurn(1); });
    Focus::Command(Input::Y, Tx("tex.imp.fliph"), []() { TexImportTurn(2); });
    PopXform();
    Focus::PopLayer();
    if (sColorSheet) ColorSheet();     // a colour of your own for the background
}

// ── Designs as the game stores them ─────────────────────────────────────────
// A design shared as the slot's own files (zipped, or the loose .ugctex.zs,
// .canvas.zs and _Thumb.ugctex.zs) goes straight into the open slot when it
// is for the same kind of item and size.

static void TexAfterFilesChanged(const std::string& msg) {
    gUgcDirty = true;
    { gEntries = UgcScanner::Scan(SAVE_UGC_PATH); gUgcGen++; }
    MiiManager::LoadUgcNames();
    RebuildUgcFilter();
    if (gEntrySel >= (int)gEntries.size()) gEntrySel = std::max(0, (int)gEntries.size() - 1);
    if (!gEntries.empty()) LoadPreview(gEntries[gEntrySel]);
    Ui::ToastShow(msg, Ui::Tone::Good);
    Sound::Play(Sound::Voice::Done);
}

static bool TexReadFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    bool ok = n > 0 && n < 64L * 1024 * 1024;
    if (ok) { out.resize((size_t)n); ok = fread(out.data(), 1, out.size(), f) == out.size(); }
    fclose(f);
    return ok;
}

static void TexImportDesignFile(const std::string& path) {
    if (gEntrySel < 0 || gEntrySel >= (int)gEntries.size()) return;
    const UgcTextureEntry e = gEntries[gEntrySel];
    std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
    std::string err;
    std::string lower = path;
    for (auto& c : lower) c = (char)tolower((unsigned char)c);
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".zip") == 0) {
        std::vector<uint8_t> zip;
        err = TexReadFile(path, zip) ? TextureProcessor::ReadZip(zip, files) : std::string("Cannot read the file");
    } else {
        // A loose file: it and the slot's other files beside it.
        const size_t sl = path.rfind('/');
        const std::string dir = sl == std::string::npos ? std::string(".") : path.substr(0, sl), base = path.substr(sl + 1);
        std::string stem = base;
        for (const char* suf : { "_Thumb.ugctex.zs", ".ugctex.zs", ".canvas.zs" }) {
            const size_t m = strlen(suf);
            if (stem.size() > m && stem.compare(stem.size() - m, m, suf) == 0) { stem = stem.substr(0, stem.size() - m); break; }
        }
        for (const char* suf : { ".ugctex.zs", ".canvas.zs", "_Thumb.ugctex.zs" }) {
            std::vector<uint8_t> b;
            if (TexReadFile(dir + "/" + stem + suf, b)) files.push_back({ stem + suf, std::move(b) });
        }
    }
    TextureProcessor::DesignFiles d;
    if (err.empty() && !TextureProcessor::FindDesign(files, d)) err = Tx("tex.design.none");
    if (err.empty()) {
        const int a = UgcKindOfStem(d.stem), b = UgcKindOfStem(e.stem);
        if (a >= 0 && b >= 0 && a != b)
            err = TokenFill(TokenFill(Tx("tex.design.kind"), "{a}", TexKindLabel(a)), "{b}", TexKindLabel(b));
    }
    if (err.empty())
        err = TextureProcessor::ImportDesign(d, e.directory() + "/" + e.stem, e.ugctexPath, e.hasThumb() ? e.thumbPath : std::string(), e.hasCanvas());
    if (!err.empty()) { Ui::ToastShow(err, Ui::Tone::Bad); Sound::Play(Sound::Voice::Err); return; }
    LogOK("Design imported into " + e.stem + " from " + path);
    TexAfterFilesChanged(Tx("tex.design.done"));
}

// ── Find a picture online (Textures > Replace) ──────────────────────────────
// Openverse (api.openverse.org: openly licensed pictures, no account needed):
// a search, a page of results as thumbnails, and A on one downloads it into
// the import sheet (turn, fit, before and after, import). The creator and the
// licence show under the highlighted picture. The downloads run on a thread.
struct FindHit { std::string title, creator, license, version, url, thumb; SDL_Texture* tex = nullptr; int tw = 0, th = 0; bool asked = false, failed = false; };
struct FindJob { int kind = 0; std::string url, who; int index = -1; uint32_t gen = 0; std::vector<uint8_t> body; std::string err; long status = 0;
                 std::string query; int page = 1; std::vector<ImageSearch::Hit> hits; bool more = false; };
static struct {
    bool open = false;
    std::string query;
    int page = 1, pages = 0;
    bool searching = false, downloading = false;
    uint32_t gen = 0;
    std::vector<FindHit> hits;
    std::string status;
} sFind;
static Thread sFindThread{};
static bool sFindRunning = false, sFindStop = false;
static Mutex sFindMutex;
static CondVar sFindCond;
static std::vector<FindJob> sFindQueue, sFindDone;
static void FindWorker(void*) {
    while (true) {
        FindJob job;
        mutexLock(&sFindMutex);
        while (sFindQueue.empty() && !sFindStop) condvarWait(&sFindCond, &sFindMutex);
        if (sFindStop) { mutexUnlock(&sFindMutex); break; }
        job = std::move(sFindQueue.front());
        sFindQueue.erase(sFindQueue.begin());
        mutexUnlock(&sFindMutex);
        if (job.kind == 0) job.err = ImageSearch::Search(job.query, job.page, job.hits, job.more);   // Commons, then Openverse
        else job.status = ShareClient::Fetch(job.url, job.body, job.err);
        mutexLock(&sFindMutex);
        sFindDone.push_back(std::move(job));
        mutexUnlock(&sFindMutex);
    }
}
static void FindSubmit(int kind, const std::string& url, int index) {
    if (!sFindRunning) {
        mutexInit(&sFindMutex);
        condvarInit(&sFindCond);
        sFindStop = false;
        if (R_SUCCEEDED(threadCreate(&sFindThread, FindWorker, nullptr, nullptr, 128 * 1024, 0x2C, -2))) {
            if (R_SUCCEEDED(threadStart(&sFindThread))) sFindRunning = true;
            else threadClose(&sFindThread);
        }
        if (!sFindRunning) { sFind.status = Tx("find.offline"); sFind.searching = sFind.downloading = false; return; }
    }
    FindJob j;
    j.kind = kind; j.url = url; j.index = index; j.gen = sFind.gen;
    j.query = sFind.query; j.page = sFind.page;
    mutexLock(&sFindMutex);
    sFindQueue.push_back(std::move(j));
    condvarWakeOne(&sFindCond);
    mutexUnlock(&sFindMutex);
}
// On the way out of the app.
static void FindStop() {
    if (!sFindRunning) return;
    mutexLock(&sFindMutex);
    sFindStop = true;
    sFindQueue.clear();
    condvarWakeAll(&sFindCond);
    mutexUnlock(&sFindMutex);
    threadWaitForExit(&sFindThread);
    threadClose(&sFindThread);
    sFindRunning = false;
}
static void FindDropHits() {
    for (auto& h : sFind.hits) if (h.tex) SDL_DestroyTexture(h.tex);
    sFind.hits.clear();
}
static void FindSearch(int page) {
    if (sFind.query.empty()) return;
    sFind.gen++;
    {   // what is still waiting belongs to the old search
        mutexLock(&sFindMutex);
        sFindQueue.clear();
        mutexUnlock(&sFindMutex);
    }
    FindDropHits();
    sFind.page = std::max(1, page);
    sFind.searching = true;
    sFind.status.clear();
    FindSubmit(0, std::string(), -1);
}
static void FindAsk() {
    const std::string q = ShowKeyboard(Tx("find.prompt"), sFind.query, 60);
    if (q.empty()) return;
    sFind.query = q;
    FindSearch(1);
    Focus::Set(Ui::IdMix(Ui::HashStr("tex.find"), 100u));
}
static void FindOpen() {
    sFind.open = true;
    Motion::Forget(Ui::HashStr("tex.find"));
}
static void FindClose() {
    sFind.open = false;
    sFind.gen++;
    mutexLock(&sFindMutex);
    sFindQueue.clear();
    mutexUnlock(&sFindMutex);
    FindDropHits();
    sFind.searching = sFind.downloading = false;
}
// Finished downloads: results, thumbnails, the picture itself.
static void FindPump() {
    std::vector<FindJob> done;
    mutexLock(&sFindMutex);
    done.swap(sFindDone);
    mutexUnlock(&sFindMutex);
    for (auto& j : done) {
        if (j.gen != sFind.gen) continue;
        if (j.kind == 0) {
            sFind.searching = false;
            if (!j.err.empty()) { sFind.status = Tx(j.err.c_str()); continue; }
            sFind.pages = j.more ? j.page + 1 : j.page;
            for (const ImageSearch::Hit& r : j.hits) {
                FindHit h;
                h.title = r.title; h.creator = r.creator; h.license = r.license; h.url = r.url; h.thumb = r.thumb;
                sFind.hits.push_back(std::move(h));
            }
            if (sFind.hits.empty()) sFind.status = Tx("find.none");
            for (size_t i = 0; i < sFind.hits.size(); i++)
                if (!sFind.hits[i].thumb.empty()) { sFind.hits[i].asked = true; FindSubmit(1, sFind.hits[i].thumb, (int)i); }
        } else if (j.kind == 1) {
            if (j.index < 0 || j.index >= (int)sFind.hits.size()) continue;
            FindHit& h = sFind.hits[(size_t)j.index];
            SDL_Surface* surf = j.status == 200 && !j.body.empty() ? IMG_Load_RW(SDL_RWFromConstMem(j.body.data(), (int)j.body.size()), 1) : nullptr;
            if (!surf) { h.failed = true; continue; }
            h.tex = SDL_CreateTextureFromSurface(gRen, surf);
            h.tw = surf->w; h.th = surf->h;
            if (h.tex) SDL_SetTextureScaleMode(h.tex, SDL_ScaleModeLinear);
            SDL_FreeSurface(surf);
        } else if (j.kind == 2) {
            sFind.downloading = false;
            const auto& b = j.body;
            const bool png = b.size() > 8 && b[0] == 0x89 && b[1] == 'P', jpg = b.size() > 3 && b[0] == 0xFF && b[1] == 0xD8;
            const bool webp = b.size() > 12 && !memcmp(b.data(), "RIFF", 4) && !memcmp(b.data() + 8, "WEBP", 4);
            if (j.status != 200 || !(png || jpg || webp)) { Ui::ToastShow(Tx("find.fail"), Ui::Tone::Bad); Sound::Play(Sound::Voice::Err); continue; }
            mkdir("/switch/TomoToolNX", 0777);
            const std::string path = std::string("/switch/TomoToolNX/.found") + (png ? ".png" : jpg ? ".jpg" : ".webp");
            FILE* f = fopen(path.c_str(), "wb");
            const bool ok = f && fwrite(b.data(), 1, b.size(), f) == b.size();
            if (f) fclose(f);
            if (!ok) { Ui::ToastShow(Tx("find.fail"), Ui::Tone::Bad); continue; }
            FindClose();
            TexImportOpen(path);
            sImp.name = TokenFill(Tx("find.from"), "{who}", j.who.empty() ? std::string("?") : j.who);
        }
    }
}

static void TexFindSheet() {
    FindPump();
    const Ui::Id id = Ui::HashStr("tex.find");
    const double since = Motion::FirstSeen(id), now = Motion::Now();
    if (now - since < 0.0001) {
        Sound::Play(Sound::Voice::Open);
        Focus::Set(Ui::IdMix(id, 1u));
    }
    const int w = 1140, h = 620, x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2 - 8;
    Focus::PushLayer(Focus::Layer::Sheet, []() { Sound::Play(Sound::Voice::Release); FindClose(); }, Tx("hint.close"), SDL_Rect{ x, y, w, h });
    DimScreen((int)(130 * std::min(1.f, (float)((now - since) / 0.16))));
    const float e = Motion::Reduced() ? 1.f : Motion::Ease(Motion::Curve::Spring, std::min(1.f, (float)((now - since) / 0.42)));
    PushXform(0, 14.f * (1 - e), 0.98f + 0.02f * e, SCREEN_W / 2.f, SCREEN_H / 2.f, std::min(1.f, e * 1.4f));
    FillRectRounded(x, y + 10, w, h, 26, SDL_Color{ 0, 0, 0, 60 });
    FillRectRounded(x, y, w, h, 26, Mix(gTheme.panel, gTheme.text, 0.03f));
    DrawRectRounded(x, y, w, h, 26, Alpha(gTheme.border, 230));
    W::IconTile(x + 28, y + 24, 52, UiIcons::Id::Globe, gTheme.gold);
    DrawTextVC(Tx("find.title"), x + 96, y + 40, gTheme.text, Font::Title);
    W::Marquee(Ui::HashStr("find.desc"), Tx("find.desc"), x + 96, y + 68, w - 140, gTheme.faint, Font::Body, true, W::Anchor::Middle);
    // The search: A types it.
    const SDL_Rect sr{ x + 28, y + 100, w - 56 - 230, 56 };
    {
        const Ui::Id sid = Ui::IdMix(id, 1u);
        W::RowLink(sid, sr, { sFind.query.empty() ? Tx("find.prompt") : "\xE2\x80\x9C" + sFind.query + "\xE2\x80\x9D", std::string() }, std::string(),
                   []() { FindAsk(); }, UiIcons::Id::Magnifier);
    }
    W::Button(Ui::IdMix(id, 2u), { x + w - 28 - 214, y + 100, 214, 56 }, Tx("find.search"), W::Kind::Primary, []() {
        if (sFind.query.empty()) FindAsk(); else FindSearch(1); }, UiIcons::Id::Magnifier);
    // The results.
    const int cols = 5, tw = 200, th = 150, gap = 16, gx = x + (w - (cols * tw + (cols - 1) * gap)) / 2, gy = y + 180;
    int focused = -1;
    for (size_t i = 0; i < sFind.hits.size() && i < 10; i++) {
        const int c = (int)i % cols, r = (int)i / cols;
        const SDL_Rect cr{ gx + c * (tw + gap), gy + r * (th + gap), tw, th };
        const Ui::Id tid = Ui::IdMix(id, 100u + (uint32_t)i);
        Focus::Node n;
        n.id = tid; n.r = cr; n.radius = 16; n.flags = Focus::Ring;
        n.hint = Tx("find.pick");
        const int idx = (int)i;
        n.press = [idx]() {
            if (sFind.downloading || idx >= (int)sFind.hits.size()) return;
            sFind.downloading = true;
            FindSubmit(2, sFind.hits[(size_t)idx].url, idx);
            // The creator rides along for the import sheet's subtitle.
            mutexLock(&sFindMutex);
            if (!sFindQueue.empty()) sFindQueue.back().who = sFind.hits[(size_t)idx].creator;
            mutexUnlock(&sFindMutex);
        };
        Focus::Add(std::move(n));
        if (Focus::Is(tid)) focused = (int)i;
        W::BeginPress(tid, cr);
        FillRectRounded(cr.x, cr.y, cr.w, cr.h, 16, Alpha(gTheme.text, 12));
        const FindHit& hh = sFind.hits[i];
        if (hh.tex && hh.tw > 0 && hh.th > 0) {
            // Cover the tile, cropped in the middle.
            const float k = std::max((float)cr.w / hh.tw, (float)cr.h / hh.th);
            const int sw = (int)(cr.w / k), sh = (int)(cr.h / k);
            const SDL_Rect src{ (hh.tw - sw) / 2, (hh.th - sh) / 2, sw, sh };
            DrawTexture(hh.tex, &src, SDL_Rect{ cr.x + 4, cr.y + 4, cr.w - 8, cr.h - 8 });
        } else if (!hh.failed) {
            Ui::Spinner(cr.x + cr.w / 2, cr.y + cr.h / 2, 12, gTheme.faint);
        } else UiIcons::Draw(UiIcons::Id::Image, cr.x + cr.w / 2, cr.y + cr.h / 2, 28, gTheme.faint);
        Focus::DrawPlate(tid);
        W::EndPress();
    }
    // What the highlight is: its name, who made it, its licence.
    const int iy = gy + 2 * (th + gap) + 8;
    if (focused >= 0) {
        const FindHit& hh = sFind.hits[(size_t)focused];
        std::string lic = hh.license;
        for (auto& ch : lic) ch = (char)toupper((unsigned char)ch);
        // (already as it reads: "CC BY-SA 3.0", "Public domain")
        W::Marquee(Ui::IdMix(Ui::HashStr("find.hit.t"), (uint32_t)focused), hh.title.empty() ? Tx("find.untitled") : hh.title, x + 40, iy + 14,
                   w - 80, gTheme.text, Font::Row, true, W::Anchor::Middle);
        W::Marquee(Ui::IdMix(Ui::HashStr("find.hit.c"), (uint32_t)focused),
                   TokenFill(TokenFill(Tx("find.credit"), "{who}", hh.creator.empty() ? "?" : hh.creator), "{lic}", lic), x + 40, iy + 42, w - 80,
                   gTheme.faint, Font::Body, true, W::Anchor::Middle);
    } else if (sFind.searching) {
        Ui::Spinner(x + 56, iy + 26, 12, gTheme.gold);
        DrawTextVC(Tx("find.searching"), x + 80, iy + 26, gTheme.dim, Font::Body);
    } else if (!sFind.status.empty()) {
        DrawTextVC(sFind.status, x + 40, iy + 26, gTheme.dim, Font::Body);
    } else if (sFind.hits.empty()) {
        DrawTextVC(Tx("find.hint"), x + 40, iy + 26, gTheme.faint, Font::Body);
    }
    if (sFind.downloading) {
        FillRectRounded(x + w / 2 - 150, y + h / 2 - 30, 300, 60, 30, Alpha(gTheme.panel, 240));
        Ui::Spinner(x + w / 2 - 110, y + h / 2, 12, gTheme.gold);
        DrawTextVC(Tx("find.downloading"), x + w / 2 - 86, y + h / 2, gTheme.text, Font::Body);
    }
    // Pages: buttons, and L / R.
    if (!sFind.hits.empty()) {
        const int by = y + h - 28 - 50;
        const bool prev = sFind.page > 1, next = sFind.pages == 0 || sFind.page < sFind.pages;
        W::Button(Ui::IdMix(id, 3u), { x + 28, by, 180, 50 }, Tx("find.prev"), W::Kind::Neutral, []() { if (sFind.page > 1) FindSearch(sFind.page - 1); },
                  UiIcons::Id::ChevronLeft, prev);
        W::Button(Ui::IdMix(id, 4u), { x + w - 28 - 180, by, 180, 50 }, Tx("find.next"), W::Kind::Neutral, []() { FindSearch(sFind.page + 1); },
                  UiIcons::Id::ChevronRight, next);
        DrawTextC(TokenFill(Tx("find.page"), "{n}", std::to_string(sFind.page)), x + w / 2, by + 25, gTheme.faint, Font::Body);
        if (prev) Focus::Command(Input::L, Tx("find.prev"), []() { FindSearch(sFind.page - 1); });
        if (next) Focus::Command(Input::R, Tx("find.next"), []() { FindSearch(sFind.page + 1); });
    }
    Focus::Command(Input::Y, Tx("find.search"), []() { FindAsk(); });
    PopXform();
    Focus::PopLayer();
}

// Replace: from the SD card (a picture, a design or an .ltd file).
static void TexPickFromSd() {
    OpenFileSheet(false, gBrowsePngPath, Tx("tex.import"), [](const std::string& path) {
        std::string lower = path;
        for (auto& c : lower) c = (char)tolower((unsigned char)c);
        const bool isLtdx = lower.size() >= 5 && lower.compare(lower.size() - 5, 4, ".ltd") == 0;
        const bool isDesign = (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".zip") == 0) ||
                              (lower.size() > 3 && lower.compare(lower.size() - 3, 3, ".zs") == 0);
        if (isLtdx) { BusyFrame(Tx("tex.importing")); DoUgcImportLtd(path); }
        else if (isDesign) { BusyFrame(Tx("tex.importing")); TexImportDesignFile(path); }
        else TexImportOpen(path);       // see it first, turn it, then import
    });
}
static bool sTexImportMenu = false;

// The type of item a texture is for, as the save keeps it (clothes, goods
// and objects; 0 for the others): it picks the game's model for the 3D view.
static uint32_t TexItemType(int kind, const std::string& stem) {
    if (!gPlayerSav.loaded) return 0;
    const char* f = kind == 1 ? "UGC.Cloth.UgcClothType" : kind == 2 ? "UGC.Goods.UgcGoodsType"
                  : kind == 5 ? "UGC.MapObject.UgcMapObjectType" : nullptr;
    if (!f) return 0;
    int slot = 0;
    for (char c : stem) if (c >= '0' && c <= '9') slot = slot * 10 + (c - '0');
    return SaveEditor::GetAnyEnumAt(gPlayerSav, SaveEditor::Hash(f), slot, 0);
}

static void TexDetail(int top) {
    if (gEntrySel < 0 || gEntrySel >= (int)gEntries.size()) { sTexDetail = false; return; }
    const UgcTextureEntry& e = gEntries[gEntrySel];
    const int kind = UgcKindOfStem(e.stem);
    // The picture: as the game shows the item (its thumbnail, like the grid),
    // the texture as it is stored, or the item itself in 3D with the texture
    // on it (the game's own model, turning).
    const int ps = 372;
    SDL_Texture* thumb = DetailThumb(e);
    const uint32_t itemType = TexItemType(kind, e.stem);
    const bool has3d = itemType && ItemPreview::Has(kind, itemType);
    if (!has3d && sTexView == 2) sTexView = 0;
    if (thumb || has3d) {
        std::vector<std::pair<std::string, int>> views = { { Tx("tex.view.game"), -1 }, { Tx("tex.view.texture"), -1 } };
        if (has3d) views.push_back({ Tx("tex.view.3d"), -1 });
        W::BeginBlock({ sEdX, top, ps, 50 });
        W::Chips(Ui::HashStr("tex.view"), sEdX, top, views, sTexView, [](int i) { sTexView = i; }, ps);
        W::EndBlock();
        top += 60;
    }
    const SDL_Rect pc{ sEdX, top, ps, ps };
    W::BeginBlock(pc);
    {
        const SDL_Color face = Mix(gTheme.panel, gTheme.text, 0.03f);
        FillRectRounded(pc.x, pc.y, pc.w, pc.h, 22, face);
        SDL_Texture* shown = thumb && sTexView == 0 ? thumb : gPreviewTex;
        bool in3d = false;
        if (sTexView == 2 && has3d) {
            const float yaw = Motion::Reduced() ? 0.6f : (float)fmod(Motion::Now() * 0.7, 6.28318531);
            if (SDL_Texture* t3 = ItemPreview::Draw(gRen, e.stem + "#" + std::to_string(gUgcGen), kind, itemType, e.ugctexPath, 400, yaw)) {
                shown = t3;
                in3d = true;
            }
        }
        int tw = 0, th = 0;
        if (shown) SDL_QueryTexture(shown, nullptr, nullptr, &tw, &th);
        DrawPicture(shown, tw, th, SDL_Rect{ pc.x + 12, pc.y + 12, pc.w - 24, pc.h - 24 }, 14, face);
        DrawRectRounded(pc.x, pc.y, pc.w, pc.h, 22, Alpha(gTheme.border, 220));
        if (tw > 0 && !in3d) {
            const std::string dim = std::to_string(tw) + " \xC3\x97 " + std::to_string(th);
            W::Pill(pc.x + 16, pc.y + pc.h - 16 - 28, dim, Ui::Tone::Neutral);
        }
    }
    W::EndBlock();
    // Under the picture: what it is called, and getting rid of it.
    {
        const int by = pc.y + pc.h + 14, bw = (ps - 12) / 2;
        W::BeginBlock({ sEdX, by, ps, 50 });
        W::Button(Ui::HashStr("tex.d.rename"), { sEdX, by, bw, 50 }, Tx("tex.rename"), W::Kind::Neutral, []() {
            gUgcRenameKbdReq = false;
            if (gPreviewStem.empty()) return;
            const std::string cur = MiiManager::GetUgcName(gPreviewStem);
            const std::string nv = ShowKeyboard(Lang::T("ugc.rename.guide"), cur, 30);
            if (nv == cur) return;
            // RenameUgc rewrites Player.sav on disk (1.x's rename, as is).
            std::string err = FlushSavesToDisk();
            if (err.empty()) err = MiiManager::RenameUgc(gPreviewStem, nv);
            ReloadSavesFromDisk(false, true, false);
            if (err.empty()) { gNeedCommit = true; RebuildUgcFilter(); gOnSwitchMsg = Lang::T("ugc.rename.ok"); gOnSwitchMsgCol = COL_GREEN; }
            else { gOnSwitchMsg = Lang::T("ugc.rename.fail") + " " + err; gOnSwitchMsgCol = COL_RED; }
        }, UiIcons::Id::Pencil);
        W::Button(Ui::HashStr("tex.d.del"), { sEdX + bw + 12, by, bw, 50 }, Tx("tex.delete"), W::Kind::Danger, []() {
            TOpenUgcDeleteConfirm(0);
            gUgcDeleteConfirmOpen = false;   // the 2.0 dialog asks instead
            sTexDelAsk = true;
            Motion::Forget(Ui::HashStr("tex.delask"));
        }, UiIcons::Id::Trash);
        W::EndBlock();
    }
    // On the right, what can be done with it, in two groups that say so.
    const int x = sEdX + ps + 22, w = sEdW - ps - 22;
    const bool ltd = kind >= 0 && kind < 7;
    int ry = top;
    auto group = [&](const char* eyebrowKey, int rows) {
        W::Eyebrow(x + 8, ry, Tx(eyebrowKey), gTheme.faint);
        ry += 24;
        const SDL_Rect card{ x, ry, w, rows * W::kRowH + (rows - 1) + 12 };
        W::BeginBlock(card);
        W::Card(card);
        ry += 6;
        return card;
    };
    auto row = [&](bool last) {
        SDL_Rect r{ x + 6, ry, w - 12, W::kRowH };
        ry += W::kRowH;
        if (!last) { W::Hairline(x + 24, ry, w - 48); ry += 1; }
        return r;
    };
    // The picture: another one from the SD card, one found online, or this
    // one without its background.
    {
        const SDL_Rect card = group("tex.d.sec.picture", 3);
        W::RowLink(Ui::HashStr("tex.d.import"), row(false), { Tx("tex.import.sd"), Tx("tex.import.sd.desc") }, std::string(),
                   []() { TexPickFromSd(); }, UiIcons::Id::Upload);
        W::RowLink(Ui::HashStr("tex.d.find"), row(false), { Tx("find.title"), Tx("find.short") }, std::string(),
                   []() { FindOpen(); }, UiIcons::Id::Magnifier);
        W::RowLink(Ui::HashStr("tex.d.bg"), row(true), { Tx("tex.removebg"), Tx("tex.removebg.desc") }, std::string(), []() {
            FILE* mf = fopen(kBgModelPath, "rb");
            if (!mf) { gOnSwitchMsg = "u2netp.bin missing from romfs (corrupt install?)"; gOnSwitchMsgCol = COL_RED; return; }
            fclose(mf);
            sTexBgAsk = true;
            Motion::Forget(Ui::HashStr("tex.bgask"));
        }, UiIcons::Id::Sparkle);
        W::EndBlock();
        ry = card.y + card.h + 18;
    }
    // Keeping and sharing it: a picture (to the export folder, shown here),
    // and an .ltd file for someone else's island.
    {
        const SDL_Rect card = group("tex.d.sec.share", ltd ? 2 : 1);
        W::RowLink(Ui::HashStr("tex.d.png"), row(!ltd), { Tx("tex.export.png"), Tx("tex.export.png.desc") }, gExportPath,
                   []() { TDoUgcExportPic(0); }, UiIcons::Id::Download);
        if (ltd) W::RowLink(Ui::HashStr("tex.d.ltd"), row(true), { Tx("tex.export.ltd"), Tx("tex.export.ltd.desc") }, std::string(),
                            []() { DoUgcExportLtd(); }, UiIcons::Id::Download);
        W::EndBlock();
        (void)card;
    }
}

static void PageTextures() {
    ToastFromMsg(gOnSwitchMsg, gOnSwitchMsgCol);
    const bool detail = sTexDetail && gEntrySel >= 0 && gEntrySel < (int)gEntries.size();
    std::string title = Tx("tab.textures"), desc = Tx("tex.desc"), pill;
    if (detail) {
        const UgcTextureEntry& e = gEntries[gEntrySel];
        title = TexName(e);
        desc = FormatStem(e.stem) + "  \xC2\xB7  " + TexKindLabel(UgcKindOfStem(e.stem));
    } else {
        pill = std::to_string(gFilteredEntries.size());
    }
    W::BeginBlock({ sEdX, 34, sEdW, 90 });
    const int hy = W::PageHeader(sEdX, 34, sEdW, UiIcons::Id::Image, detail ? Tx("tab.textures") : IslandName(), title, desc, pill,
                                 Font::Title, detail ? std::function<void()>([]() { CloseTexture(); }) : nullptr);
    W::EndBlock();
    if (detail) TexDetail(hy); else TexGrid(hy);
    if (detail && !IsSidebarId(Focus::Current())) {
        Focus::SetBaseBack([]() { Sound::Play(Sound::Voice::Release); CloseTexture(); }, Tx("hint.back"));
    }
    if (!detail && !gFilteredEntries.empty()) Focus::Command(Input::Y, Tx("hint.overview"), []() { TexOpenOverview(); });
    if (sTexOverview && !detail) TexOverviewSheet();
    else sTexOverview = false;
    if (sTexNewMenu) {
        std::vector<W::MenuItem> items;
        for (int k = 0; k < 7; k++) {
            const int kk = k;
            items.push_back({ Tx(gUgcKinds[k].labelKey), "", UiIcons::Id::Plus, false, [kk]() {
                sTexNewMenu = false;
                TDoCreateBlankUgc(kk);
                if (gEntrySel >= 0 && gEntrySel < (int)gEntries.size()) Focus::Set(TexTileId(gEntries[gEntrySel].stem));
            }, false, {}, TexKindArt(k) });
        }
        W::Menu(Ui::HashStr("tex.newmenu"), Tx("ugc.kind.create.title"), items, []() { sTexNewMenu = false; }, Tx("hint.close"), 0,
                Focus::RectOf(Ui::HashStr("tex.new")));
    }
    if (sFileSheet) FileSheet();
    if (sImp.open && detail && !sFileSheet) TexImportSheet();
    else if (sImp.open && !detail) TexImportClose();
    if (sTexImportMenu && detail) {
        std::vector<W::MenuItem> items;
        items.push_back({ Tx("tex.import.sd"), Tx("tex.import.sd.desc"), UiIcons::Id::Folder, false, []() { sTexImportMenu = false; TexPickFromSd(); }, false });
        items.push_back({ Tx("find.title"), Tx("find.short"), UiIcons::Id::Globe, false, []() { sTexImportMenu = false; FindOpen(); }, false });
        W::Menu(Ui::HashStr("tex.impmenu"), Tx("tex.import"), items, []() { sTexImportMenu = false; }, Tx("hint.close"), 0,
                Focus::RectOf(Ui::HashStr("tex.d.import")));
    } else sTexImportMenu = false;
    if (sFind.open && detail && !sFileSheet && !sImp.open) TexFindSheet();
    else if (sFind.open && !detail) FindClose();
    if (sTexBgAsk) {
        W::Dialog(Ui::HashStr("tex.bgask"), UiIcons::Id::Sparkle, Ui::Tone::Second, Tx("bg.title"), Tx("bg.body.l1") + "\n" + Tx("bg.body.l2"),
                  { { Tx("picker.cancel"), W::Kind::Neutral, []() { sTexBgAsk = false; } },
                    { Tx("tex.removebg.go"), W::Kind::Primary, []() { sTexBgAsk = false; TConfirmBgRemove(0); } } },
                  1, []() { sTexBgAsk = false; }, Tx("hint.cancel"));
    }
    if (sTexDelAsk) {
        W::Dialog(Ui::HashStr("tex.delask"), UiIcons::Id::Trash, Ui::Tone::Bad, TokenFill(Tx("tex.delete.ask"), "{name}", gUgcDeleteConfirmName),
                  Tx("ugc.delete.warn"),
                  { { Tx("picker.cancel"), W::Kind::Neutral, []() { sTexDelAsk = false; TCancelUgcDelete(0); } },
                    { Tx("tex.delete"), W::Kind::Danger, []() {
                        sTexDelAsk = false;
                        gUgcDeleteConfirmOpen = true;
                        TConfirmUgcDelete(0);
                        sTexDetail = false;
                        W::Arrive(false);
                        if (gOnSwitchMsgCol.g > 160 && gOnSwitchMsgCol.r < 140) {
                            // Done: the delete sound, not the generic one.
                            Ui::ToastShow(gOnSwitchMsg, Ui::Tone::Good);
                            gOnSwitchMsg.clear();
                            Sound::Play(Sound::Voice::Delete);
                        }
                    } } },
                  0, []() { sTexDelAsk = false; TCancelUgcDelete(0); }, Tx("hint.cancel"));
    }
}
