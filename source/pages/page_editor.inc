// page_editor.inc — TomoToolNX 2.0: the open island. A sidebar of places
// (LLM-Selfbot's desktop panel) and the pages built on the new kit: Home,
// Backups, Phone & PC and Settings. The places not rebuilt yet (Miis,
// Textures, Player, Map) still draw themselves; the sidebar opens them.
// Included by shell.inc inside namespace S2.

// The sidebar is open (icons and words) or folded to its icons; it glides
// between the two and every place follows: sEdSideW, sEdX and sEdW are this
// frame's, set by EdLayout() before anything is drawn.
constexpr int kEdSideOpenW = 270, kEdSideFoldW = 88;
static int   sEdSideW = kEdSideOpenW;
static int   sEdX = sEdSideW + 36;
static int   sEdW = SCREEN_W - sEdX - 36;
static float sEdFoldK = 0.f;              // 0 open .. 1 folded, this frame
static Motion::Tween sEdFoldTw;
// What the sidebar draws over the page (a folded item's name, the undo tip):
// after the page, so the page does not cover it.
static std::vector<std::function<void()>> sEdLate;

static void EdLayout() {
    static bool sInit = false;
    if (!sInit) { sInit = true; sEdFoldTw.Snap(gSideFolded ? 1.f : 0.f); }
    sEdFoldK = std::min(1.f, std::max(0.f, sEdFoldTw.Value()));
    sEdSideW = (int)lroundf(kEdSideOpenW + (kEdSideFoldW - kEdSideOpenW) * sEdFoldK);
    sEdX = sEdSideW + 36;
    sEdW = SCREEN_W - sEdX - 36;
}

static void EdSetFolded(bool fold) {
    if (gSideFolded == fold) return;
    gSideFolded = fold;
    if (Motion::Reduced()) sEdFoldTw.Snap(fold ? 1.f : 0.f);
    else sEdFoldTw.To(fold ? 1.f : 0.f, 0.36f, Motion::Curve::Swift);
    Sound::Play(fold ? Sound::Voice::Release : Sound::Voice::Open);
    SaveConfig();
}

struct EdPlace { OnSwitchMode mode; const char* key; UiIcons::Id icon; };
static const EdPlace kEdIsland[] = {
    { OnSwitchMode::WebUI,    "tab.webui",    UiIcons::Id::Home  },
    { OnSwitchMode::MiiStats, "tab.mii",      UiIcons::Id::Users },
    { OnSwitchMode::UGC,      "tab.textures", UiIcons::Id::Image },
    { OnSwitchMode::Player,   "tab.player",   UiIcons::Id::User  },
    { OnSwitchMode::Map,      "tab.map",      UiIcons::Id::Map   },
};
static const EdPlace kEdMore[] = {
    { OnSwitchMode::Backups,  "backups.title",  UiIcons::Id::Clock },
    { OnSwitchMode::Phone,    "tab.phone",      UiIcons::Id::Phone },
    { OnSwitchMode::Settings, "settings.title", UiIcons::Id::Gear  },
};
static const OnSwitchMode kEdOrder[] = {
    OnSwitchMode::WebUI, OnSwitchMode::MiiStats, OnSwitchMode::UGC, OnSwitchMode::Player, OnSwitchMode::Map,
    OnSwitchMode::Backups, OnSwitchMode::Phone, OnSwitchMode::Settings,
};
constexpr int kEdOrderN = (int)(sizeof(kEdOrder) / sizeof(kEdOrder[0]));

static void PagePlayer();
static void PageTextures();
static void PageMiis();
static void PageMap();

static Ui::Id EdItemId(OnSwitchMode m) { return Ui::IdMix(Ui::HashStr("ed.side"), (uint32_t)m); }
static OnSwitchMode EdActive() { return gOnSwitchMode == OnSwitchMode::TexSettings ? OnSwitchMode::UGC : gOnSwitchMode; }
// The places drawn by the 2.0 kit.
static bool EdOwnsMode(OnSwitchMode m) {
    return m == OnSwitchMode::WebUI || m == OnSwitchMode::Backups || m == OnSwitchMode::Phone || m == OnSwitchMode::Settings ||
           m == OnSwitchMode::Player || m == OnSwitchMode::UGC || m == OnSwitchMode::TexSettings || m == OnSwitchMode::MiiStats ||
           m == OnSwitchMode::Map;
}

static bool         sLeaveAsk = false;
static bool         sWarnAsk = false;
static OnSwitchMode sWarnTarget = OnSwitchMode::WebUI;
static double       sWarnAt = 0;
static Ui::Id       sEdLastFocus = 0;

static std::string IslandName() {
    static double sTry = -10;
    if (!gPlayerSav.loaded && Motion::Now() - sTry > 2.0) {
        sTry = Motion::Now();
        std::string e;
        SaveEditor::Load(SAVE_PLAYER_SAV, gPlayerSav, e);
    }
    static const uint32_t H_ISLAND = SaveEditor::Hash("Player.IslandName");
    return gPlayerSav.loaded ? SaveEditor::GetWStr32(gPlayerSav, H_ISLAND) : std::string();
}
static std::string PlayerName() {
    static const uint32_t H_PNAME = SaveEditor::Hash("Player.Name");
    return gPlayerSav.loaded ? SaveEditor::GetWStr32(gPlayerSav, H_PNAME) : std::string();
}
static std::string UserName() {
    return (gUserSel >= 0 && gUserSel < (int)gUsers.size()) ? gUsers[gUserSel].nickname : std::string();
}

// Go to a place of the editor. Miis and Player ask once whether the user
// knows what they are doing (the 1.x warning, now a dialog).
static void GoPage(OnSwitchMode m) {
    if ((m == OnSwitchMode::MiiStats || m == OnSwitchMode::Player) && !gSaveWarningAcked) {
        sWarnTarget = m;
        sWarnAsk = true;
        sWarnAt = Motion::Now();
        Motion::Forget(Ui::HashStr("ed.warn"));
        return;
    }
    if (gMapStroking && gOnSwitchMode == OnSwitchMode::Map) MapStrokeEnd();
    gOnSwitchMode = m;
    gOnSwitchMsg.clear();
}

// Saved with "Save now" since the island was opened (for the leave dialog:
// "Your changes are saved" only when something was).
static bool sEdSavedVisit = false;

// Write everything that is waiting, without leaving.
static void SaveNow() {
    HousingCleanupEmptyHouses();
    struct Item { SaveEditor::SavFile* sav; bool* dirty; const char* path; };
    const Item items[] = {
        { &gMiiSav,    &gMiiSavDirty,    SAVE_MII_SAV    },
        { &gPlayerSav, &gPlayerSavDirty, SAVE_PLAYER_SAV },
        { &gMapSav,    &gMapDirty,       SAVE_MAP_SAV    },
    };
    std::string err;
    for (const Item& it : items) {
        if (!*it.dirty) continue;
        if (it.sav->loaded) {
            err = SaveEditor::Save(it.path, *it.sav);
            if (err.empty()) err = SaveMount::Commit();
            if (!err.empty()) break;
        }
        *it.dirty = false;
    }
    if (err.empty()) err = SaveMount::Commit();
    if (!err.empty()) {
        gSaveErr = err;
        LogERR("Save failed: " + err);
        Sound::Play(Sound::Voice::Err);
        Ui::ToastShow(Tx("backprompt.failed") + " " + err, Ui::Tone::Bad, 6.f);
        return;
    }
    gUgcDirty = false; gWebUiDirty = false; gNeedCommit = false; gSaveErr.clear();
    gHomeCacheStale = true;
    sEdSavedVisit = true;
    History::Clear();          // saved: undo starts again from here
    LogOK(Tx("save.feedback.title"));
    Sound::Play(Sound::Voice::Done);
    Ui::ToastShow(Tx("save.feedback.body"), Ui::Tone::Good);
}

// ── Undo ─────────────────────────────────────────────────────────────────────
// The round button beside Save now. A press takes back the last change (the
// count on it says how many there are); held, a red ring fills around it as
// it grows and reddens, and when it closes every change since the last save
// is taken back. Highlighted, it says so in words.
static double sUndoHold = -1;          // when the press began (-1: none)
static bool   sUndoTouch = false;      // by a finger (else A)
static bool   sUndoAllDone = false;    // this hold already undid everything
static double sUndoAllAt = -10, sUndoOneAt = -10;
static int    sUndoTicks = 0;

// Whether a save in memory holds exactly what its file on the SD card does
// (then it has nothing to save, whatever its flag says).
static bool SameAsDisk(const SaveEditor::SavFile& mem, const char* path) {
    if (!mem.loaded) return true;
    SaveEditor::SavFile disk;
    std::string err;
    if (!SaveEditor::Load(path, disk, err) || disk.entries.size() != mem.entries.size()) return false;
    for (const auto& e : mem.entries) {
        auto it = disk.index.find(e.hash);
        if (it == disk.index.end() || it->second >= disk.entries.size()) return false;
        const auto& d = disk.entries[it->second];
        if (d.type != e.type || d.inlineRaw != e.inlineRaw || d.payload != e.payload) return false;
    }
    return true;
}

