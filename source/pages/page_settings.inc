// page_settings.inc — TomoToolNX 2.0: Settings (included by shell.inc, inside namespace S2).

static std::string SizeText(uint64_t b);   // (page_start.inc)

// ── Settings ─────────────────────────────────────────────────────────────────
enum SetCat { SC_LOOK, SC_SOUND, SC_LANG, SC_PICTURES, SC_FILES, SC_NETWORK, SC_ABOUT, SC_COUNT };
static int    sSetCat = SC_LOOK;
static bool   sThemeMenu = false, sLangMenu = false, sColorSheet = false, sFolderSheet = false;
static int    sColorTarget = 0;    // 0 accent, 1 background, 2 picture matte
static Ui::Id sSetLastFocus = 0;

static const Ui::Id kSetCatGroup = Ui::HashStr("set.cats");
static Ui::Id SetCatId(int i) { return Ui::IdMix(Ui::HashStr("set.cat"), (uint32_t)i); }

// `art`: the game's icon for it on the editor's category chips.
struct CatDef { const char* key; const char* descKey; UiIcons::Id icon; const char* firstRow; Art::Id art; };
static const CatDef kCats[SC_COUNT] = {
    { "settings.cat.look",     "settings.cat.look.desc",     UiIcons::Id::Palette, "set.theme",   Art::Id::IcoLook     },
    { "settings.cat.sound",    "settings.cat.sound.desc",    UiIcons::Id::Speaker, "set.sound",   Art::Id::IcoVoice    },
    { "settings.cat.language", "settings.cat.language.desc", UiIcons::Id::Globe,   "set.lang",    Art::Id::IcoLanguage },
    { "settings.cat.pictures", "settings.cat.pictures.desc", UiIcons::Id::Image,   "set.shape",   Art::Id::IcoPictures },
    { "settings.cat.files",    "settings.cat.files.desc",    UiIcons::Id::Folder,  "set.export",  Art::Id::IcoBackups  },
    { "settings.cat.network",  "settings.cat.network.desc",  UiIcons::Id::Wifi,    "set.webui",   Art::Id::IcoPhone    },
    { "settings.cat.about",    "settings.cat.about.desc",    UiIcons::Id::Info,    "set.version", Art::Id::IcoAbout    },
};

static void SetCategory(int i) {
    if (i == sSetCat) return;
    sSetCat = i;
    W::Arrive();
}

static int ThemeIndex() {
    const auto& av = ThemeNS::Available();
    for (int i = 0; i < (int)av.size(); i++) if (av[i].code == ThemeNS::Current()) return i;
    return 0;
}
static void ThemeSet(const std::string& code) {
    ThemeNS::SetCurrent(code);
    ApplyCustomColors();
    SaveConfig();
}
static int LangIndex() {
    const auto& av = Lang::Available();
    for (int i = 0; i < (int)av.size(); i++) if (av[i].code == Lang::Current()) return i;
    return 0;
}
static void LangSet(const std::string& code) {
    Lang::SetCurrent(code);
    FontReload(Lang::Current());
    SaveConfig();
}

// The wallpapers Settings, Look offers: each place its own pattern, one of the
// game's patterns everywhere, or none.
struct WallpaperOpt { const char* code; const char* key; Art::Id pat; };
static const WallpaperOpt kWallpapers[] = {
    { "auto",       "settings.wallpaper.auto",       Art::Id::Count },
    { "face",       "settings.wallpaper.face",       Art::Id::PatFace },
    { "food",       "settings.wallpaper.food",       Art::Id::PatFood },
    { "clothes",    "settings.wallpaper.clothes",    Art::Id::PatClothes },
    { "coordinate", "settings.wallpaper.coordinate", Art::Id::PatCoordinate },
    { "goods",      "settings.wallpaper.goods",      Art::Id::PatGoods },
    { "house",      "settings.wallpaper.house",      Art::Id::PatHouse },
    { "building",   "settings.wallpaper.building",   Art::Id::PatBuilding },
    { "ground",     "settings.wallpaper.ground",     Art::Id::PatGround },
    { "island",     "settings.wallpaper.island",     Art::Id::PatIslandEdit },
    { "photo",      "settings.wallpaper.photo",      Art::Id::PatPhoto },
    { "itembook",   "settings.wallpaper.itembook",   Art::Id::PatItemBook },
    { "quiz",       "settings.wallpaper.quiz",       Art::Id::PatQuiz },
    { "off",        "settings.wallpaper.off",        Art::Id::Count },
};
constexpr int kWallpaperN = (int)(sizeof(kWallpapers) / sizeof(kWallpapers[0]));
static int WallpaperIndex() {
    for (int i = 0; i < kWallpaperN; i++) if (gWallpaper == kWallpapers[i].code) return i;
    return 0;
}

// The update row: where the update stands, in a few words.
static std::string UpdateStateText() {
    switch (Updater::GetState()) {
        case Updater::State::Checking:        return Tx("update.state.checking");
        case Updater::State::NoUpdate:        return Tx("update.state.current");
        case Updater::State::UpdateAvailable: return TokenFill(Tx("update.state.available"), "{v}", Updater::GetLatestVersion());
        case Updater::State::Downloading:     return TokenFill(Tx("update.state.downloading"), "{p}",
                                                               std::to_string((int)(Updater::GetProgress() * 100.f)));
        case Updater::State::Ready:           return Tx("update.state.ready");
        case Updater::State::Error:           return Tx("update.state.failed");
        default:                              return Tx("update.state.idle");
    }
}
static void UpdateRowPressed() {
    const bool inEditor = gScreen == Screen::OnSwitch;
    switch (Updater::GetState()) {
        case Updater::State::UpdateAvailable:
            if (!inEditor) { gScreen = Screen::UpdateAvailable; return; }
            if (Updater::CanInstall()) Updater::StartDownload();
            else { Sound::Play(Sound::Voice::Info); Ui::ToastShow(Tx("update.dev.note"), Ui::Tone::Neutral, 4.f); }
            return;
        case Updater::State::Downloading:
            Sound::Play(Sound::Voice::Info);
            return;
        case Updater::State::Ready:
            if (!inEditor) { gScreen = Screen::UpdateAvailable; return; }
            Sound::Play(Sound::Voice::Info);
            Ui::ToastShow(Tx("update.ready.editor"), Ui::Tone::Good, 4.f);
            return;
        case Updater::State::Checking:
            return;
        default:
            Updater::StartCheck();
            return;
    }
}