// What the pages keep beside the saves, after the saves went back.
static void EdAfterUndo(unsigned touched) {
    if (touched & 1u) {            // Mii.sav: the list, names, houses, row undos
        memset(gMiiUndoValid, 0, sizeof(gMiiUndoValid));
        gWordUndo.valid = false;
        std::vector<uint8_t> bytes;
        if (gMiiSav.loaded && SaveEditor::Serialize(gMiiSav, bytes).empty()) {
            std::vector<MiiManager::MiiSlot> list;
            if (MiiManager::ListMiisInBytes(bytes, list)) gMiis = list;
        }
        if (gMiiStatsMiiSel >= (int)gMiis.size()) gMiiStatsMiiSel = std::max(0, (int)gMiis.size() - 1);
        HousingRebuild();
    }
    if (touched & 2u) {            // Player.sav
        memset(gPlayerUndoValid, 0, sizeof(gPlayerUndoValid));
        gNameLangUndoValid = false; gIslandLangUndoValid = false; gPlayerStampUndoValid = false;
    }
    if (touched & 4u) {            // Map.sav: its own undo would now replay old steps
        gMapUndo.clear(); gMapRedo.clear(); gMapSelSlot = -1;
    }
    // Nothing left to undo: a save back to what is on the SD card has no
    // unsaved changes (its dot goes), even when a flag was set without one.
    if (!History::CanUndo()) {
        if (gMiiSavDirty && SameAsDisk(gMiiSav, SAVE_MII_SAV)) gMiiSavDirty = false;
        if (gPlayerSavDirty && SameAsDisk(gPlayerSav, SAVE_PLAYER_SAV)) gPlayerSavDirty = false;
        if (gMapDirty && SameAsDisk(gMapSav, SAVE_MAP_SAV)) gMapDirty = false;
    }
    gHomeCacheStale = true;
}

static void EdUndoOne() {
    const unsigned t = History::Undo();
    if (!t) return;
    EdAfterUndo(t);
    sUndoOneAt = Motion::Now();
    Sound::Play(Sound::Voice::Undo);
}

static void EdUndoAll() {
    const int n = History::Steps();
    unsigned t = 0;
    while (History::CanUndo()) t |= History::Undo();
    if (!t) return;
    EdAfterUndo(t);
    sUndoAllAt = Motion::Now();
    Sound::Play(Sound::Voice::Delete, { 0.5f, true });
    Sound::Play(Sound::Voice::Whoosh, { 0.5f, false, 0.8f, 60.f });
    Ui::ToastShow(TokenFill(Tx("undo.all.done"), "{n}", std::to_string(n)), Ui::Tone::Warn);
}

static void EdUndoButton(SDL_Rect r, SDL_Rect tipArea) {
    const Ui::Id id = Ui::HashStr("ed.undo");
    const Input::Frame& in = Input::Last();
    const double now = Motion::Now();
    const int steps = History::Steps();
    const bool can = steps > 0;
    Focus::Node n;
    n.id = id; n.r = r; n.radius = r.h / 2;
    n.flags = (uint16_t)(Focus::OwnSound | Focus::Ring);
    n.hint = can ? Tx("undo.hint") : std::string();
    n.press = []() {};                       // the hold below decides what a press does
    Focus::Add(std::move(n));
    const bool f = Focus::Is(id);
    auto inside = [&](float px, float py) { return px >= r.x - 6 && py >= r.y - 6 && px < r.x + r.w + 6 && py < r.y + r.h + 6; };
    if (can && sUndoHold < 0) {
        if (f && (in.down & Input::A)) { sUndoHold = now; sUndoTouch = false; sUndoAllDone = false; sUndoTicks = 0; }
        else if (in.pressed && inside(in.x, in.y)) { sUndoHold = now; sUndoTouch = true; sUndoAllDone = false; sUndoTicks = 0; }
    }
    constexpr double kGrace = 0.25, kFill = 1.1;    // a short press never starts the ring
    float p = 0.f;
    if (sUndoHold >= 0) {
        const bool still = sUndoTouch ? (in.touching && inside(in.x, in.y)) : ((in.held & Input::A) != 0);
        p = (float)std::max(0.0, std::min(1.0, (now - sUndoHold - kGrace) / kFill));
        if (still) {
            // Rising ticks while it fills, then everything at once.
            const int tick = (int)(p * 8.f);
            if (!sUndoAllDone && tick > sUndoTicks) { sUndoTicks = tick; Sound::Play(Sound::Voice::Tick, { 0.2f + 0.1f * tick, false, 0.6f }); }
            if (!sUndoAllDone && p >= 1.f) { sUndoAllDone = true; EdUndoAll(); }
        } else {
            const bool letGo = sUndoTouch ? in.released && inside(in.x, in.y) : true;
            if (!sUndoAllDone && letGo) {
                if (p <= 0.f) EdUndoOne();                        // a press: one step
                else Sound::Play(Sound::Voice::Release);          // let go half way: nothing
            }
            sUndoHold = -1;
            sUndoTicks = 0;
            p = 0.f;
        }
    }
    // What is shown follows the hold: up with it, then back down smoothly
    // (let go half way, or once everything is undone, even still held).
    static float sShown = 0.f;
    const float target = sUndoAllDone ? 0.f : p;
    if (target >= sShown || Motion::Reduced()) sShown = target;
    else sShown = std::max(target, sShown - Motion::Dt() * 4.f);
    const float ep = Motion::Reduced() ? sShown : Motion::Ease(Motion::Curve::Swift, sShown);
    // Done: a pop and a tick, shrinking back; one undo: a small dip.
    const float done = std::max(0.f, 1.f - (float)((now - sUndoAllAt) / 0.6));
    const float bump = std::max(0.f, 1.f - (float)((now - sUndoOneAt) / 0.25));
    const float pop = done > 0.f ? sinf(done * 3.14159265f) * 0.12f : 0.f;
    const float scale = 1.f + 0.16f * ep + pop - 0.06f * bump;
    const float cx = r.x + r.w / 2.f, cy = r.y + r.h / 2.f;
    const bool busy = sShown > 0.001f || done > 0.f;
    PushXform(0, 0, scale, cx, cy, can || busy ? 1.f : 0.45f);
    const SDL_Color red = gTheme.red;
    const SDL_Color base = Mix(gTheme.panel, gTheme.text, f ? 0.16f : 0.09f);
    const SDL_Color fill = Mix(base, red, std::min(1.f, ep * 1.1f + done * 0.8f));
    const float R = r.h / 2.f;
    FillDisc(cx, cy + 2.f, R, SDL_Color{ 0, 0, 0, 50 });
    FillDisc(cx, cy, R, fill);
    // The ring track says "hold me"; it fills red as you hold.
    const Uint8 track = can ? (f ? 70 : 38) : 18;
    StrokeArc(cx, cy, R + 5.f, 2.f, 0.f, 360.f, Alpha(gTheme.text, track), Alpha(gTheme.text, track));
    if (sShown > 0.001f) StrokeArc(cx, cy, R + 5.f, 3.f, -90.f, -90.f + 360.f * sShown, red, Mix(red, SDL_Color{ 255, 255, 255, 255 }, 0.25f));
    // The arrow (a tick for a moment once everything is undone).
    const SDL_Color ic = sShown > 0.001f || done > 0.f ? SDL_Color{ 255, 255, 255, 255 } : (can ? gTheme.text : gTheme.dim);
    UiIcons::Draw(done > 0.35f ? UiIcons::Id::Check : UiIcons::Id::Undo, (int)cx, (int)cy, 24, ic);
    PopXform();
    // How many changes there are, on its shoulder.
    if (can && sShown <= 0.001f) {
        const std::string cnt = steps > 99 ? "99+" : std::to_string(steps);
        const int bw = std::max(22, TextW(cnt, Font::Xs) + 12), bh = 20;
        const int bx = r.x + r.w - bw / 2 - 2, by = r.y - 6;
        FillRectRounded(bx, by, bw, bh, bh / 2, gTheme.gold);
        DrawTextC(cnt, bx + bw / 2, by + bh / 2, SDL_Color{ 20, 18, 26, 255 }, Font::Xs);
    }
    // In words, above (beside it when the sidebar is folded): what a press
    // does, and what holding does. Drawn after the page, so it stays on top.
    if (can && (f || sShown > 0.001f)) {
        const bool holding = sShown > 0.001f;
        const SDL_Rect a = tipArea;
        const float shown = sShown;
        sEdLate.push_back([a, holding, shown, ep, red, steps]() {
        const float sShown = shown;
        FillRectRounded(a.x, a.y + 3, a.w, a.h, 14, SDL_Color{ 0, 0, 0, 60 });
        FillRectRounded(a.x, a.y, a.w, a.h, 14, holding ? Mix(gTheme.panel, red, 0.35f + 0.4f * ep) : Alpha(gTheme.panel, 250));
        DrawRectRounded(a.x, a.y, a.w, a.h, 14, holding ? Alpha(red, 200) : Alpha(gTheme.border, 230));
        if (holding) {
            W::Marquee(Ui::HashStr("undo.tip.0"), Tx("undo.holding"), a.x + 12, a.y + a.h / 2 - 9, a.w - 24, gTheme.text, Font::Body, true, W::Anchor::Middle);
            FillRectRounded(a.x + 12, a.y + a.h - 18, a.w - 24, 6, 3, Alpha(gTheme.text, 40));
            FillRectRounded(a.x + 12, a.y + a.h - 18, (int)((a.w - 24) * sShown), 6, 3, red);
        } else {
            // A dot for a press, a ring for a hold, like the button.
            FillDisc((float)a.x + 20.f, (float)a.y + 18.f, 4.f, gTheme.text);
            W::Marquee(Ui::HashStr("undo.tip.1"), Tx("undo.tip.press"), a.x + 34, a.y + 18, a.w - 44, gTheme.text, Font::Xs, true, W::Anchor::Middle);
            StrokeArc((float)a.x + 20.f, (float)a.y + a.h - 18.f, 5.f, 2.f, 0.f, 360.f, red, red);
            W::Marquee(Ui::HashStr("undo.tip.2"), TokenFill(Tx("undo.tip.hold"), "{n}", std::to_string(steps)), a.x + 34, a.y + a.h - 18, a.w - 44, gTheme.text, Font::Xs, true, W::Anchor::Middle);
        }
        });
    }
}