// The music row's second line: what plays, or why nothing can.
static std::string MusicStatusText() {
    switch (Music::GetState()) {
        case Music::State::Off:     return Tx("music.state.off");
        case Music::State::Loading: return Tx("music.state.loading");
        case Music::State::Missing: {
            const std::string w = Music::Why();
            return Tx(w == "notrunning" ? "music.state.notrunning" : w == "emu" ? "music.state.emu" : "music.state.missing");
        }
        case Music::State::NoGame: {
            const std::string w = Music::Why();
            if (w == "emu")          return Tx("music.state.emu");
            if (w == "notrunning")   return Tx("music.state.notrunning");
            if (w == "notinstalled") return Tx("music.state.notinstalled");
            if (w == "nofile")       return Tx("music.state.nofile");
            return TokenFill(Tx("music.state.error"), "{code}", w.empty() ? std::string("?") : w);
        }
        case Music::State::Playing: {
            const std::string file = Music::Playing();
            const std::string code = Music::PieceOf(file);
            return TokenFill(Tx("music.state.playing"), "{name}", code.empty() ? file : Tx(("music.piece." + code).c_str()));
        }
        default:                    return Tx("settings.item.music.desc");
    }
}

static int SettingsRowCount() {
    switch (sSetCat) {
        case SC_LOOK: return 5;
        case SC_SOUND: return 7;
        case SC_LANG: return 2;
        case SC_PICTURES: return 5;
        case SC_FILES: return 3;
        case SC_NETWORK: return 3;
        default: return 2;
    }
}

// ── Pictures as the game will hold them, made on a thread ───────────────────
// TextureProcessor::PreviewTexture takes the steps an import takes (turn,
// fit, bars, alpha cut, block encoder, read back), so what is shown is what
// gets written. The encoder is slow on the console: a run goes on a thread
// and the last picture stays meanwhile. Settings → Pictures and the import
// sheet each keep one.
struct TexPreviewWorker {
    struct Job {
        int shape = -2, enc = 0, bc1 = 0, fit = 0, rot = 0, W = 0, H = 0, fmt = 0;
        bool fh = false, fv = false;
        uint8_t mr = 0, mg = 0, mb = 0, ma = 0;
        uint32_t gen = 0;              // which source picture
        bool Same(const Job& o) const {
            return shape == o.shape && enc == o.enc && bc1 == o.bc1 && fit == o.fit && rot == o.rot && W == o.W && H == o.H &&
                   fmt == o.fmt && fh == o.fh && fv == o.fv && mr == o.mr && mg == o.mg && mb == o.mb && ma == o.ma && gen == o.gen;
        }
    };
    const RgbaImage* src = nullptr;    // the caller keeps it alive (and unchanged) while busy
    Job job, done;
    RgbaImage out;
    Thread th{};
    bool open = false;
    std::atomic<bool> busy{ false }, ready{ false };
    SDL_Texture* tex = nullptr;
    int texW = 0, texH = 0;
    double shownAt = -10;

    static Job JobOf(const TextureProcessor::ImportOptions& o, int W, int H, TextureProcessor::TextureFormat f, uint32_t gen) {
        Job j;
        j.shape = (int)o.canvasShape; j.enc = (int)o.encoder; j.bc1 = (int)o.bc1Mode; j.fit = (int)o.fitMode;
        j.rot = o.rotate; j.fh = o.flipH; j.fv = o.flipV; j.W = W; j.H = H; j.fmt = (int)f;
        j.mr = o.matte.r; j.mg = o.matte.g; j.mb = o.matte.b; j.ma = o.matte.a; j.gen = gen;
        return j;
    }
    static TextureProcessor::ImportOptions OptsOf(const Job& j) {
        TextureProcessor::ImportOptions o;
        o.canvasShape = (TextureProcessor::CanvasShape)j.shape; o.encoder = (TextureProcessor::Bc1Encoder)j.enc;
        o.bc1Mode = (TextureProcessor::Bc1Mode)j.bc1; o.fitMode = (TextureProcessor::FitMode)j.fit;
        o.rotate = j.rot; o.flipH = j.fh; o.flipV = j.fv;
        o.matte = TextureProcessor::Matte{ j.mr, j.mg, j.mb, j.ma };
        return o;
    }
    static void Run(void* p) {
        TexPreviewWorker* w = (TexPreviewWorker*)p;
        const Job j = w->job;
        w->out = TextureProcessor::PreviewTexture(*w->src, j.W, j.H, (TextureProcessor::TextureFormat)j.fmt, OptsOf(j));
        w->done = j;
        w->ready = true;
        w->busy = false;
    }
    void Join() { if (open) { threadWaitForExit(&th); threadClose(&th); open = false; } }
    // Each frame: a finished picture onto the screen, and a new run when the
    // wish changed. Returns what to draw (possibly from an older wish).
    SDL_Texture* Update(const RgbaImage* s, const Job& want) {
        if (ready) {
            ready = false;
            Join();
            if (tex) { SDL_DestroyTexture(tex); tex = nullptr; }
            if (out.width > 0) tex = SDL_CreateTexture(gRen, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, out.width, out.height);
            if (tex) {
                SDL_UpdateTexture(tex, nullptr, out.pixels.data(), out.width * 4);
                SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
                texW = out.width; texH = out.height;
                shownAt = Motion::Now();
            }
        }
        if (!busy && !ready && s && s->width > 0 && !want.Same(done)) {
            Join();
            src = s;
            job = want;
            busy = true;
            if (R_SUCCEEDED(threadCreate(&th, Run, this, nullptr, 256 * 1024, 0x2D, -2))) {
                if (R_SUCCEEDED(threadStart(&th))) open = true;
                else { threadClose(&th); Run(this); }
            } else Run(this);                       // no thread: here, once
        }
        return tex;
    }
    bool Busy() const { return busy; }
    float FadeIn() const { return Motion::Reduced() ? 1.f : std::min(1.f, (float)((Motion::Now() - shownAt) / 0.2)); }
    void Stop() { Join(); busy = false; }
    void Drop() {
        Stop();
        ready = false;
        if (tex) { SDL_DestroyTexture(tex); tex = nullptr; }
        texW = texH = 0;
        done = Job{};
        out = RgbaImage();
    }
};

// A picture in a box, on a checkerboard (see-through parts show as such).
static void DrawPreviewPicture(SDL_Texture* t, int tw, int th, SDL_Rect box, float alpha) {
    const float k = tw > 0 && th > 0 ? std::min((float)box.w / tw, (float)box.h / th) : 1.f;
    const int dw = tw > 0 ? (int)(tw * k) : box.w, dh = th > 0 ? (int)(th * k) : box.h;
    const SDL_Rect pic{ box.x + (box.w - dw) / 2, box.y + (box.h - dh) / 2, dw, dh };
    const int cell = 10;
    for (int yy = 0; yy < pic.h; yy += cell)
        for (int xx = 0; xx < pic.w; xx += cell) {
            const bool odd = ((xx / cell) + (yy / cell)) & 1;
            FillRectSquare(pic.x + xx, pic.y + yy, std::min(cell, pic.w - xx), std::min(cell, pic.h - yy),
                           odd ? Alpha(gTheme.text, 34) : Alpha(gTheme.text, 14));
        }
    if (t) DrawTexture(t, nullptr, pic, (uint8_t)(255 * std::max(0.f, std::min(1.f, alpha))));
    DrawRectRounded(pic.x - 1, pic.y - 1, pic.w + 2, pic.h + 2, 4, Alpha(gTheme.border, 220));
}

// The options the picture rows set, as an import uses them.
static TextureProcessor::ImportOptions PictureOptions() {
    TextureProcessor::ImportOptions o;
    o.canvasShape = gCanvasShape; o.encoder = gEncMode; o.bc1Mode = gBc1Mode; o.fitMode = gFitMode; o.matte = gMatte;
    return o;
}

// ── Pictures: what the settings do to a picture ─────────────────────────────
// A sample (the news studio, wide on purpose) as the default slot (512 x 512,
// BC1) would hold it.
namespace PicPreview {
static RgbaImage sSample;              // the sample, loaded once
static SDL_Texture* sSampleTex = nullptr;
static TexPreviewWorker sWork;

static bool LoadSample() {
    if (sSample.width) return true;
    if (!TextureProcessor::LoadImageFile("romfs:/gfx/news_bg.jpg", sSample).empty() || sSample.width <= 0) { sSample = RgbaImage(); return false; }
    sSampleTex = SDL_CreateTexture(gRen, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, sSample.width, sSample.height);
    if (sSampleTex) {
        SDL_UpdateTexture(sSampleTex, nullptr, sSample.pixels.data(), sSample.width * 4);
        SDL_SetTextureScaleMode(sSampleTex, SDL_ScaleModeLinear);
    }
    return true;
}

// On the way out: let a run that is still going finish first.
static void Stop() { sWork.Stop(); }

static void Draw(SDL_Rect r) {
    if (!LoadSample()) return;
    const int S = 512;
    SDL_Texture* t = sWork.Update(&sSample, TexPreviewWorker::JobOf(PictureOptions(), S, S, TextureProcessor::TextureFormat::Bc1, 1));
    // The sample, then what the game will hold.
    W::Eyebrow(r.x, r.y, Tx("settings.preview.before"), gTheme.faint);
    const int sw = r.w, sh = sw * 9 / 16;
    if (sSampleTex) {
        FillRectRounded(r.x, r.y + 22, sw, sh, 10, Alpha(gTheme.text, 10));
        DrawTexture(sSampleTex, nullptr, SDL_Rect{ r.x, r.y + 22, sw, sh });
    }
    const int ay = r.y + 22 + sh + 18;
    W::Eyebrow(r.x, ay, Tx("settings.preview.after"), gTheme.faint);
    const int boxY = ay + 22, boxH = r.y + r.h - boxY;
    const int side = std::min(sw, boxH);
    const SDL_Rect box{ r.x + (sw - side) / 2, boxY + (boxH - side) / 2, side, side };
    DrawPreviewPicture(t, sWork.texW, sWork.texH, box, sWork.FadeIn());
    if (sWork.Busy()) DrawTextC(Tx("settings.preview.working"), box.x + box.w / 2, box.y + box.h / 2, gTheme.text, Font::Body);
    DrawTextR(std::to_string(S) + " \xC3\x97 " + std::to_string(S) + "  \xC2\xB7  BC1", r.x + r.w, ay + 8, gTheme.faint, Font::Xs);
}
}  // namespace PicPreview

// The Background (matte) choice: 3 = a colour of your own, on the colour sheet.
static int MatteIndex() {
    if (gMatte.a == 0) return 0;
    if (gMatte.r == 255 && gMatte.g == 255 && gMatte.b == 255) return 1;
    if (gMatte.r == 0 && gMatte.g == 0 && gMatte.b == 0) return 2;
    return 3;
}
static void MattePick(int v) {
    if (v == 3) {
        OpenColorPicker(2);
        gShowColorPicker = false;
        sColorTarget = 2;
        sColorSheet = true;
        Motion::Forget(Ui::HashStr("set.colorsheet"));
        return;
    }
    TSetMatte(v);
}
// The Background row: the colour as a dot, the choice behind A.
static void MatteRow(Ui::Id id, SDL_Rect r, bool enabled) {
    static const char* const kMatte[] = { "opt.matte.transparent", "opt.matte.white", "opt.matte.black", "opt.matte.custom" };
    const int cur = MatteIndex();
    std::vector<std::string> labels;
    for (int k = 0; k < 4; k++) labels.push_back(Tx(kMatte[k]));
    std::vector<SDL_Color> dots;
    if (cur > 0) dots.push_back(SDL_Color{ gMatte.r, gMatte.g, gMatte.b, 255 });
    W::RowStepper(id, r, { Tx("settings.matte"), Tx(enabled ? "settings.matte.desc" : "settings.matte.disabled") }, labels[cur],
                  cur / 3.f, 1.f / 3.f, [cur](int d) { MattePick(std::max(0, std::min(3, cur + d))); },
                  [id, labels, cur]() {
                      W::OpenChoice(id, Tx("settings.matte"), labels, cur, [](int pick) { MattePick(pick); }, Focus::RectOf(id));
                  },
                  Tx("hint.choose"), enabled, dots);
}