// ── Sidebar ──────────────────────────────────────────────────────────────────
// The fold toggle: a round button at the top (right of the name; under the
// logo when folded).
static void EdFoldButton(SDL_Rect r) {
    const Ui::Id id = Ui::HashStr("ed.fold");
    Focus::Node n;
    n.id = id; n.r = { r.x - 6, r.y - 6, r.w + 12, r.h + 12 }; n.radius = r.h / 2 + 6;
    n.flags = (uint16_t)(Focus::Ring | Focus::OwnSound);
    n.hint = Tx(gSideFolded ? "side.open" : "side.fold");
    n.press = []() { EdSetFolded(!gSideFolded); };
    Focus::Add(std::move(n));
    W::BeginPress(id, r);
    const bool f = Focus::Is(id);
    const float cx = r.x + r.w / 2.f, cy = r.y + r.h / 2.f, R = r.w / 2.f;
    FillDisc(cx, cy, R, Mix(gTheme.panel, gTheme.text, f ? 0.16f : 0.07f));
    StrokeArc(cx, cy, R, 1.2f, 0.f, 360.f, Alpha(gTheme.text, f ? 70 : 30), Alpha(gTheme.text, f ? 70 : 30));
    UiIcons::Draw(gSideFolded ? UiIcons::Id::PanelOpen : UiIcons::Id::PanelClose, (int)cx, (int)cy, 20, f ? gTheme.text : gTheme.dim);
    W::EndPress();
}

// A folded item's name, in a bubble beside it while it has the highlight.
static void EdSideTip(const std::string& text, int cy) {
    if (text.empty()) return;
    sEdLate.push_back([text, cy]() {
        const int pad = 14, w = TextW(text, Font::Row) + pad * 2, h = 40;
        const int x = sEdSideW + 14, y = cy - h / 2;
        FillRectRounded(x, y + 3, w, h, 13, SDL_Color{ 0, 0, 0, 60 });
        FillRectRounded(x, y, w, h, 13, Alpha(gTheme.panel, 252));
        DrawRectRounded(x, y, w, h, 13, Alpha(gTheme.border, 230));
        DrawTextVC(text, x + pad, cy, gTheme.text, Font::Row);
    });
}

// Open: icons and words, 270 px. Folded: the icons alone, 88 px, the names in
// a bubble on the highlight. Everything glides from one to the other.
static void EdSidebar() {
    const float k = sEdFoldK;                              // 0 open .. 1 folded
    const bool folded = k > 0.5f;
    const float words = std::max(0.f, 1.f - 2.f * k);       // the names fade out first
    auto L = [k](int a, int b) { return (int)lroundf(a + (b - a) * k); };
    auto R = [&](SDL_Rect a, SDL_Rect b) { return SDL_Rect{ L(a.x, b.x), L(a.y, b.y), L(a.w, b.w), L(a.h, b.h) }; };
    const bool gliding = sEdFoldTw.Busy();
    FillRectSquare(0, 0, sEdSideW, SCREEN_H, Alpha(gTheme.panel, 120));
    FillRectSquare(sEdSideW - 1, 0, 1, SCREEN_H, Alpha(gTheme.border, 200));
    const SDL_Rect foldR = R({ kEdSideOpenW - 36, 22, 28, 28 }, { (kEdSideFoldW - 28) / 2, 74, 28, 28 });
    const std::string island = IslandName();
    Brand(24, 26, island.empty() ? UserName() : island, k, kEdSideFoldW / 2, foldR.x - 8);
    EdFoldButton(foldR);
    const int iwO = kEdSideOpenW - 36, iwF = kEdSideFoldW - 36;
    // The places: open from 124 and 410, 50 apart; folded from 112 and 372, 48 apart.
    auto placeRect = [&](int group, int i) {
        return R({ 18, (group == 0 ? 124 : 410) + i * 50, iwO, 46 }, { 18, (group == 0 ? 112 : 372) + i * 48, iwF, 46 });
    };
    const OnSwitchMode act = EdActive();
    SDL_Rect activeR{ 0, -1, 0, 0 };
    {
        int i = 0;
        for (const auto& p : kEdIsland) { if (p.mode == act) activeR = placeRect(0, i); i++; }
        i = 0;
        for (const auto& p : kEdMore) { if (p.mode == act) activeR = placeRect(1, i); i++; }
    }
    W::SidebarGroup(28, 98, Tx("ed.group.island"), 1.f - 2.5f * k);
    W::SidebarGroup(28, 384, Tx("ed.group.more"), 1.f - 2.5f * k);
    if (k > 0.5f) FillRectSquare(kEdSideFoldW / 2 - 18, L(397, 361), 36, 1, Alpha(gTheme.border, (int)(230 * (2.f * k - 1.f))));
    if (activeR.y >= 0) W::SidebarMarks(Ui::HashStr("ed.marks"), activeR, gliding);
    auto item = [&](const EdPlace& p, SDL_Rect r) {
        const OnSwitchMode m = p.mode;
        const bool dot = (m == OnSwitchMode::MiiStats && gMiiSavDirty) || (m == OnSwitchMode::Player && gPlayerSavDirty) ||
                         (m == OnSwitchMode::Map && gMapDirty) || (m == OnSwitchMode::UGC && gUgcDirty);
        // The highlight stays on the place chosen (tap or A); ▶ goes into it.
        W::SidebarItem(EdItemId(m), r, p.icon, Tx(p.key), m == act, dot, [m]() {
            if (m != EdActive()) GoPage(m);
            Focus::Set(EdItemId(m));
        }, words);
        if (folded && Focus::Is(EdItemId(m))) EdSideTip(Tx(p.key), r.y + r.h / 2);
    };
    {
        int i = 0;
        for (const auto& p : kEdIsland) item(p, placeRect(0, i++));
        i = 0;
        for (const auto& p : kEdMore) item(p, placeRect(1, i++));
    }
    // A save marked unsaved with nothing to undo is checked against its file
    // once (until a flag or the history changes): a flag set without a real
    // change loses its dot.
    {
        static unsigned sCheckedFor = ~0u;
        const unsigned state = (gMiiSavDirty ? 1u : 0u) | (gPlayerSavDirty ? 2u : 0u) | (gMapDirty ? 4u : 0u) | ((unsigned)History::Steps() << 3);
        if (state != sCheckedFor) {
            sCheckedFor = state;
            if (!History::CanUndo() && (state & 7u)) {
                if (gMiiSavDirty && SameAsDisk(gMiiSav, SAVE_MII_SAV)) gMiiSavDirty = false;
                if (gPlayerSavDirty && SameAsDisk(gPlayerSav, SAVE_PLAYER_SAV)) gPlayerSavDirty = false;
                if (gMapDirty && SameAsDisk(gMapSav, SAVE_MAP_SAV)) gMapDirty = false;
                sCheckedFor = (gMiiSavDirty ? 1u : 0u) | (gPlayerSavDirty ? 2u : 0u) | (gMapDirty ? 4u : 0u);
            }
        }
    }
    // Save now / all saved and Undo beside it (folded: Undo above it, both
    // round), then Leave.
    const bool unsaved = HasUnsavedChanges();
    const int sy = SCREEN_H - 24 - 46 - 58;
    const SDL_Rect saveR = R({ 18, sy, iwO - 62, 50 }, { (kEdSideFoldW - 50) / 2, 588, 50, 50 });
    const std::string saveText = Tx(unsaved ? "ed.save" : "home.saved");
    W::Button(Ui::HashStr("ed.save"), saveR, folded || saveR.w < 120 ? std::string() : saveText,
              unsaved ? W::Kind::Primary : W::Kind::Soft, []() { SaveNow(); }, unsaved ? UiIcons::Id::Floppy : UiIcons::Id::Check,
              unsaved, Focus::OwnSound);
    if (folded && Focus::Is(Ui::HashStr("ed.save"))) EdSideTip(saveText, saveR.y + saveR.h / 2);
    const SDL_Rect undoR = R({ 18 + iwO - 50, sy, 50, 50 }, { (kEdSideFoldW - 50) / 2, 528, 50, 50 });
    EdUndoButton(undoR, R({ 14, sy - 76, iwO + 8, 62 }, { kEdSideFoldW + 14, 528 - 6, 260, 62 }));
    const SDL_Rect leaveR = R({ 18, SCREEN_H - 24 - 46, iwO, 46 }, { 18, SCREEN_H - 24 - 46, iwF, 46 });
    W::SidebarItem(Ui::HashStr("ed.leave"), leaveR, UiIcons::Id::Exit, Tx("ed.leave"), false, false,
                   []() { sLeaveAsk = true; Motion::Forget(Ui::HashStr("ed.leave.dlg")); }, words);
    if (folded && Focus::Is(Ui::HashStr("ed.leave"))) EdSideTip(Tx("ed.leave"), leaveR.y + leaveR.h / 2);
    if (folded && Focus::Is(Ui::HashStr("ed.fold"))) EdSideTip(Tx("side.open"), foldR.y + foldR.h / 2);
}

static bool IsSidebarId(Ui::Id id) {
    for (int i = 0; i < kEdOrderN; i++) if (id == EdItemId(kEdOrder[i])) return true;
    return id == Ui::HashStr("ed.save") || id == Ui::HashStr("ed.leave") || id == Ui::HashStr("ed.undo") || id == Ui::HashStr("ed.fold");
}

// ── Dialogs ──────────────────────────────────────────────────────────────────
static void LeaveDialog() {
    const bool unsaved = HasUnsavedChanges();
    auto stay = []() { sLeaveAsk = false; };
    if (unsaved) {
        std::string body = Tx("leave.body.unsaved");
        if (!gSaveErr.empty()) body = Tx("backprompt.failed") + "\n" + gSaveErr;
        W::Dialog(Ui::HashStr("ed.leave.dlg"), UiIcons::Id::Exit, gSaveErr.empty() ? Ui::Tone::Warn : Ui::Tone::Bad,
                  Tx("leave.title.unsaved"), body,
                  { { Tx("leave.save"), W::Kind::Primary, []() {
                        gBackPromptToUserPick = true;
                        TAckBackYes(0);
                        if (gScreen == Screen::OnSwitch) { Sound::Play(Sound::Voice::Err); }
                        else sLeaveAsk = false;
                    } },
                    { Tx("leave.discard"), W::Kind::Danger, []() {
                        sLeaveAsk = false;
                        gBackPromptToUserPick = true;
                        Sound::Play(Sound::Voice::Delete);
                        TAckBackNo(0);
                    } },
                    { Tx("quit.stay"), W::Kind::Neutral, stay } },
                  0, stay, Tx("quit.stay"));
    } else {
        // Nothing waiting: say it was saved only if it was ("Everything is
        // saved" after changing nothing read as if something had happened).
        W::Dialog(Ui::HashStr("ed.leave.dlg2"), UiIcons::Id::Exit, Ui::Tone::Neutral, Tx("leave.title"),
                  sEdSavedVisit ? Tx("leave.body.saved") : std::string(),
                  { { Tx("quit.stay"), W::Kind::Neutral, stay },
                    { Tx("ed.leave"), W::Kind::Primary, []() { sLeaveAsk = false; LeaveEditor(); gScreen = Screen::UserPick; } } },
                  1, stay, Tx("quit.stay"));
    }
}

static void WarnDialog() {
    const double left = 2.0 - (Motion::Now() - sWarnAt);
    std::string ok = Tx("warn.understand");
    if (left > 0) ok += "  (" + std::to_string((int)ceil(left)) + ")";
    W::Dialog(Ui::HashStr("ed.warn"), UiIcons::Id::Alert, Ui::Tone::Warn, Tx("warn.title"),
              Tx("warn.body.l1") + "\n" + Tx("warn.body.l2b"),
              { { Tx("backups.title"), W::Kind::Neutral, []() { sWarnAsk = false; GoPage(OnSwitchMode::Backups); } },
                { ok, W::Kind::Primary, []() {
                    sWarnAsk = false;
                    gSaveWarningTarget = sWarnTarget;
                    TAckSaveWarning(0);
                }, left <= 0 } },
              1, []() { sWarnAsk = false; }, Tx("hint.cancel"));
}

static void MtpDialog() {
    W::Dialog(Ui::HashStr("ed.mtp"), UiIcons::Id::Usb, Ui::Tone::Warn, Tx("webui.mtp.warn.title"),
              Tx("webui.mtp.warn.l1") + "\n" + Tx("webui.mtp.warn.l2") + "\n" + Tx("webui.mtp.warn.l3"),
              { { Tx("webui.mtp.warn.no"), W::Kind::Neutral, []() { TAckMtpWarningNo(0); } },
                { Tx("webui.mtp.warn.yes"), W::Kind::Primary, []() { TAckMtpWarningYes(0); } } },
              0, []() { TAckMtpWarningNo(0); }, Tx("hint.cancel"));
}

// ── Pieces ───────────────────────────────────────────────────────────────────
// A figure that counts up when the page arrives, with what it counts and the
// game's picture for it on the right.
static void FigureTile(Ui::Id id, SDL_Rect r, Art::Id art, UiIcons::Id icon, int value, const std::string& suffix,
                       const std::string& label, std::function<void()> press, uint16_t flags = 0) {
    Focus::Node n;
    n.id = id; n.r = { r.x - 5, r.y - 5, r.w + 10, r.h + 10 }; n.radius = 24; n.flags = (uint16_t)(flags | Focus::Ring);
    n.press = std::move(press);
    n.hint = Tx("hint.open");
    Focus::Add(std::move(n));
    W::BeginPress(id, r);
    Focus::DrawPlate(id);
    W::Card(r);
    const bool f = Focus::Is(id);
    const float t = Motion::Reduced() ? 1.f : std::min(1.f, (float)((Motion::Now() - W::ArrivedAt() - 0.1) / 0.9));
    const int shown = (int)lroundf(value * Motion::Ease(Motion::Curve::QuintOut, std::max(0.f, t)));
    DrawText(std::to_string(shown) + suffix, r.x + 22, r.y + 16, gTheme.text, Font::Xl);
    W::Eyebrow(r.x + 22, r.y + r.h - 32, label, gTheme.faint);
    if (Art::Has(art)) {
        const int as = r.h - 22;
        const SDL_Rect ab{ r.x + r.w - 14 - as, r.y + 11, as, as };
        if (Art::IsMask(art)) Art::Draw(art, SDL_Rect{ ab.x + 14, ab.y + 14, ab.w - 28, ab.h - 28 }, f ? gTheme.gold : gTheme.dim);
        else Art::Draw(art, ab, SDL_Color{ 255, 255, 255, (Uint8)(f ? 255 : 225) });
    } else {
        UiIcons::Draw(icon, r.x + r.w - 34, r.y + 32, 24, f ? gTheme.gold : gTheme.dim);
    }
    W::EndPress();
}

// A pill with one of the game's icons in front of its words. Returns its width.
static int IconPill(int x, int y, Art::Id icon, const std::string& text) {
    const int h = 36, iw = Art::Has(icon) ? 22 : 0;
    const int w = 16 + iw + (iw ? 9 : 0) + TextW(text, Font::Body) + 18;
    FillRectRounded(x, y, w, h, h / 2, Alpha(gTheme.text, 16));
    int tx = x + 16;
    if (iw) { Art::Icon(icon, tx + iw / 2, y + h / 2, 24, gTheme.dim); tx += iw + 9; }
    DrawTextVC(text, tx, y + h / 2, gTheme.text, Font::Body);
    return w;
}

// page_map.inc: the island baked to a small picture (loads Map.sav if needed).
static SDL_Texture* MapThumb();

static int DrawQr(const std::string& url, int cx, int top, int size);
static std::string ServerUrl();
// The QR code big, for a phone across the room: B, A or a tap closes it.
static bool sQrBig = false;
static void QrBigSheet() {
    const Ui::Id id = Ui::HashStr("phone.qrbig");
    const double since = Motion::FirstSeen(id), now = Motion::Now();
    if (now - since < 0.0001) Sound::Play(Sound::Voice::Open);
    auto close = []() { sQrBig = false; Sound::Play(Sound::Voice::Release); Focus::Set(Ui::HashStr("phone.qr")); };
    const int w = 560, h = 640, x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
    Focus::PushLayer(Focus::Layer::Sheet, close, Tx("hint.close"), SDL_Rect{ x, y, w, h });
    DimScreen((int)(170 * std::min(1.f, (float)((now - since) / 0.16))));
    const float t = Motion::Reduced() ? 1.f : std::min(1.f, (float)((now - since) / 0.5));
    const float e = Motion::Reduced() ? 1.f : Motion::Ease(Motion::Curve::Jelly, t);
    PushXform(0, 10.f * (1 - e), 0.86f + 0.14f * e, SCREEN_W / 2.f, SCREEN_H / 2.f, std::min(1.f, t * 2.f));
    FillRectRounded(x, y + 12, w, h, 32, SDL_Color{ 0, 0, 0, 70 });
    FillRectRounded(x, y, w, h, 32, Mix(gTheme.panel, gTheme.text, 0.03f));
    StrokeRectRounded((float)x, (float)y, (float)w, (float)h, 32.f, 1.2f, Alpha(gTheme.border, 230));
    const std::string url = ServerUrl();
    DrawTextC(Tx("phone.qr.scan"), SCREEN_W / 2, y + 44, gTheme.dim, Font::Body);
    const int qh = DrawQr(url, SCREEN_W / 2, y + 74, w - 80);
    DrawTextC(url, SCREEN_W / 2, y + 74 + qh + 40, gTheme.gold, Font::Title);
    // The whole sheet is one button: anywhere closes it.
    Focus::Node n;
    n.id = Ui::HashStr("phone.qrbig.close"); n.r = { x, y, w, h }; n.radius = 32; n.flags = Focus::NoPlate | Focus::Default;
    n.hint = Tx("hint.close"); n.press = close;
    Focus::Add(std::move(n));
    PopXform();
    Focus::PopLayer();
}