// The picture rows (Settings → Pictures and the import sheet): shape,
// encoder, block mode, fit and background. `row(h)` gives each row's place,
// `sep()` draws the line between two. The ids hang off `base`.
template <typename RowFn, typename SepFn>
static void PictureRows(Ui::Id base, RowFn row, SepFn sep, int rowH = W::kRowH) {
    auto opt = [&](uint32_t k, const char* titleKey, const char* descKey, int cur, int count, const char* const* names,
                   void (*set)(int), bool enabled) {
        std::vector<std::string> labels;
        for (int i = 0; i < count; i++) labels.push_back(Tx(names[i]));
        W::RowChoice(Ui::IdMix(base, k), row(rowH), { Tx(titleKey), Tx(descKey) }, labels, cur,
                     [cur, count, set](int d) { set(std::max(0, std::min(count - 1, cur + d))); }, enabled);
        sep();
    };
    static const char* const kShape[] = { "opt.shape.square", "opt.shape.book", "opt.shape.tv" };
    static const char* const kEnc[]   = { "opt.encoder.custom", "opt.encoder.pca", "opt.encoder.rgbcx" };
    static const char* const kBc1[]   = { "opt.bc1.auto", "opt.bc1.four", "opt.bc1.three" };
    static const char* const kFit[]   = { "opt.fit.cover", "opt.fit.contain", "opt.fit.fill" };
    const int shape = (int)gCanvasShape, enc = (int)gEncMode, bc1 = (int)gBc1Mode, fit = (int)gFitMode;
    opt(1, "settings.shape",
        shape == 0 ? "settings.shape.hint.square" : shape == 1 ? "settings.shape.hint.book" : "settings.shape.hint.tv",
        shape, 3, kShape, TSetCanvasShape, true);
    opt(2, "settings.encoder",
        enc == 0 ? "settings.encoder.hint.custom" : enc == 2 ? "settings.encoder.hint.rgbcx" : "settings.encoder.hint.pca",
        enc, 3, kEnc, TSetEncMode, true);
    const bool bcOn = gEncMode != TextureProcessor::Bc1Encoder::PCA;
    opt(3, "settings.bc1",
        !bcOn ? "settings.bc1.disabled" : bc1 == 0 ? "settings.bc1.hint.auto" : bc1 == 1 ? "settings.bc1.hint.fourColor" : "settings.bc1.hint.threeColor",
        bc1, 3, kBc1, TSetBc1Mode, bcOn);
    opt(4, "settings.fit",
        fit == 0 ? "settings.fit.hint.cover" : fit == 1 ? "settings.fit.hint.contain" : "settings.fit.hint.fill",
        fit, 3, kFit, TSetFitMode, true);
    MatteRow(Ui::IdMix(base, 5), row(rowH), gFitMode == TextureProcessor::FitMode::Contain);
}

// The rows of the current category inside a card at x, y, w.
// Pressed over and over, the test sound gets fed up: "okay, okay", then
// cross, then it sulks for a moment and plays nothing. Left alone, it calms
// down (an easter egg).
static int sSoundAnger = 0;
static double sSoundLast = -10, sSoundSulk = -10;
static void SoundTestPress() {
    const double now = Motion::Now();
    if (now < sSoundSulk) { Sound::Play(Sound::Voice::Seat, { 0.15f, true }); sSoundLast = now; return; }
    const int calm = (int)((now - sSoundLast) / 1.5);
    sSoundAnger = std::max(0, sSoundAnger - calm) + 1;
    sSoundLast = now;
    if (sSoundAnger < 5) Sound::Play(Sound::Voice::Ok);
    else if (sSoundAnger < 9) Sound::Play(Sound::Voice::Tick, { 0.95f, true });
    else if (sSoundAnger < 13) Sound::Play(Sound::Voice::Err);
    else { Sound::Play(Sound::Voice::Err, { 0.5f, true }); sSoundSulk = now + 6.0; sSoundAnger = 0; }
}