// ── Overview ─────────────────────────────────────────────────────────────────
// A long list all at once, as a dense grid over the page (Miis, textures):
// A picks one and the page lands on it, B closes. `draw` paints a cell's
// content (its card and highlight are drawn here).
struct Overview {
    std::string title, sub;
    int count = 0, current = -1, cols = 8, cellH = 140;
    int below = -1;                    // >= 0: cells are square plus this much (a caption)
    std::function<void(int, SDL_Rect, bool, SDL_Color)> draw;   // cell, rect, highlighted, its colour
    std::function<void(int)> pick;
    std::function<void()> close;
};
static Ui::Id OverviewCellId(int i) { return Ui::IdMix(Ui::HashStr("ov.cell"), (uint32_t)i); }
static void OverviewSheet(const Overview& o) {
    const Ui::Id id = Ui::HashStr("ov.sheet");
    const double since = Motion::FirstSeen(id), now = Motion::Now();
    static Focus::Scroll sc;
    if (now - since < 0.0001) { Sound::Play(Sound::Voice::Open); sc = Focus::Scroll{}; }
    const int x = 36, y = 26, w = SCREEN_W - 72, h = SCREEN_H - 52 - 26;
    Focus::PushLayer(Focus::Layer::Sheet, o.close, Tx("hint.close"), SDL_Rect{ x, y, w, h });
    DimScreen((int)(160 * std::min(1.f, (float)((now - since) / 0.16))));
    const float e = Motion::Reduced() ? 1.f : Motion::Ease(Motion::Curve::Spring, std::min(1.f, (float)((now - since) / 0.42)));
    PushXform(0, 18.f * (1 - e), 0.97f + 0.03f * e, SCREEN_W / 2.f, SCREEN_H / 2.f, std::min(1.f, e * 1.4f));
    FillRectRounded(x, y + 12, w, h, 30, SDL_Color{ 0, 0, 0, 70 });
    FillRectRounded(x, y, w, h, 30, Mix(gTheme.panel, gTheme.text, 0.03f));
    DrawRectRounded(x, y, w, h, 30, Alpha(gTheme.border, 230));
    W::IconTile(x + 26, y + 22, 52, UiIcons::Id::Grid, gTheme.gold);
    W::Marquee(Ui::HashStr("ov.title"), o.title, x + 94, y + 20, w - 260, gTheme.text, Font::Title, true);
    W::Marquee(Ui::HashStr("ov.sub"), o.sub, x + 94, y + 20 + TextH(Font::Title), w - 260, gTheme.dim, Font::Body, true);
    W::Button(Ui::HashStr("ov.close"), { x + w - 26 - 48, y + 24, 48, 48 }, std::string(), W::Kind::Neutral,
              [c = o.close]() { Sound::Play(Sound::Voice::Release); c(); }, UiIcons::Id::Close, true, Focus::OwnSound);
    const SDL_Rect view{ x + 8, y + 92, w - 16, h - 92 - 8 };
    const int pad = 14, gap = 12;
    const int cw = (view.w - 2 * pad - (o.cols - 1) * gap) / o.cols;
    const int ch = o.below >= 0 ? cw + o.below : o.cellH;
    const int left = view.x + (view.w - (o.cols * cw + (o.cols - 1) * gap)) / 2;
    Focus::BeginScroll(Ui::HashStr("ov.grid"), view, sc);
    for (int i = 0; i < o.count; i++) {
        const int row = i / o.cols, col = i % o.cols;
        const SDL_Rect r{ left + col * (cw + gap), view.y + pad + row * (ch + gap) - (int)sc.offset, cw, ch };
        const Ui::Id cid = OverviewCellId(i);
        Focus::Node n;
        n.id = cid; n.r = { r.x - 4, r.y - 4, r.w + 8, r.h + 8 }; n.radius = 20; n.hint = Tx("hint.select");
        n.flags = (uint16_t)(Focus::Ring | (i == std::max(0, o.current) ? Focus::Default : 0));
        n.press = [p = o.pick, c = o.close, i]() { c(); p(i); };
        Focus::Add(std::move(n));
        if (r.y + r.h < view.y - 20 || r.y > view.y + view.h + 20) continue;
        // Rows arrive one after another near the top.
        const float at = Motion::Reduced() ? 1.f : std::max(0.f, std::min(1.f, (float)((now - since - 0.06 - std::min(row, 5) * 0.04) / 0.36)));
        PushXform(0, 10.f * (1 - Motion::Ease(Motion::Curve::Spring, at)), 1, 0, 0, std::min(1.f, at * 1.6f));
        W::BeginPress(cid, r);
        Focus::DrawPlate(cid);
        const bool f = Focus::Is(cid);
        const SDL_Color face = Mix(gTheme.panel, gTheme.text, f ? 0.08f : (i == o.current ? 0.06f : 0.03f));
        FillRectRounded(r.x, r.y, r.w, r.h, 16, face);
        if (o.draw) o.draw(i, r, f, face);
        if (i == o.current) DrawRectRounded(r.x, r.y, r.w, r.h, 16, Alpha(gTheme.gold, 140));
        W::EndPress();
        PopXform();
    }
    Focus::EndScroll((float)(2 * pad + ((o.count + o.cols - 1) / o.cols) * (ch + gap) - gap));
    PopXform();
    Focus::PopLayer();
}