static void SettingsRows(int x, int y, int w) {
    int cy = y;
    auto row = [&](int h) { SDL_Rect r{ x, cy, w, h }; cy += h; return r; };
    auto sep = [&]() { W::Hairline(x + 18, cy, w - 36); cy += 1; };
    switch (sSetCat) {
    case SC_LOOK: {
        const auto& av = ThemeNS::Available();
        const int n = std::max(1, (int)av.size());
        const int ti = ThemeIndex();
        W::RowStepper(Ui::HashStr("set.theme"), row(W::kRowH), { Tx("settings.item.theme"), Tx("settings.item.theme.desc") },
                      av.empty() ? std::string() : Lang::T(av[ti].labelKey), n > 1 ? (float)ti / (n - 1) : 0.f,
                      n > 1 ? 1.f / (n - 1) : 1.f,
                      [ti](int d) {
                          const auto& a = ThemeNS::Available();
                          const int k = (int)a.size();
                          if (k) ThemeSet(a[std::max(0, std::min(k - 1, ti + d))].code);
                      },
                      []() { sThemeMenu = true; }, Tx("hint.choose"), true,
                      av.empty() ? std::vector<SDL_Color>() : ThemeNS::Swatches(av[ti].code));
        sep();
        {
            const int wi = WallpaperIndex();
            std::vector<std::string> walls;
            for (int k = 0; k < kWallpaperN; k++) walls.push_back(Tx(kWallpapers[k].key));
            W::RowChoice(Ui::HashStr("set.wallpaper"), row(W::kRowH), { Tx("settings.item.wallpaper"), Tx("settings.item.wallpaper.desc") },
                          walls, wi,
                          [wi](int d) {
                              const int k = std::max(0, std::min(kWallpaperN - 1, wi + d));
                              gWallpaper = kWallpapers[k].code;
                              SaveConfig();
                          });
        }
        sep();
        W::RowSwatch(Ui::HashStr("set.accent"), row(W::kRowH), { Tx("settings.item.accent"), Tx("settings.item.accent.desc") },
                     gTheme.gold, gAccentCustom ? std::string() : Tx("settings.default"),
                     []() { OpenColorPicker(0); gShowColorPicker = false; sColorTarget = 0; sColorSheet = true; });
        sep();
        W::RowSwatch(Ui::HashStr("set.bg"), row(W::kRowH), { Tx("settings.item.bg"), Tx("settings.item.bg.desc") },
                     gTheme.bg, gBgCustom ? std::string() : Tx("settings.default"),
                     []() { OpenColorPicker(1); gShowColorPicker = false; sColorTarget = 1; sColorSheet = true; });
        sep();
        W::RowToggle(Ui::HashStr("set.motion"), row(W::kRowH), { Tx("settings.item.motion"), Tx("settings.item.motion.desc") },
                     gReduceMotion, [](bool v) { gReduceMotion = v; SaveConfig(); });
        break;
    }
    case SC_SOUND: {
        W::RowToggle(Ui::HashStr("set.sound"), row(W::kRowH), { Tx("settings.item.sound"), Tx("settings.item.sound.desc") },
                     gSoundOn, [](bool v) { gSoundOn = v; SaveConfig(); });
        sep();
        {
            // Whose sounds: Tomodachi Life's own, or the classic ones.
            const std::vector<std::string> styles{ Tx("soundstyle.game"), Tx("soundstyle.classic") };
            const int st = Sound::Style();
            W::RowChoice(Ui::HashStr("set.soundstyle"), row(W::kRowH), { Tx("settings.item.soundstyle"), Tx("settings.item.soundstyle.desc") },
                         styles, st, [st](int d) {
                             Sound::SetStyle(std::max(0, std::min(1, st + d)));
                             SaveConfig();
                             Sound::Play(Sound::Voice::Ok);
                         });
        }
        sep();
        const float vol = Sound::Volume();
        char buf[16];
        snprintf(buf, sizeof buf, "%d%%", (int)lroundf(vol * 100));
        W::RowSlider(Ui::HashStr("set.volume"), row(W::kRowH), { Tx("settings.item.volume"), Tx("settings.item.volume.desc") },
                     vol / 2.f, 0.05f, buf, [vol](int d) {
                         Sound::SetVolume(std::max(0.f, std::min(2.f, vol + d * 0.1f)));
                         gSoundVolumeDirty = true;
                     });
        sep();
        {
            const double now = Motion::Now();
            const bool sulking = now < sSoundSulk, after = !sulking && now - sSoundSulk < 4.0;
            const int anger = sulking ? 99 : std::max(0, sSoundAnger - (int)((now - sSoundLast) / 1.5));
            const char* mood = sulking ? "soundtest.sulk" : after ? "soundtest.calm" : anger >= 9 ? "soundtest.mood2" : anger >= 5 ? "soundtest.mood1" : nullptr;
            // Shaking more the crosser it is, settling after each press.
            const float amp = (sulking ? 3.f : anger >= 5 ? (float)std::min(anger, 12) * 0.9f : 0.f) * expf(-(float)(now - sSoundLast) * 5.f);
            const float dx = Motion::Reduced() ? 0.f : sinf((float)now * 70.f) * amp;
            PushXform(dx, 0, 1.f, 0, 0, 1.f);
            W::RowLink(Ui::HashStr("set.soundtest"), row(W::kRowH), { Tx("settings.item.soundtest"), mood ? Tx(mood) : std::string() }, "",
                       []() { SoundTestPress(); }, UiIcons::Id::Speaker, sulking || anger >= 9, Focus::OwnSound);
            PopXform();
        }
        sep();
        // The game's music: on or off (and what plays, or why it cannot), how
        // loud, and which.
        W::RowToggle(Ui::HashStr("set.music"), row(W::kRowH), { Tx("settings.item.music"), MusicStatusText() },
                     gMusicOn, [](bool v) { gMusicOn = v; SaveConfig(); });
        sep();
        {
            const float mv = Music::Volume();
            char mb[16];
            snprintf(mb, sizeof mb, "%d%%", (int)lroundf(mv * 100));
            W::RowSlider(Ui::HashStr("set.musicvol"), row(W::kRowH), { Tx("settings.item.musicvol"), Tx("settings.item.musicvol.desc") },
                         mv / 2.f, 0.05f, mb, [mv](int d) {
                             Music::SetVolume(std::max(0.f, std::min(2.f, mv + d * 0.1f)));
                             gSoundVolumeDirty = true;
                         });
        }
        sep();
        {
            std::vector<std::string> picks{ Tx("music.auto") };
            int pi = 0;
            for (int k = 0; k < Music::kPieceCount; k++) {
                picks.push_back(Tx(("music.piece." + std::string(Music::kPieces[k].code)).c_str()));
                if (gMusicPick == Music::kPieces[k].code) pi = k + 1;
            }
            W::RowChoice(Ui::HashStr("set.musicpick"), row(W::kRowH), { Tx("settings.item.musicpick"), Tx("settings.item.musicpick.desc") },
                         picks, pi, [pi](int d) {
                             const int k = std::max(0, std::min(Music::kPieceCount, pi + d));
                             gMusicPick = k == 0 ? std::string("auto") : std::string(Music::kPieces[k - 1].code);
                             SaveConfig();
                         });
        }
        break;
    }
    case SC_LANG: {
        const auto& av = Lang::Available();
        const int n = std::max(1, (int)av.size());
        const int li = LangIndex();
        W::RowStepper(Ui::HashStr("set.lang"), row(W::kRowH), { Tx("settings.item.lang"), Tx("settings.item.lang.desc") },
                      av.empty() ? std::string() : av[li].label, n > 1 ? (float)li / (n - 1) : 0.f, n > 1 ? 1.f / (n - 1) : 1.f,
                      [li](int d) {
                          const auto& a = Lang::Available();
                          const int k = (int)a.size();
                          if (k) LangSet(a[std::max(0, std::min(k - 1, li + d))].code);
                      },
                      []() { sLangMenu = true; }, Tx("hint.choose"), true, {}, av.empty() ? Art::Id::Count : Art::Flag(av[li].code.c_str()));
        sep();
        W::RowLink(Ui::HashStr("set.langhelp"), row(W::kRowH), { Tx("settings.item.translate"), Tx("settings.language.hint") }, "",
                   nullptr, UiIcons::Id::Count);
        break;
    }
    case SC_PICTURES: {
        // The rows on the left, what they do to a picture on the right.
        const int pw = w >= 760 ? 290 : 0;
        if (pw) {
            PicPreview::Draw(SDL_Rect{ x + w - pw - 14, y + 10, pw, 5 * W::kRowH + 4 - 20 });
            FillRectSquare(x + w - pw - 30, y + 14, 1, 5 * W::kRowH - 24, Alpha(gTheme.border, 160));
            w -= pw + 32;
        }
        PictureRows(Ui::HashStr("set.pic"), [&](int h) { return row(h); }, sep);
        break;
    }
    case SC_FILES: {
        W::RowLink(Ui::HashStr("set.export"), row(W::kRowH), { Tx("settings.item.export"), Tx("settings.item.export.desc") },
                   gExportPath, []() {
                       gBrowseForExportDir = true; gBrowseForMii = false;
                       std::string sp = gExportPath;
                       struct stat st{};
                       if (stat(sp.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
                           const size_t p = sp.rfind('/');
                           sp = (p && p != std::string::npos) ? sp.substr(0, p) : "/";
                       }
                       BrowseRefresh(sp);
                       sFolderSheet = true;
                   }, UiIcons::Id::Folder);
        sep();
        W::RowStepper(Ui::HashStr("set.maxbk"), row(W::kRowH), { Tx("settings.item.maxbk"), Tx("settings.item.maxbk.desc") },
                      std::to_string(gMaxBackups), (gMaxBackups - 1) / 98.f, 1.f / 98.f,
                      [](int d) { gMaxBackups = std::max(1, std::min(99, gMaxBackups + d)); SaveConfig(); });
        sep();
        {
            // Real Mii pictures: made from the game on this console.
            std::string v;
            const MiiPack::State st = MiiPack::GetState();
            if (st == MiiPack::State::Running) {
                char b[16];
                snprintf(b, sizeof b, " %d%%", (int)(MiiPack::Progress() * 100.f));
                v = Tx("miipack.making") + b;
            } else if (st == MiiPack::State::Failed) {
                const std::string e = MiiPack::Error();
                const size_t bar = e.find('|');
                if (bar != std::string::npos) v = TokenFill(Tx(e.substr(0, bar).c_str()), "{code}", e.substr(bar + 1));
                else v = e.rfind("miipack.", 0) == 0 ? Tx(e.c_str()) : Tx("miipack.failed");
            } else {
                v = MiiPack::Present() ? Tx("miipack.ready") : Tx("miipack.none");
            }
            if (MiiPack::BuiltIn() && st != MiiPack::State::Running) v = Tx("miipack.builtin");
            W::RowLink(Ui::HashStr("set.miipics"), row(W::kRowH), { Tx("settings.item.miipics"), Tx("settings.item.miipics.desc") },
                       v, []() {
                           // The app carries them: nothing to make.
                           if (MiiPack::BuiltIn()) { Sound::Play(Sound::Voice::Info); return; }
                           if (MiiPack::GetState() != MiiPack::State::Running) MiiPack::Start(true);
                       }, UiIcons::Id::Users);
        }
        break;
    }
    case SC_NETWORK: {
        W::RowToggle(Ui::HashStr("set.webui"), row(W::kRowH), { Tx("settings.item.webui"), Tx("settings.item.webui.desc") },
                     gWebUiEnabled, [](bool v) { gWebUiEnabled = v; SaveConfig(); });
        sep();
        W::RowToggle(Ui::HashStr("set.update"), row(W::kRowH), { Tx("settings.item.update"), Tx("settings.item.update.desc") },
                     gUpdateCheck, [](bool v) { gUpdateCheck = v; SaveConfig(); });
        sep();
        // The update itself: check, download, and (outside the editor) install.
        W::RowLink(Ui::HashStr("set.updnow"), row(W::kRowH),
                   { Tx("update.row"), Updater::IsDevBuild() ? Tx("update.row.dev") : std::string("v" APP_VERSION) },
                   UpdateStateText(), []() { UpdateRowPressed(); }, UiIcons::Id::Download);
        break;
    }
    case SC_ABOUT: {
        W::RowLink(Ui::HashStr("set.version"), row(W::kRowH),
                   { Updater::IsDevBuild() ? "TomoToolNX " + Tx("update.devbuild") : std::string("TomoToolNX v" APP_VERSION),
                     Tx("settings.about.license") }, "",
                   nullptr, UiIcons::Id::Count);
        sep();
        W::RowLink(Ui::HashStr("set.github"), row(W::kRowH), { "GitHub", "github.com/" GITHUB_REPO }, "", nullptr, UiIcons::Id::Count);
        break;
    }
    }
}

// The colour sheet: hue, saturation and brightness, applied live.
static void ColorSheet() {
    const Ui::Id id = Ui::HashStr("set.colorsheet");
    auto close = [](bool keep) {
        if (!keep) {
            if (gPickerTarget == 2) gMatte = gPickerPrevMatte;
            else {
                if (gPickerTarget == 0) { gAccentColor = gPickerPrevColor; gAccentCustom = gPickerPrevCustom; }
                else                    { gBgColor     = gPickerPrevColor; gBgCustom     = gPickerPrevCustom; }
                ApplyCustomColors();
            }
        } else SaveConfig();
        sColorSheet = false;
        Motion::Forget(Ui::HashStr("set.colorsheet"));
    };
    const double since = Motion::FirstSeen(id);
    const double now = Motion::Now();
    if (now - since < 0.0001) { Sound::Play(Sound::Voice::Open); Focus::Set(Ui::HashStr("set.hue")); }
    const int w = 720, h = 432, x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
    Focus::PushLayer(Focus::Layer::Sheet, [close]() { close(false); }, Tx("hint.cancel"), SDL_Rect{ x, y, w, h });
    DimScreen((int)(130 * std::min(1.f, (float)((now - since) / 0.16))));
    // Rises into place (LLM-Selfbot's sheet: 14 px, spring).
    const float e = Motion::Reduced() ? 1.f : Motion::Ease(Motion::Curve::Spring, std::min(1.f, (float)((now - since) / 0.42)));
    PushXform(0, 14.f * (1 - e), 0.98f + 0.02f * e, SCREEN_W / 2.f, SCREEN_H / 2.f, std::min(1.f, e * 1.4f));
    FillRectRounded(x, y + 10, w, h, 26, SDL_Color{ 0, 0, 0, 60 });
    FillRectRounded(x, y, w, h, 26, Mix(gTheme.panel, gTheme.text, 0.03f));
    DrawRectRounded(x, y, w, h, 26, Alpha(gTheme.border, 230));
    W::IconTile(x + 28, y + 26, 52, UiIcons::Id::Palette, gTheme.gold);
    DrawTextVC(Tx(sColorTarget == 0 ? "picker.accent" : sColorTarget == 2 ? "picker.matte" : "picker.bg"), x + 96, y + 52, gTheme.text, Font::Title);
    // The colour itself, big, with its code.
    FillRectRounded(x + w - 28 - 132, y + 30, 132, 44, 14, gPickerColor);
    DrawRectRounded(x + w - 28 - 132, y + 30, 132, 44, 14, Alpha(gTheme.text, 50));
    char hex[16];
    snprintf(hex, sizeof hex, "#%02X%02X%02X", gPickerColor.r, gPickerColor.g, gPickerColor.b);
    DrawTextR(hex, x + w - 28 - 132 - 14, y + 52, gTheme.dim, Font::Body);
    const int rx = x + 14, rw = w - 28;
    int ry = y + 100;
    char v[16];
    snprintf(v, sizeof v, "%d", (int)lroundf(gPickH));
    W::RowSlider(Ui::HashStr("set.hue"), { rx, ry, rw, W::kRowHSmall }, { Tx("picker.hue"), "" }, gPickH / 360.f, 10.f / 360.f, v,
                 [](int d) { gPickH = std::fmod(gPickH + d * 10.f + 360.f, 360.f); PickerSetFromHSV(); });
    ry += W::kRowHSmall + 6;
    snprintf(v, sizeof v, "%d%%", (int)lroundf(gPickS * 100));
    W::RowSlider(Ui::HashStr("set.sat"), { rx, ry, rw, W::kRowHSmall }, { Tx("picker.saturation"), "" }, gPickS, 0.05f, v,
                 [](int d) { gPickS = std::max(0.f, std::min(1.f, gPickS + d * 0.05f)); PickerSetFromHSV(); });
    ry += W::kRowHSmall + 6;
    snprintf(v, sizeof v, "%d%%", (int)lroundf(gPickV * 100));
    W::RowSlider(Ui::HashStr("set.val"), { rx, ry, rw, W::kRowHSmall }, { Tx("picker.brightness"), "" }, gPickV, 0.05f, v,
                 [](int d) { gPickV = std::max(0.f, std::min(1.f, gPickV + d * 0.05f)); PickerSetFromHSV(); });
    // The hue band under the sliders: where the hue is.
    ry += W::kRowHSmall + 16;
    const int bandX = rx + 22, bandW = rw - 44;
    for (int i = 0; i < bandW; i += 4)
        FillRectSquare(bandX + i, ry, std::min(4, bandW - i), 8, HSVtoRGB(360.f * i / bandW, std::max(0.45f, gPickS), std::max(0.7f, gPickV)));
    FillDisc(bandX + bandW * gPickH / 360.f, ry + 4.f, 7.f, gTheme.text);
    ry += 34;
    const int bw = 200, gap = 14, bx = x + (w - (3 * bw + 2 * gap)) / 2;
    W::Button(Ui::HashStr("set.c.save"), { bx, ry, bw, 54 }, Tx("picker.save"), W::Kind::Primary,
              [close]() { Sound::Play(Sound::Voice::Done); close(true); }, UiIcons::Id::Check, true, Focus::OwnSound);
    W::Button(Ui::HashStr("set.c.reset"), { bx + bw + gap, ry, bw, 54 }, Tx("picker.reset"), W::Kind::Neutral, []() {
        if (gPickerTarget == 2) {             // the matte: back to the colour it had
            const TextureProcessor::Matte m = gPickerPrevMatte.a ? gPickerPrevMatte : TextureProcessor::Matte{ 136, 136, 136, 255 };
            gMatte = TextureProcessor::Matte{ m.r, m.g, m.b, 255 };
            gPickerColor = SDL_Color{ m.r, m.g, m.b, 255 };
            RGBtoHSV(gPickerColor, gPickH, gPickS, gPickV);
            Sound::Play(Sound::Voice::Undo, { 0.5f, false, 1.f, 40.f });
            return;
        }
        if (gPickerTarget == 0) gAccentCustom = false; else gBgCustom = false;
        ApplyCustomColors();
        gPickerColor = gPickerTarget == 0 ? gTheme.gold : gTheme.bg;
        RGBtoHSV(gPickerColor, gPickH, gPickS, gPickV);
        Sound::Play(Sound::Voice::Undo, { 0.5f, false, 1.f, 40.f });
    });
    W::Button(Ui::HashStr("set.c.cancel"), { bx + 2 * (bw + gap), ry, bw, 54 }, Tx("picker.cancel"), W::Kind::Neutral,
              [close]() { close(false); });
    PopXform();
    Focus::PopLayer();
}

// The export folder: a list of folders to walk into, and "use this one".
static void FolderSheet() {
    std::vector<W::MenuItem> items;
    items.push_back({ Tx("browse.use_folder"), gBrowseCurPath, UiIcons::Id::Check, false, []() {
        gExportPath = gBrowseCurPath;
        mkdir(gExportPath.c_str(), 0777);
        SaveConfig();
        sFolderSheet = false;
        Sound::Play(Sound::Voice::Done, { 0.5f, false, 1.f, 60.f });
        Ui::ToastShow(Tx("browse.folder_set"), Ui::Tone::Good);
    }, false });
    for (const auto& e : gBrowseEntries) {
        if (!e.isDir) continue;
        const bool up = e.name == "[..]";
        const std::string name = e.name;
        items.push_back({ up ? std::string("..") : e.name, "", up ? UiIcons::Id::ChevronLeft : UiIcons::Id::Folder, false,
                          [name, up]() {
                              std::string np;
                              if (up) {
                                  const size_t p = gBrowseCurPath.rfind('/');
                                  np = (p == 0 || p == std::string::npos) ? "/" : gBrowseCurPath.substr(0, p);
                              } else {
                                  np = (gBrowseCurPath == "/" ? "" : gBrowseCurPath) + "/" + name;
                              }
                              BrowseRefresh(np);
                              Focus::Set(Ui::IdMix(Ui::HashStr("set.folders"), 0u));
                          }, false });
    }
    W::Menu(Ui::HashStr("set.folders"), Tx("browse.title.export"), items, []() { sFolderSheet = false; }, Tx("hint.cancel"));
}

// The lists and sheets a settings row opens.
static void SettingsSheets() {
    if (sThemeMenu) {
        std::vector<W::MenuItem> items;
        const auto& av = ThemeNS::Available();
        for (int i = 0; i < (int)av.size(); i++) {
            const std::string code = av[i].code;
            items.push_back({ Lang::T(av[i].labelKey), "", UiIcons::Id::Palette, false,
                              [code]() { ThemeSet(code); sThemeMenu = false; }, i == ThemeIndex(), ThemeNS::Swatches(code) });
        }
        W::Menu(Ui::HashStr("set.thememenu"), Tx("settings.item.theme"), items, []() { sThemeMenu = false; }, Tx("hint.close"),
                ThemeIndex(), Focus::RectOf(Ui::HashStr("set.theme")));
    }
    if (sLangMenu) {
        std::vector<W::MenuItem> items;
        const auto& av = Lang::Available();
        for (int i = 0; i < (int)av.size(); i++) {
            const std::string code = av[i].code;
            items.push_back({ av[i].label, "", UiIcons::Id::Globe, false, [code]() { LangSet(code); sLangMenu = false; },
                              i == LangIndex(), {}, Art::Flag(code.c_str()) });
        }
        W::Menu(Ui::HashStr("set.langmenu"), Tx("settings.item.lang"), items, []() { sLangMenu = false; }, Tx("hint.close"),
                LangIndex(), Focus::RectOf(Ui::HashStr("set.lang")));
    }
    if (sColorSheet) ColorSheet();
    if (sFolderSheet) FolderSheet();
}

// The category's rows in their card, from `y` down to the hint bar: a card
// taller than that scrolls, keeping the highlighted row in view.
static Focus::Scroll sSetScroll;
static int sSetScrollCat = -1;
static void SettingsCard(int x, int y, int w) {
    if (sSetScrollCat != sSetCat) { sSetScroll = Focus::Scroll(); sSetScrollCat = sSetCat; }
    const int n = SettingsRowCount();
    const int h = n * W::kRowH + (n - 1) + 12;
    const SDL_Rect view{ x - 8, y - 6, w + 16, SCREEN_H - 62 - (y - 6) };
    Focus::BeginScroll(Ui::HashStr("set.body"), view, sSetScroll);
    const int top = y - (int)sSetScroll.offset;
    const SDL_Rect card{ x, top, w, h };
    W::BeginBlock(card);
    W::Card(card);
    SettingsRows(x + 6, top + 6, w - 12);
    W::EndBlock();
    Focus::EndScroll((float)(h + 12));
}

static void PageSettings() {
    // Selection follows the highlight in the category list; coming back from
    // the rows with ◀ lands on the category already shown.
    {
        const Ui::Id cur = Focus::Current();
        int curCat = -1;
        bool lastWasCat = false;
        for (int i = 0; i < SC_COUNT; i++) {
            if (cur == SetCatId(i)) curCat = i;
            if (sSetLastFocus == SetCatId(i)) lastWasCat = true;
        }
        if (curCat >= 0 && curCat != sSetCat) {
            if (!lastWasCat && sSetLastFocus != 0 && !Input::Last().tap) Focus::Set(SetCatId(sSetCat));
            else SetCategory(curCat);
        }
    }

    SidePanel();
    Brand(28, 30, Tx("settings.title"));
    const int ix = 24, iw = kSideW - 48;
    int y = 112;
    W::SidebarGroup(ix + 10, y, Tx("settings.title"));
    y += 30;
    const int listTop = y;
    W::SidebarMarks(kSetCatGroup, SDL_Rect{ ix, listTop + sSetCat * 54, iw, 48 });
    for (int i = 0; i < SC_COUNT; i++) {
        const int idx = i;
        W::SidebarItem(SetCatId(i), { ix, y, iw, 48 }, kCats[i].icon, Tx(kCats[i].key), sSetCat == i, false, [idx]() {
            SetCategory(idx);
            if (!Input::Last().tap) Focus::Set(Ui::HashStr(kCats[idx].firstRow));
        });
        y += 54;
    }
    // Back, for fingers (B does the same).
    W::SidebarItem(Ui::HashStr("set.leave"), { ix, SCREEN_H - 24 - 48, iw, 48 }, UiIcons::Id::ChevronLeft, Tx("hint.back"), false, false,
                   []() { gScreen = Screen::UserPick; });

    // The category's page: header, then a card of rows.
    const int cx = kSideW + 44, cw = SCREEN_W - cx - 44;
    const int top = 40;
    W::BeginBlock({ cx, top, cw, 80 });
    const int hy = W::PageHeader(cx, top, cw, kCats[sSetCat].icon, Tx("settings.title"), Tx(kCats[sSetCat].key),
                                 Tx(kCats[sSetCat].descKey));
    W::EndBlock();
    SettingsCard(cx, hy, cw);

    // B: from the rows back to the category; from the categories, out.
    Focus::SetBaseBack([]() {
        const Ui::Id cur = Focus::Current();
        for (int i = 0; i < SC_COUNT; i++)
            if (cur == SetCatId(i)) { Sound::Play(Sound::Voice::Release); gScreen = Screen::UserPick; return; }
        Sound::Play(Sound::Voice::Release);
        Focus::Set(SetCatId(sSetCat));
    }, Tx("hint.back"));
    Focus::Command(Input::L, Tx("hint.section"), []() {
        SetCategory((sSetCat + SC_COUNT - 1) % SC_COUNT);
        Focus::Set(SetCatId(sSetCat));
        Sound::Play(Sound::Voice::Whoosh);
    });
    Focus::Command(Input::R, Tx("hint.section"), []() {
        SetCategory((sSetCat + 1) % SC_COUNT);
        Focus::Set(SetCatId(sSetCat));
        Sound::Play(Sound::Voice::Whoosh);
    });

    SettingsSheets();
    sSetLastFocus = Focus::Current();
}

// Settings inside the editor: the editor's sidebar stays, the categories
// become chips above the rows.
static void SettingsInEditor(int x, int y, int w) {
    W::BeginBlock({ x, y, w, 80 });
    const int hy = W::PageHeader(x, y, w, kCats[sSetCat].icon, Tx("settings.title"), Tx(kCats[sSetCat].key), Tx(kCats[sSetCat].descKey));
    W::EndBlock();
    std::vector<std::pair<std::string, int>> chips;
    std::vector<Art::Id> art;
    for (int i = 0; i < SC_COUNT; i++) { chips.push_back({ Tx(kCats[i].key), -1 }); art.push_back(kCats[i].art); }
    W::BeginBlock({ x, hy, w, 56 });
    W::Chips(Ui::HashStr("set.chips"), x, hy, chips, sSetCat, [](int i) { SetCategory(i); }, w, art);
    W::EndBlock();
    SettingsCard(x, hy + 56 + 16, w);
    SettingsSheets();
}