// The QR code of an address, `size` pixels wide at most. Returns its height.
static int DrawQr(const std::string& url, int cx, int top, int size) {
    static std::string sUrl;
    static QR::Mat sQr;
    static bool sOk = false;
    if (url != sUrl) { sUrl = url; sOk = QR::build(url, sQr); }
    if (!sOk) return 0;
    int ms = size / (sQr.N + 4);
    if (ms < 2) ms = 2;
    const int qs = (sQr.N + 4) * ms, qx = cx - qs / 2;
    FillRectRounded(qx, top, qs, qs, 16, SDL_Color{ 255, 255, 255, 255 });
    SDL_SetRenderDrawBlendMode(gRen, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(gRen, 14, 14, 20, 255);
    for (int qy = 0; qy < sQr.N; qy++)
        for (int qx2 = 0; qx2 < sQr.N; qx2++)
            if (sQr.get(qx2, qy)) {
                const SDL_Rect r = XformRect(qx + (qx2 + 2) * ms, top + (qy + 2) * ms, ms, ms);
                SDL_RenderFillRect(gRen, &r);
            }
    return qs;
}

static std::string ServerUrl() { return "http://" + gIP + ":" + std::to_string(HTTP_PORT); }
static bool HasWifi() { return !gIP.empty() && gIP != "?.?.?.?"; }

static void ToggleServer(bool on) {
    if (!on) { HttpServer::Stop(); LogINF("WebUI stopped"); return; }
    gIP = SaveMount::GetLocalIP();
    if (gIP.empty()) gIP = "?.?.?.?";
    HttpServer::Start(HTTP_PORT, SAVE_UGC_PATH);
    HttpServer::SetSaveWarnAcked(gSaveWarningAcked);
    LogOK("WebUI started");
}

// Left-aligned wrapped text; returns the height used.
static int TextLines(const std::string& s, int x, int y, int w, SDL_Color c, Font f, int maxLines) {
    const int lh = TextH(f);
    int n = 0;
    for (const std::string& l : Ui::WrapLines(s, f, w, maxLines)) { DrawText(l, x, y + n * lh, c, f); ++n; }
    return n * lh;
}

static void ActivityCard(SDL_Rect r) {
    W::BeginBlock(r);
    W::Card(r);
    W::Eyebrow(r.x + 22, r.y + 16, Tx("home.activity"), gTheme.faint);
    const int lineH = TextH(Font::Body) + 3;
    int ly = r.y + 40;
    const int maxLines = std::max(1, (r.y + r.h - 14 - ly) / lineH);
    if (gLog.empty()) DrawText(Tx("webui.log.empty"), r.x + 22, ly, gTheme.faint, Font::Body);
    for (int i = std::max(0, (int)gLog.size() - maxLines); i < (int)gLog.size(); i++, ly += lineH) {
        FillDisc((float)(r.x + 26), (float)(ly + lineH / 2), 3.f, gLog[i].col);
        W::Marquee(Ui::IdMix(Ui::HashStr("home.log"), (uint32_t)i), gLog[i].text, r.x + 38, ly, r.w - 60, gTheme.dim, Font::Body, true);
    }
    W::EndBlock();
}

// ── Home ─────────────────────────────────────────────────────────────────────
static void PageHome() {
    if (gHomeCacheStale) HomeRefreshCache();
    const std::string island = IslandName();
    std::string who = PlayerName();
    const std::string user = UserName();
    if (!user.empty()) who += (who.empty() ? "" : "  \xC2\xB7  ") + user;
    const bool unsaved = HasUnsavedChanges();
    const std::string st = Tx(unsaved ? "status.unsaved" : "home.saved");
    int y = 34;
    // The hero: the user's picture, the island's name big, who plays it, and
    // the island itself on the right (its own map, fading into the card).
    const SDL_Rect hero{ sEdX, y, sEdW, 180 };
    W::BeginBlock(hero);
    {
        W::Card(hero);
        const SDL_Color cardC = Mix(gTheme.bg, gTheme.panel, 0.87f);
        int mapX = hero.x + hero.w;
        if (SDL_Texture* mt = MapThumb()) {
            const int mh = hero.h - 24, mw = mh * 3 / 2;                     // the island is 120 × 80 tiles
            const SDL_Rect mr{ hero.x + hero.w - 12 - mw, hero.y + 12, mw, mh };
            DrawTexture(mt, nullptr, mr, 230);
            const int fw = mw / 2, strips = 24;
            for (int i = 0; i < strips; i++) {
                const float k = 1.f - (float)i / strips;
                FillRectSquare(mr.x + i * fw / strips, mr.y, fw / strips + 1, mr.h, Alpha(cardC, (int)(255 * k * k)));
            }
            MaskCorners(mr.x, mr.y, mr.w, mr.h, 16, cardC);
            mapX = mr.x + fw / 2;
        }
        SDL_Texture* tex = (gUserSel >= 0 && gUserSel < (int)gAvatarTextures.size()) ? gAvatarTextures[gUserSel] : nullptr;
        const int av = 120, ax = hero.x + 26, ay = hero.y + (hero.h - av) / 2;
        if (tex) {
            DrawTexture(tex, nullptr, SDL_Rect{ ax, ay, av, av });
            MaskCorners(ax, ay, av, av, 30, cardC);
        } else {
            W::IconTile(ax, ay, av, UiIcons::Id::Home, gTheme.gold);
        }
        StrokeRectRounded((float)ax - 4.f, (float)ay - 4.f, (float)av + 8.f, (float)av + 8.f, 34.f, 3.f, Alpha(gTheme.gold, 190));
        const int tx = ax + av + 28, tw = mapX - tx - 16;
        W::Eyebrow(tx, ay + 2, Tx("home.eyebrow.island"), gTheme.faint);
        W::Marquee(Ui::HashStr("home.island"), island.empty() ? Tx("common.noname") : island, tx, ay + 18, tw, gTheme.text, Font::Xl, true);
        int px = tx;
        const int py = ay + av - 36;
        const std::string player = PlayerName();
        if (!player.empty()) px += IconPill(px, py, Art::Id::IcoPlayer, player) + 10;
        if (!user.empty()) px += IconPill(px, py, Art::Id::IcoFace, user) + 10;
        W::Pill(px, py + 4, st, unsaved ? Ui::Tone::Warn : Ui::Tone::Good, true);
        if (gUpdateCheck && Updater::GetState() == Updater::State::UpdateAvailable) {
            // On the label's line, after it, with room between them.
            const std::string up = TokenFill(Tx("home.update"), "{v}", Updater::GetLatestVersion());
            const int ew = TextW(UpperUtf8(Tx("home.eyebrow.island")), Font::Xs);
            const int ux = std::min(tx + ew + 16, tx + tw - W::PillW(up, true));
            W::Pill(std::max(tx + ew + 16, ux), ay - 4, up, Ui::Tone::Second, true);
        }
        (void)who;
    }
    W::EndBlock();
    y += hero.h + 20;
    // Figures: each opens its place.
    {
        const int gap = 18, w = (sEdW - 2 * gap) / 3, h = 116;
        const SDL_Rect r0{ sEdX, y, w, h }, r1{ sEdX + w + gap, y, w, h }, r2{ sEdX + 2 * (w + gap), y, w, h };
        W::BeginBlock(r0);
        FigureTile(Ui::HashStr("home.miis"), r0, Art::Id::ArtMii, UiIcons::Id::Users, (int)gMiis.size(), "", Tx("home.stat.miis"),
                   []() { GoPage(OnSwitchMode::MiiStats); }, Focus::Default);
        W::EndBlock();
        W::BeginBlock(r1);
        FigureTile(Ui::HashStr("home.tex"), r1, Art::Id::ArtAtelier, UiIcons::Id::Image, (int)gEntries.size(), "", Tx("home.stat.textures"),
                   []() { GoPage(OnSwitchMode::UGC); });
        W::EndBlock();
        W::BeginBlock(r2);
        FigureTile(Ui::HashStr("home.bk"), r2, Art::Id::IcoBackups, UiIcons::Id::Clock, gHomeBackupCount, " / " + std::to_string(gMaxBackups),
                   Tx("home.stat.backups"), []() { GoPage(OnSwitchMode::Backups); });
        W::EndBlock();
        y += h + 20;
    }
    // Backups and Phone & PC, side by side.
    {
        const int gap = 18, w = (sEdW - gap) / 2, h = 226;
        const SDL_Rect a{ sEdX, y, w, h }, b{ sEdX + w + gap, y, w, h };
        W::BeginBlock(a);
        W::Card(a);
        W::Eyebrow(a.x + 22, a.y + 18, Tx("home.backups.title"), gTheme.faint);
        const std::string last = gHomeLastBackup.empty() ? Tx("home.backups.none")
                                                         : TokenFill(Tx("home.backups.last"), "{date}", gHomeLastBackup);
        W::Marquee(Ui::HashStr("home.lastbk"), last, a.x + 22, a.y + 42, a.w - 44, gTheme.text, Font::Row, true);
        TextLines(Tx("home.backups.where"), a.x + 22, a.y + 84, a.w - 44, gTheme.faint, Font::Sm, 2);
        const int bw = (a.w - 44 - 12) / 2, by = a.y + a.h - 22 - 52;
        W::Button(Ui::HashStr("home.backupnow"), { a.x + 22, by, bw, 52 }, Tx("home.backup.now"), W::Kind::Primary, []() {
            if (HasUnsavedChanges()) { Sound::Play(Sound::Voice::Info); Ui::ToastShow(Tx("home.backup.need_save"), Ui::Tone::Warn, 3.5f); return; }
            HomeRun(HOME_BACKUP);
        }, UiIcons::Id::Floppy);
        W::Button(Ui::HashStr("home.allbk"), { a.x + 22 + bw + 12, by, bw, 52 }, Tx("home.backup.all"), W::Kind::Neutral,
                  []() { GoPage(OnSwitchMode::Backups); }, UiIcons::Id::Clock);
        W::EndBlock();

        W::BeginBlock(b);
        W::Card(b);
        W::Eyebrow(b.x + 22, b.y + 18, Tx("tab.phone"), gTheme.faint);
        const bool srv = HttpServer::IsRunning();
        const std::string ps = Tx(srv ? "home.web.on" : "home.web.off");
        W::Pill(b.x + b.w - 22 - W::PillW(ps, true), b.y + 14, ps, srv ? Ui::Tone::Good : Ui::Tone::Neutral, true);
        if (srv && HasWifi()) {
            W::Marquee(Ui::HashStr("home.url"), ServerUrl(), b.x + 22, b.y + 46, b.w - 44, gTheme.gold, Font::Title, true);
            TextLines(Tx("home.web.how"), b.x + 22, b.y + 88, b.w - 44, gTheme.dim, Font::Sm, 2);
        } else {
            W::Marquee(Ui::HashStr("home.webstate"), Tx(HasWifi() ? "home.web.stopped" : "home.web.nowifi"), b.x + 22, b.y + 46, b.w - 44, gTheme.text, Font::Row, true);
            TextLines(Tx(HasWifi() ? "home.web.stopped2" : "home.web.nowifi2"), b.x + 22, b.y + 84, b.w - 44, gTheme.dim, Font::Sm, 2);
        }
        W::Button(Ui::HashStr("home.phone"), { b.x + 22, b.y + b.h - 22 - 52, b.w - 44, 52 }, Tx("home.phone.open"), W::Kind::Neutral,
                  []() { GoPage(OnSwitchMode::Phone); }, UiIcons::Id::Phone);
        W::EndBlock();
        y += h + 22;
    }
    // Recent activity lives on Phone & PC, where it comes from.
}

// ── Backups ──────────────────────────────────────────────────────────────────
static Ui::Id EdBkRowId(int i) { return Ui::IdMix(Ui::HashStr("edbk.row"), (uint32_t)i); }
static std::vector<std::string> sEdBkList;
static double sEdBkAt = -10;
static int sEdBkMenu = -1, sEdBkAsk = 0, sEdBkAskIdx = -1;

static void EdBackupsRefresh() {
    sEdBkList.clear();
    if (gUserSel >= 0 && gUserSel < (int)gUsers.size())
        sEdBkList = BackupService::ListBackups(SaveMount::SafeUserFolder(gUsers[gUserSel]));
    BackupService::RequestInfo(sEdBkList);
    sEdBkAt = Motion::Now();
}

// Restore inside the editor: close the island, put the backup back, open it again.
static void EdRestore(int idx) {
    sEdBkAsk = 0; sEdBkMenu = -1;
    if (idx < 0 || idx >= (int)sEdBkList.size()) return;
    if (HasUnsavedChanges()) { Sound::Play(Sound::Voice::Info); Ui::ToastShow(Tx("home.backup.need_save"), Ui::Tone::Warn, 3.5f); return; }
    const int u = gUserSel;
    if (u < 0 || u >= (int)gUsers.size()) return;
    const std::string path = sEdBkList[idx];
    BusyFrame(Tx("restore.progress.title"), Tx("restore.progress.wait"));
    LeaveEditor();
    std::string err = SaveMount::Mount(gUsers[u].uid);
    if (!err.empty()) { gError = Tx("restore.toast.mount_fail") + " " + err; gScreen = Screen::Error; return; }
    const std::string rerr = BackupService::RestoreBackup(path, "tomodata:/");
    SaveMount::Commit();
    // Reopen on the restored save (the next frame scans it again).
    gScreen = Screen::OnSwitch;
    gOnSwitchMode = OnSwitchMode::Backups;
    Sound::Play(rerr.empty() ? Sound::Voice::Done : Sound::Voice::Err);
    if (rerr.empty()) Ui::ToastShow(Tx("restore.toast.ok"), Ui::Tone::Good, 4.f);
    else              Ui::ToastShow(Tx("restore.toast.fail") + " " + rerr, Ui::Tone::Bad, 6.f);
    gHomeCacheStale = true;
    sEdBkAt = -10;
}

static void EdDeleteBackup(int idx) {
    sEdBkAsk = 0; sEdBkMenu = -1;
    if (idx < 0 || idx >= (int)sEdBkList.size()) return;
    BackupService::ForgetInfo(sEdBkList[idx]);
    if (!BackupService::DeleteBackupAt(sEdBkList[idx])) { Sound::Play(Sound::Voice::Err); return; }
    Sound::Play(Sound::Voice::Delete);
    Ui::ToastShow(Tx("restore.toast.deleted"), Ui::Tone::Good);
    EdBackupsRefresh();
    gHomeCacheStale = true;
}

static void PageBackups() {
    if (Motion::Now() - sEdBkAt > 3.0) EdBackupsRefresh();
    const int n = (int)sEdBkList.size();
    W::BeginBlock({ sEdX, 34, sEdW, 90 });
    const int hy = W::PageHeader(sEdX, 34, sEdW, UiIcons::Id::Clock, IslandName(), Tx("backups.title"),
                                 TokenFill(Tx("backups.desc"), "{max}", std::to_string(gMaxBackups)));
    W::EndBlock();
    // The strip: how many are kept, the room they take, the last one's age,
    // and Back up now.
    const SDL_Rect strip{ sEdX, hy, sEdW, 96 };
    {
        W::BeginBlock(strip);
        W::Card(strip);
        const int bw = 250, bh = 54;
        const SDL_Rect br{ strip.x + strip.w - 21 - bw, strip.y + (strip.h - bh) / 2, bw, bh };
        W::Button(Ui::HashStr("edbk.now"), br, Tx("home.backup.now"), W::Kind::Primary, []() {
            if (HasUnsavedChanges()) { Sound::Play(Sound::Voice::Info); Ui::ToastShow(Tx("home.backup.need_save"), Ui::Tone::Warn, 3.5f); return; }
            sEdBkAt = -10;
            HomeRun(HOME_BACKUP);
        }, UiIcons::Id::Floppy, true, Focus::Default);
        uint64_t total = 0;
        bool all = true;
        for (const std::string& p : sEdBkList) {
            BackupService::BackupInfo bi;
            if (BackupService::GetInfo(p, bi)) total += bi.bytes; else all = false;
        }
        std::string last = Tx("bk.never");
        if (n > 0) {
            last = BackupAge(ParseBackupName(sEdBkList[0]));
            if (last.empty()) last = BackupWhen(sEdBkList[0]);
        }
        const std::string kept = TokenFill(TokenFill(Tx("bk.kept"), "{n}", std::to_string(n)), "{max}", std::to_string(gMaxBackups));
        const std::string labels[3] = { Tx("bk.stat.kept"), Tx("bk.stat.size"), Tx("bk.stat.last") };
        const std::string values[3] = { kept, all ? SizeText(total) : std::string(), last };
        const int cellW = (br.x - 12 - strip.x) / 3;
        for (int c = 0; c < 3; c++) {
            const int cx = strip.x + c * cellW;
            if (c > 0) FillRectSquare(cx, strip.y + 24, 1, strip.h - 48, Alpha(gTheme.border, 170));
            W::Eyebrow(cx + 24, strip.y + 20, labels[c], gTheme.faint);
            const int vy = strip.y + 40;
            if (values[c].empty()) { W::Spinner(cx + 24 + 11, vy + TextH(Font::Title) / 2, 10, gTheme.dim); continue; }
            const std::string v = W::Ellipsize(values[c], Font::Title, cellW - 40);   // (its width places the pips)
            W::Marquee(Ui::IdMix(Ui::HashStr("bk.stat"), (uint32_t)c), values[c], cx + 24, vy, cellW - 40, gTheme.text, Font::Title, true);
            // Kept: one pip per place, filled for the backups there are.
            if (c == 0 && gMaxBackups <= 10) {
                int px = cx + 24 + TextW(v, Font::Title) + 16;
                if (px + gMaxBackups * 15 <= cx + cellW - 12) {
                    const float py = (float)(vy + TextH(Font::Title) / 2);
                    for (int k = 0; k < gMaxBackups; k++, px += 15) {
                        if (k < n) FillDisc((float)px, py, 5.f, gTheme.gold);
                        else { FillDisc((float)px, py, 5.f, Alpha(gTheme.text, 60)); FillDisc((float)px, py, 3.2f, Mix(gTheme.panel, gTheme.bg, 0.3f)); }
                    }
                }
            }
        }
        W::EndBlock();
    }
    // The timeline: newest first, a rail with a stop for each backup.
    const int top = strip.y + strip.h + 10;
    const SDL_Rect view{ sEdX - 12, top, sEdW + 24, SCREEN_H - 60 - top };
    if (n == 0) {
        const SDL_Rect card{ sEdX, top + 12, sEdW, view.h - 24 };
        W::BeginBlock(card);
        W::Card(card);
        const int cx = sEdX + sEdW / 2, cy = card.y + card.h / 2 - 36;
        FillDisc((float)cx, (float)(cy - 22), 52.f, Alpha(gTheme.gold, 26));
        if (!Art::Icon(Art::Id::IcoBackups, cx, cy - 22, 64, gTheme.gold)) UiIcons::Draw(UiIcons::Id::Clock, cx, cy - 22, 44, gTheme.gold);
        DrawTextC(Tx("restore.empty.title"), cx, cy + 58, gTheme.text, Font::Title);
        Ui::TextBlockC(Tx("home.backups.none") + " " + Tx("home.backups.where"), cx, cy + 84, sEdW - 160, gTheme.dim, Font::Body, 2);
        W::EndBlock();
    } else {
        static Focus::Scroll sc;
        Focus::BeginScroll(Ui::HashStr("edbk.list"), view, sc);
        const int cardH = 112, step = cardH + 14, pad = 12;
        const int railX = sEdX + 13, cardX = sEdX + 40, cardW = sEdW - 40;
        const int y0 = view.y + pad - (int)sc.offset;
        if (n > 1) FillRectRounded(railX - 1, y0 + cardH / 2, 3, (n - 1) * step, 1, Alpha(gTheme.text, 30));
        for (int i = 0; i < n; i++) {
            const SDL_Rect r{ cardX, y0 + i * step, cardW, cardH };
            const bool vis = !(r.y + r.h < view.y || r.y > view.y + view.h);
            if (vis) W::BeginBlock(r);
            const int idx = i;
            BackupCard(EdBkRowId(i), r, sEdBkList[i], i, n, vis, [idx]() { sEdBkMenu = idx; Motion::Forget(Ui::HashStr("edbk.menu")); });
            if (vis) {
                // The stop on the rail: the newest one lit.
                const float dcy = (float)(r.y + r.h / 2), dcx = (float)railX + 0.5f;
                if (i == 0) { FillDisc(dcx, dcy, 12.f, Alpha(gTheme.gold, 46)); FillDisc(dcx, dcy, 6.5f, gTheme.gold); }
                else { FillDisc(dcx, dcy, 6.5f, Mix(gTheme.bg, gTheme.text, Focus::Is(EdBkRowId(i)) ? 0.7f : 0.32f)); FillDisc(dcx, dcy, 3.5f, gTheme.bg); }
                W::EndBlock();
            }
        }
        Focus::EndScroll((float)(2 * pad + n * step - 14));
    }
    if (sEdBkMenu >= 0 && sEdBkMenu < n) {
        const int b = sEdBkMenu;
        std::vector<W::MenuItem> items;
        items.push_back({ Tx("restore.menu.restore"), Tx("restore.confirm.restore.l1"), UiIcons::Id::Undo, false,
                          [b]() { sEdBkMenu = -1; sEdBkAsk = 1; sEdBkAskIdx = b; Motion::Forget(Ui::HashStr("edbk.ask")); }, false });
        items.push_back({ Tx("bk.details"), Tx("bk.details.sub"), UiIcons::Id::Info, false,
                          [b]() { sEdBkMenu = -1; sEdBkAsk = 3; sEdBkAskIdx = b; Motion::Forget(Ui::HashStr("edbk.info")); }, false });
        items.push_back({ Tx("restore.menu.delete"), Tx("restore.confirm.delete.l1"), UiIcons::Id::Trash, true,
                          [b]() { sEdBkMenu = -1; sEdBkAsk = 2; sEdBkAskIdx = b; Motion::Forget(Ui::HashStr("edbk.ask")); }, false });
        W::Menu(Ui::HashStr("edbk.menu"), BackupWhen(sEdBkList[b]), items, []() { sEdBkMenu = -1; }, Tx("hint.close"), 0,
                Focus::RectOf(EdBkRowId(b)));
    }
    if (sEdBkAsk == 3 && sEdBkAskIdx >= 0 && sEdBkAskIdx < n) {
        const int b = sEdBkAskIdx;
        BackupDetailsDialog(Ui::HashStr("edbk.info"), sEdBkList[b], [b]() { sEdBkAsk = 0; Focus::Set(EdBkRowId(b)); });
    } else if (sEdBkAsk && sEdBkAskIdx >= 0 && sEdBkAskIdx < n) {
        std::string tm;
        const std::string when = BackupWhen(sEdBkList[sEdBkAskIdx], &tm) + (tm.empty() ? "" : "  \xC2\xB7  " + tm);
        const bool del = sEdBkAsk == 2;
        const int idx = sEdBkAskIdx;
        W::Dialog(Ui::HashStr("edbk.ask"), del ? UiIcons::Id::Trash : UiIcons::Id::Undo, del ? Ui::Tone::Bad : Ui::Tone::Accent,
                  Tx(del ? "restore.confirm.delete.title" : "restore.confirm.restore.title"),
                  when + "\n" + Tx(del ? "restore.confirm.delete.l1" : "restore.confirm.restore.l1") + " " +
                      Tx(del ? "restore.confirm.delete.l2" : "restore.confirm.restore.l2"),
                  { { Tx("picker.cancel"), W::Kind::Neutral, []() { sEdBkAsk = 0; } },
                    { Tx(del ? "restore.chip.delete" : "restore.chip.restore"), del ? W::Kind::Danger : W::Kind::Primary,
                      [idx, del]() { if (del) EdDeleteBackup(idx); else EdRestore(idx); } } },
                  0, []() { sEdBkAsk = 0; }, Tx("hint.cancel"));
    }
}

// ── Phone & PC ───────────────────────────────────────────────────────────────
static void PagePhone() {
    W::BeginBlock({ sEdX, 34, sEdW, 90 });
    const int hy = W::PageHeader(sEdX, 34, sEdW, UiIcons::Id::Phone, IslandName(), Tx("tab.phone"), Tx("phone.desc"));
    W::EndBlock();
    const int gap = 18, lw = 548, rw = sEdW - lw - gap;
    const int bottom = SCREEN_H - 66;
    // The browser: the server, whether it starts by itself, its address and code.
    {
        const SDL_Rect c{ sEdX, hy, lw, bottom - hy };
        W::BeginBlock(c);
        W::Card(c);
        const bool srv = HttpServer::IsRunning();
        W::RowToggle(Ui::HashStr("ph.server"), { c.x + 6, c.y + 6, c.w - 12, W::kRowH }, { Tx("phone.server"), Tx("home.web.title") },
                     srv, [](bool v) { ToggleServer(v); });
        W::Hairline(c.x + 24, c.y + 6 + W::kRowH, c.w - 48);
        W::RowToggle(Ui::HashStr("ph.auto"), { c.x + 6, c.y + 7 + W::kRowH, c.w - 12, W::kRowH },
                     { Tx("phone.auto"), Tx("phone.auto.desc") }, gWebUiEnabled, [](bool v) { gWebUiEnabled = v; SaveConfig(); });
        W::Hairline(c.x + 24, c.y + 7 + 2 * W::kRowH, c.w - 48);
        const int midTop = c.y + 8 + 2 * W::kRowH + 16;
        if (srv && HasWifi()) {
            const std::string url = ServerUrl();
            const int uw = std::min(c.w - 48, TextW(url, Font::Title) + 48);
            FillRectRounded(c.x + (c.w - uw) / 2, midTop, uw, 50, 16, Alpha(gTheme.bg, 160));
            DrawRectRounded(c.x + (c.w - uw) / 2, midTop, uw, 50, 16, Alpha(gTheme.border, 200));
            DrawTextC(url, c.x + c.w / 2, midTop + 25, gTheme.gold, Font::Title);
            const int qTop = midTop + 50 + 16;
            const int textH = TextH(Font::Body) + TextH(Font::Xs) + 30;
            // The code itself can be pressed to see it big (easier to scan).
            const int qMax = std::min(240, c.y + c.h - 14 - textH - qTop);
            const int qh = DrawQr(url, c.x + c.w / 2, qTop, qMax);
            {
                const Ui::Id qid = Ui::HashStr("phone.qr");
                Focus::Node n;
                n.id = qid; n.r = { c.x + c.w / 2 - qh / 2 - 5, qTop - 5, qh + 10, qh + 10 }; n.radius = 21;
                n.flags = Focus::Ring; n.hint = Tx("phone.qr.big");
                n.press = []() { sQrBig = true; Motion::Forget(Ui::HashStr("phone.qrbig")); };
                Focus::Add(std::move(n));
                Focus::DrawPlate(qid);
                // A badge on the corner says it opens (for touch), on the
                // code's blank margin so it never hides a module.
                const bool qf = Focus::Is(qid);
                const float bx = (float)(c.x + c.w / 2 + qh / 2 + 1), by = (float)(qTop + qh + 1);
                FillDisc(bx, by, 17.f, qf ? gTheme.gold : Mix(gTheme.panel, gTheme.text, 0.12f));
                UiIcons::Draw(UiIcons::Id::Magnifier, (int)bx, (int)by, 17,
                              qf ? (ColorLuma(gTheme.gold) > 150 ? SDL_Color{ 20, 18, 28, 255 } : SDL_Color{ 250, 250, 255, 255 }) : gTheme.dim);
            }
            W::Marquee(Ui::HashStr("qr.how"), Tx("home.web.how"), c.x + c.w / 2, qTop + qh + 30, c.w - 40, gTheme.dim, Font::Body, true, W::Anchor::Centre);
            W::Marquee(Ui::HashStr("qr.warn"), Tx("home.web.warn"), c.x + c.w / 2, qTop + qh + 30 + TextH(Font::Body), c.w - 40, gTheme.faint, Font::Xs, true, W::Anchor::Centre);
        } else {
            const int cy = (midTop + c.y + c.h) / 2 - 20;
            W::IconTile(c.x + c.w / 2 - 36, cy - 66, 72, UiIcons::Id::Wifi, HasWifi() ? gTheme.gold : gTheme.dim);
            DrawTextC(Tx(HasWifi() ? "home.web.stopped" : "home.web.nowifi"), c.x + c.w / 2, cy + 30, gTheme.text, Font::Title);
            Ui::TextBlockC(Tx(HasWifi() ? "home.web.stopped2" : "home.web.nowifi2"), c.x + c.w / 2, cy + 56, c.w - 80, gTheme.dim,
                           Font::Body, 2);
        }
        W::EndBlock();
    }
    // USB file transfer.
    {
        const int x = sEdX + lw + gap;
        const SDL_Rect c{ x, hy, rw, W::kRowH + 12 + 70 };
        W::BeginBlock(c);
        W::Card(c);
        const bool mr = MtpServer::IsRunning(), mo = MtpServer::IsSessionOpen();
        const std::string us = Tx(mo ? "home.usb.on" : mr ? "home.usb.wait" : "home.usb.off");
        W::RowToggle(Ui::HashStr("ph.usb"), { c.x + 6, c.y + 6, c.w - 12, W::kRowH }, { Tx("home.usb.title"), us },
                     mr, [](bool) { TToggleMtp(0); });
        UiIcons::Draw(UiIcons::Id::Usb, c.x + 34, c.y + W::kRowH + 30, 18, mo ? gTheme.green : mr ? gTheme.warn : gTheme.faint);
        Ui::TextBlockC(Tx("phone.usb.desc"), c.x + c.w / 2 + 12, c.y + W::kRowH + 18, c.w - 80, gTheme.faint, Font::Xs, 3, 2);
        W::EndBlock();
        const int ay = c.y + c.h + gap;
        ActivityCard({ x, ay, rw, bottom - ay });
    }
    if (sQrBig && HttpServer::IsRunning() && HasWifi()) QrBigSheet();
    else sQrBig = false;
}

// ── The editor frame ─────────────────────────────────────────────────────────
static void PageEditor() {
    // A new visit to the island starts with nothing saved yet.
    {
        static bool sWasOpen = false;
        if (gEditorOpen != sWasOpen) { sWasOpen = gEditorOpen; sEdSavedVisit = false; }
    }
    // Coming back from a page with ◀ lands on the place shown, not on its
    // neighbour.
    {
        const Ui::Id cur = Focus::Current();
        const bool curSide = IsSidebarId(cur) && cur != Ui::HashStr("ed.save") && cur != Ui::HashStr("ed.leave") && cur != Ui::HashStr("ed.undo") &&
                             cur != Ui::HashStr("ed.fold");
        if (curSide && cur != EdItemId(EdActive()) && sEdLastFocus != 0 && !IsSidebarId(sEdLastFocus) && !Input::Last().tap)
            Focus::Set(EdItemId(EdActive()));
    }
    EdSidebar();
    Focus::PreferArea(SDL_Rect{ sEdX - 10, 0, sEdW + 20, SCREEN_H });
    // B (a page may set its own after this): from a page to the sidebar; from the sidebar, "Leave the island?".
    Focus::SetBaseBack([]() {
        Sound::Play(Sound::Voice::Release);
        if (IsSidebarId(Focus::Current())) { sLeaveAsk = true; Motion::Forget(Ui::HashStr("ed.leave.dlg")); Motion::Forget(Ui::HashStr("ed.leave.dlg2")); }
        else Focus::Set(EdItemId(EdActive()));
    }, Tx(IsSidebarId(Focus::Current()) ? "ed.leave" : "hint.back"));
    switch (EdActive()) {
        case OnSwitchMode::Backups:  PageBackups(); break;
        case OnSwitchMode::Phone:    PagePhone(); break;
        case OnSwitchMode::Settings: SettingsInEditor(sEdX, 34, sEdW); break;
        case OnSwitchMode::Player:   PagePlayer(); break;
        case OnSwitchMode::UGC:      PageTextures(); break;
        case OnSwitchMode::MiiStats: PageMiis(); break;
        case OnSwitchMode::Map:      PageMap(); break;
        default:                     PageHome(); break;
    }
    for (auto& fn : sEdLate) fn();
    sEdLate.clear();
    // L / R: the previous / next place.
    auto step = [](int d) {
        int i = 0;
        for (int k = 0; k < kEdOrderN; k++) if (kEdOrder[k] == EdActive()) i = k;
        const OnSwitchMode m = kEdOrder[(i + d + kEdOrderN) % kEdOrderN];
        Sound::Play(Sound::Voice::Whoosh);
        GoPage(m);
        if (!IsSidebarId(Focus::Current())) Focus::Set(0);
        else Focus::Set(EdItemId(m));
    };
    Focus::Command(Input::L, Tx("hint.place"), [step]() { step(-1); });
    Focus::Command(Input::R, Tx("hint.place"), [step]() { step(1); });
    Focus::Command(Input::Plus, Tx("ed.leave"), []() {});
    if (gShowMtpWarning) MtpDialog();
    if (sWarnAsk) WarnDialog();
    if (sLeaveAsk) LeaveDialog();
    sEdLastFocus = Focus::Current();
}
