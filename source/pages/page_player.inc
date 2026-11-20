// page_player.inc — TomoToolNX 2.0: the Player place. Player.sav as cards of
// rows: you, your money, your island, the clock. Every edit goes through the
// same fields and undo slots as 1.x (PLAYER_FIELDS, gPlayerUndo*), so what is
// written to the save does not change. Included by shell.inc inside S2.

static int PlField(const char* label) {
    for (int i = 0; i < PLAYER_FIELD_COUNT; i++) if (strcmp(PLAYER_FIELDS[i].label, label) == 0) return i;
    return 0;
}

static uint32_t PlGet(int fi) {
    const auto& fd = PLAYER_FIELDS[fi];
    const uint32_t h = PFHash(fd);
    if (fd.isUInt || fd.isSkinTone) return SaveEditor::GetUInt(gPlayerSav, h);
    if (fd.isEnum || fd.isRegion) return SaveEditor::GetEnum(gPlayerSav, h);
    return SaveEditor::GetAnyScalar(gPlayerSav, h);
}

// The hands the app draws (the map's pointer, the Skin tone row) take the
// skin tone chosen in Player.sav.
static void PlSyncSkinTone() {
    if (!gPlayerSav.loaded) return;
    static int fSkin = -2;
    if (fSkin == -2) {
        fSkin = -1;
        for (int i = 0; i < (int)(sizeof(PLAYER_FIELDS) / sizeof(PLAYER_FIELDS[0])); i++)
            if (PLAYER_FIELDS[i].isSkinTone) { fSkin = i; break; }
    }
    if (fSkin >= 0) Art::SetSkinTone((int)std::min(PlGet(fSkin), 5u));
}

static void PlKeepUndo(int fi, uint32_t cur) {
    if (!gPlayerUndoValid[fi]) { gPlayerUndoVal[fi] = (int32_t)cur; gPlayerUndoValid[fi] = true; }
}

// A number field moved by `delta`, within its limits (1.x's nudge, as is).
static void PlAdjust(int fi, int delta) {
    const auto& fd = PLAYER_FIELDS[fi];
    const uint32_t h = PFHash(fd);
    const uint32_t v = fd.isUInt ? SaveEditor::GetUInt(gPlayerSav, h) : SaveEditor::GetAnyScalar(gPlayerSav, h);
    PlKeepUndo(fi, v);
    uint32_t nv;
    if (fd.isInt) {
        int32_t sv = (int32_t)v + delta;
        if (fd.minVal != -1 && sv < fd.minVal) sv = fd.minVal;
        if (fd.maxVal != -1 && sv > fd.maxVal) sv = fd.maxVal;
        nv = (uint32_t)sv;
    } else if (delta < 0) {
        const uint32_t ad = (uint32_t)(-delta);
        nv = v >= ad ? v - ad : 0;
        if (fd.minVal >= 0 && (int32_t)nv < fd.minVal) nv = (uint32_t)fd.minVal;
    } else {
        nv = v + (uint32_t)delta;
        if (fd.maxVal >= 0 && (int32_t)nv > fd.maxVal) nv = (uint32_t)fd.maxVal;
    }
    if (fd.isUInt) SaveEditor::SetUInt(gPlayerSav, h, nv);
    else           SaveEditor::SetAnyScalar(gPlayerSav, h, nv);
    gPlayerSavDirty = true;
}

static std::string Thousands(uint32_t v) {
    std::string s = std::to_string(v), out;
    int n = 0;
    for (int i = (int)s.size() - 1; i >= 0; i--) {
        out.insert(out.begin(), s[i]);
        if (++n % 3 == 0 && i > 0) out.insert(out.begin(), ',');
    }
    return out;
}

// What X undoes on the highlighted row, if anything.
struct PlUndo { Ui::Id id; bool can; std::function<void()> fn; };
static std::vector<PlUndo> sPlUndo;

static void PlUndoField(Ui::Id id, int fi) {
    const bool can = PLAYER_FIELDS[fi].isDateTime ? gPlayerStampUndoValid : gPlayerUndoValid[fi];
    sPlUndo.push_back({ id, can, [fi]() { gPlayerFieldSel = fi; TUndoPlayer(0); } });
}

static int sPlAsk = 0;   // 1 grant every wish, 2 reset granted wishes
static bool sPlDonationMenu = false;

static Focus::Scroll sPlScroll;

// A section: its name above a card of rows. Returns the y below it.
struct PlSection {
    int x, y, w, rowY;
    void Begin(const std::string& name, int rows, int smallRows = 0) {
        W::Eyebrow(x + 6, y, name, gTheme.faint);
        const int h = rows * W::kRowH + smallRows * W::kRowHSmall + std::max(0, rows + smallRows - 1) + 12;
        W::Card({ x, y + 22, w, h });
        rowY = y + 22 + 6;
    }
    SDL_Rect Row(bool small = false, bool last = false) {
        const int h = small ? W::kRowHSmall : W::kRowH;
        SDL_Rect r{ x + 6, rowY, w - 12, h };
        rowY += h;
        if (!last) { W::Hairline(x + 24, rowY, w - 48); rowY += 1; }
        return r;
    }
    int End() const { return rowY + 6 + 26; }
};

// ── The hand, big (Player > Skin tone, A) ───────────────────────────────────
// The player's own hand in the chosen skin tone (the game's photos), doing
// what it does in the game, round and round: it pinches a Mii up and puts it
// down a little further, pokes it twice, then holds up its profile card with
// the thumb over it. A different Mii of the island each time round. ◀ ▶ (or
// a tap on a colour) changes the tone as it plays; A or B is done.
static uint32_t MiiAvatarSeed(int slot, const std::string& name);   // page_miis.inc
static int MiiFace(int slot);
static SDL_Color MiiHairColor(int slot);
static SDL_Texture* MiiPicture(int slot);

static bool sPlHand = false;
static SDL_Texture* sPlHandTex[5][6] = {};
static int sPlHandSz[5][6][2] = {};
enum PlPose { PoseOpen, PoseClose, PosePoint, PoseHold, PoseThumb };
static SDL_Texture* PlHandTex(int pose, int tone, int& w, int& h) {
    static const char* const kPose[5] = { "pick_open", "pick_close", "point", "hold", "thumb" };
    tone = std::max(0, std::min(5, tone));
    if (!sPlHandTex[pose][tone]) {
        const std::string path = std::string("romfs:/gfx/hands/") + kPose[pose] + "_" + std::to_string(tone) + ".png";
        if (SDL_Surface* s = IMG_Load(path.c_str())) {
            sPlHandTex[pose][tone] = SDL_CreateTextureFromSurface(gRen, s);
            sPlHandSz[pose][tone][0] = s->w; sPlHandSz[pose][tone][1] = s->h;
            if (sPlHandTex[pose][tone]) {
                SDL_SetTextureBlendMode(sPlHandTex[pose][tone], SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(sPlHandTex[pose][tone], SDL_ScaleModeLinear);
            }
            SDL_FreeSurface(s);
        }
    }
    w = sPlHandSz[pose][tone][0]; h = sPlHandSz[pose][tone][1];
    return sPlHandTex[pose][tone];
}
static void PlHandFree() {
    for (auto& row : sPlHandTex)
        for (auto& t : row) if (t) { SDL_DestroyTexture(t); t = nullptr; }
}
// A picture at (x, y) w x h, turned by `deg` around (px, py) inside it.
static void PlDrawTurned(SDL_Texture* t, float x, float y, float w, float h, float deg, float px, float py, uint8_t alpha = 255) {
    if (!t) return;
    SDL_SetTextureAlphaMod(t, alpha);
    const SDL_FRect d{ x, y, w, h };
    const SDL_FPoint c{ px, py };
    SDL_RenderCopyExF(gRen, t, nullptr, &d, deg, &c, SDL_FLIP_NONE);
    SDL_SetTextureAlphaMod(t, 255);
}

static void PlHandStage(int fSkin) {
    const Ui::Id id = Ui::HashStr("pl.handstage");
    const double since = Motion::FirstSeen(id), now = Motion::Now();
    const int tone = (int)std::min(PlGet(fSkin), 5u);
    auto close = []() { sPlHand = false; PlHandFree(); Motion::Forget(Ui::HashStr("pl.handstage")); };
    if (now - since < 0.0001) { Sound::Play(Sound::Voice::Open); Focus::Set(Ui::IdMix(id, 10u + (uint32_t)tone)); }
    Focus::PushLayer(Focus::Layer::Sheet, [close]() { Sound::Play(Sound::Voice::Release); close(); }, Tx("hint.close"), SDL_Rect{ 0, 0, SCREEN_W, SCREEN_H });
    const float in = Motion::Reduced() ? 1.f : std::min(1.f, (float)((now - since) / 0.3));
    DimScreen((int)(185 * in));
    // A soft light on the stage.
    for (int k = 0; k < 6; k++) FillDisc(SCREEN_W / 2.f, 350.f, 330.f - k * 48.f, Alpha(gTheme.gold, (int)(5 * in)));
    const uint8_t A = (uint8_t)(255 * in);
    // In the top left corner, out of the hand's way.
    W::IconTile(56, 44, 52, UiIcons::Id::User, gTheme.gold);
    DrawTextVC(Tx("pl.hand.title"), 124, 62, Alpha(gTheme.text, A), Font::Title);
    {
        int ly = 84;
        for (const auto& line : Ui::WrapLines(Tx("pl.hand.desc"), Font::Body, 330, 3)) { DrawText(line, 124, ly, Alpha(gTheme.faint, A), Font::Body); ly += 26; }
    }

    // The Mii in the scene: a different one of the island each time round.
    constexpr float L = 9.0f;
    const float tt = Motion::Reduced() ? 6.6f : (float)std::max(0.0, now - since - 0.35);
    const int loop = (int)(tt / L);
    const float t = Motion::Reduced() ? 6.6f : fmodf(tt, L);
    std::string name = Tx("pl.hand.you");
    int slot = -1;
    if (!gMiis.empty()) { const auto& m = gMiis[(size_t)loop % gMiis.size()]; slot = m.slot; name = m.name; }
    auto mii = [&](float cx, float cy, int r) {
        FillDisc(cx + 3.f, cy + r * 0.92f + 6.f, r * 0.7f, SDL_Color{ 0, 0, 0, (Uint8)(60 * in) });   // its shadow
        if (slot >= 0) W::Avatar((int)cx, (int)cy, r, name, MiiAvatarSeed(slot, name), MiiFace(slot), MiiHairColor(slot), MiiPicture(slot));
        else W::Avatar((int)cx, (int)cy, r, name, 7u);
    };
    auto ease = [](float a, float b, float x) { const float u = std::max(0.f, std::min(1.f, (x - a) / (b - a))); return u * u * (3.f - 2.f * u); };
    auto back = [](float a, float b, float x) {      // overshoots a little, then settles
        const float u = std::max(0.f, std::min(1.f, (x - a) / (b - a))), c1 = 1.70158f, c3 = c1 + 1.f;
        return 1.f + c3 * powf(u - 1.f, 3.f) + c1 * powf(u - 1.f, 2.f);
    };
    // Sounds, once a beat.
    static int sCue = -1;
    auto cue = [&](int k, float at, Sound::Voice v, float pitch) {
        const int key = loop * 16 + k;
        if (t >= at && t < at + 0.12f && sCue != key) { sCue = key; Sound::Play(v, { pitch }); }
    };
    const float P0x = 520.f, P1x = 760.f, Py = 430.f;
    const int R = 54;
    int w = 0, h = 0;
    if (t < 3.4f) {
        // Pinch: down from above, close on the Mii's head, lift, carry, put down, let go.
        const bool shut = t >= 0.8f && t < 2.7f;
        SDL_Texture* ht = PlHandTex(shut ? PoseClose : PoseOpen, tone, w, h);
        static const float kOpenX[6] = { 0.46f, 0.54f, 0.37f, 0.38f, 0.40f, 0.43f };
        static const float kCloseX[6] = { 0.62f, 0.59f, 0.37f, 0.46f, 0.43f, 0.52f };
        const float gxf = shut ? kCloseX[tone] : kOpenX[tone], gyf = shut ? 0.975f : 0.92f;
        float gx = P0x, gy = Py - R - 6.f, lift = 0.f;
        gy = -40.f + (Py - R - 6.f + 40.f) * back(0.f, 0.7f, t);
        if (t >= 0.95f) lift = 76.f * ease(0.95f, 1.45f, t) * (1.f - ease(2.25f, 2.65f, t));
        gx = P0x + (P1x - P0x) * ease(1.45f, 2.25f, t);
        if (t >= 2.85f) gy = (Py - R - 6.f) - 560.f * ease(2.85f, 3.4f, t);
        gy -= lift;
        // The Mii hangs under the fingers while held, swinging behind the move.
        const float held = (t >= 0.95f && t < 2.7f) ? 1.f : 0.f;
        const float vel = (P1x - P0x) * (ease(1.45f, 2.25f, t + 0.05f) - ease(1.45f, 2.25f, t - 0.05f)) / 0.1f;
        const float swing = held * (-vel * 0.02f + sinf(t * 9.f) * 3.f * (1.f - ease(0.95f, 1.6f, t)));
        float mx = t < 1.45f ? P0x : t >= 2.65f ? P1x : gx, my = Py;
        if (held > 0.f) { mx = gx + swing; my = gy + R + 6.f; }
        const float drop = t >= 2.7f ? (1.f - ease(2.7f, 3.0f, t)) * sinf((t - 2.7f) * 30.f) * 4.f : 0.f;
        const int rr = (int)(R * (t >= 0.8f && t < 0.95f ? 0.94f : 1.f));
        mii(mx, my + drop, rr);
        const float tilt = std::max(-7.f, std::min(7.f, vel * 0.025f));
        if (ht && h > 0) PlDrawTurned(ht, gx - gxf * w, gy - gyf * h, (float)w, (float)h, tilt, gxf * w, gyf * h, A);
        cue(0, 0.8f, Sound::Voice::Grab, 0.5f);
        cue(1, 2.7f, Sound::Voice::Land, 0.5f);
    } else if (t < 5.9f) {
        // Poke: in from the right, the fingertip on the Mii, twice; the Mii hops.
        SDL_Texture* ht = PlHandTex(PosePoint, tone, w, h);
        static const float kTipY[6] = { 0.22f, 0.17f, 0.12f, 0.22f, 0.16f, 0.25f };
        const float tx = P1x + R + 4.f, ty = Py - 10.f;
        float fx = SCREEN_W + 40.f + (tx + 70.f - SCREEN_W - 40.f) * back(3.4f, 3.9f, t);
        auto poke = [&](float a) { const float u = (t - a) / 0.28f; return u > 0.f && u < 1.f ? sinf(u * 3.14159f) : 0.f; };
        const float p = std::max(poke(3.9f), poke(4.25f));
        fx -= 70.f * p;
        if (t >= 5.0f) fx += (SCREEN_W + 80.f - tx) * ease(5.0f, 5.5f, t);
        const float hop = t >= 4.5f && t < 5.0f ? sinf((t - 4.5f) / 0.5f * 3.14159f) * 26.f : 0.f;
        const float fade = 1.f - ease(5.5f, 5.9f, t);
        if (fade > 0.01f) mii(P1x - 4.f * p, Py - hop, (int)(R * (0.92f + 0.08f * fade) * (1.f - 0.06f * p)));
        // A ring where the finger touches.
        for (float a : { 3.9f, 4.25f }) {
            const float u = (t - a - 0.14f) / 0.4f;
            if (u > 0.f && u < 1.f) StrokeArc(tx, ty, 10.f + 34.f * u, 3.f, 0.f, 360.f, Alpha(gTheme.gold, (int)(220 * (1.f - u))), Alpha(gTheme.gold, (int)(220 * (1.f - u))));
        }
        if (ht && h > 0) PlDrawTurned(ht, fx, ty - kTipY[tone] * h, (float)w, (float)h, -3.f * p, 0.f, kTipY[tone] * h, A);
        cue(2, 4.04f, Sound::Voice::Tick, 0.7f);
        cue(3, 4.39f, Sound::Voice::Tick, 0.85f);
        cue(4, 4.5f, Sound::Voice::Pluck, 0.7f);
    } else {
        // The profile card, held up from below: the fingers behind it, the thumb over it.
        int tw = 0, th = 0;
        SDL_Texture* hand = PlHandTex(PoseHold, tone, w, h);
        SDL_Texture* thumb = PlHandTex(PoseThumb, tone, tw, th);
        const float rise = back(5.9f, 6.5f, t) * (1.f - ease(8.3f, 9.0f, t));
        const float bob = sinf((t - 5.9f) * 2.2f) * 5.f;
        const float cx = SCREEN_W / 2.f - 30.f, cy = 300.f + (1.f - rise) * 500.f + bob;
        const int cw = 360, ch = 200;
        const float cardX = cx - cw / 2.f, cardY = cy - ch / 2.f;
        // The hand: its thumb on the card's lower right corner.
        const float k = w > 0 ? w / 512.f : 1.f;
        const float hx = cardX + cw - 120.f, hy = cardY + ch - 70.f;
        if (hand && h > 0) PlDrawTurned(hand, hx, hy, (float)w, (float)h, 0.f, 0.f, 0.f, A);
        FillRectRounded((int)cardX + 4, (int)cardY + 8, cw, ch, 24, SDL_Color{ 0, 0, 0, 80 });
        FillRectRounded((int)cardX, (int)cardY, cw, ch, 24, Mix(gTheme.panel, SDL_Color{ 255, 255, 255, 255 }, 0.08f));
        FillRectRounded((int)cardX, (int)cardY, cw, 44, 24, Alpha(gTheme.gold, 60));
        DrawRectRounded((int)cardX, (int)cardY, cw, ch, 24, Alpha(gTheme.border, 230));
        DrawTextVC(Tx("pl.hand.card"), (int)cardX + 22, (int)cardY + 22, gTheme.dim, Font::Xs);
        mii(cardX + 82.f, cardY + 118.f, 50);
        W::Marquee(Ui::HashStr("pl.hand.name"), name, (int)cardX + 152, (int)cardY + 100, cw - 170, gTheme.text, Font::Title, true, W::Anchor::Middle);
        for (int i = 0; i < 3; i++) {
            const float beat = 1.f + 0.12f * std::max(0.f, sinf((t - 6.6f) * 5.f - i * 0.8f));
            FillDisc(cardX + 162.f + i * 30.f, cardY + 148.f, 9.f * beat, SDL_Color{ 255, 118, 150, 255 });
        }
        if (thumb && th > 0) PlDrawTurned(thumb, hx + 28.f * k, hy - 21.f * k, (float)tw, (float)th, 0.f, 0.f, 0.f, A);
        cue(5, 6.0f, Sound::Voice::Rise, 0.6f);
    }

    // The six skin tones: moving along them shows each at once.
    static const SDL_Color SKIN[] = { {246,217,189,255},{236,193,157,255},{210,160,122,255},{176,122,84,255},{130,85,51,255},{90,57,28,255} };
    const int d = 56, gap = 22, total = 6 * d + 5 * gap, x0 = SCREEN_W / 2 - total / 2, sy = 640;
    for (int i = 0; i < 6; i++) {
        const Ui::Id sid = Ui::IdMix(id, 10u + (uint32_t)i);
        const SDL_Rect r{ x0 + i * (d + gap), sy - d / 2, d, d };
        Focus::Node n;
        n.id = sid; n.r = r; n.radius = d / 2; n.flags = Focus::OwnSound | Focus::Ring;
        n.press = [close]() { Sound::Play(Sound::Voice::Ok); close(); };
        n.hint = Tx("pl.hand.done");
        Focus::Add(std::move(n));
        const float cxs = r.x + d / 2.f;
        FillDisc(cxs, sy + 3.f, d / 2.f, SDL_Color{ 0, 0, 0, 70 });
        FillDisc(cxs, (float)sy, d / 2.f * (i == tone ? 1.08f : 1.f), SKIN[i]);
        if (i == tone) StrokeArc(cxs, (float)sy, d / 2.f + 7.f, 3.f, 0.f, 360.f, gTheme.gold, gTheme.gold);
    }
    // The highlight picks the tone (as the row's ◀ ▶ do).
    for (int i = 0; i < 6; i++)
        if (Focus::Is(Ui::IdMix(id, 10u + (uint32_t)i)) && i != tone) {
            PlKeepUndo(fSkin, PlGet(fSkin));
            SaveEditor::SetUInt(gPlayerSav, PFHash(PLAYER_FIELDS[fSkin]), (uint32_t)i);
            gPlayerSavDirty = true;
            Art::SetSkinTone(i);
            Sound::Play(Sound::Voice::Tick, { i / 5.f });
        }
    Focus::PopLayer();
}

static void PagePlayer() {
    if (!gPlayerSav.loaded) {
        static double sTry = -10;
        if (Motion::Now() - sTry > 2.0) {
            sTry = Motion::Now();
            std::string err;
            if (!SaveEditor::Load(SAVE_PLAYER_SAV, gPlayerSav, err)) gPlayerMsg = "Load error: " + err;
        }
    }
    W::BeginBlock({ sEdX, 34, sEdW, 90 });
    const int hy = W::PageHeader(sEdX, 34, sEdW, UiIcons::Id::User, IslandName(), Tx("tab.player"), Tx("pl.desc"),
                                 gPlayerSavDirty ? Tx("status.unsaved") : std::string());
    W::EndBlock();
    if (!gPlayerSav.loaded) {
        const SDL_Rect c{ sEdX, hy, sEdW, 220 };
        W::BeginBlock(c);
        W::Card(c);
        W::IconTile(sEdX + sEdW / 2 - 34, c.y + 30, 68, UiIcons::Id::Alert, gTheme.red);
        W::Marquee(Ui::HashStr("pl.msg"), gPlayerMsg.empty() ? Tx("player.loading") : gPlayerMsg, sEdX + sEdW / 2, c.y + 130, sEdW - 60, gTheme.text, Font::Row, true, W::Anchor::Centre);
        DrawTextC(Tx("player.notfound.hint"), sEdX + sEdW / 2, c.y + 166, gTheme.dim, Font::Body);
        W::EndBlock();
        return;
    }
    sPlUndo.clear();
    const SDL_Rect view{ sEdX - 10, hy - 6, sEdW + 20, SCREEN_H - 62 - (hy - 6) };
    Focus::BeginScroll(Ui::HashStr("pl.scroll"), view, sPlScroll);
    PlSection s{ sEdX, hy - (int)sPlScroll.offset, sEdW, 0 };

    const int fName = PlField("Name"), fIsland = PlField("Island"), fMoney = PlField("Money"), fCur = PlField("Currency");
    const int fBoot = PlField("Boot Count"), fSkin = PlField("Skin Tone"), fDay = PlField("Bday Day"), fMon = PlField("Bday Month");
    const int fYear = PlField("Bday Year"), fSize = PlField("Island Size"), fFount = PlField("Fountain Lv"), fWish = PlField("Wishes");
    const int fReg = PlField("Region"), fPen = PlField("TT Penalty"), fDon = PlField("Donation");

    auto keyboardRow = [&](const char* id, const W::RowText& t, int fi, const char* hashName, const std::string& empty, bool last) {
        const uint32_t h = hashName ? SaveEditor::Hash(hashName) : PFHash(PLAYER_FIELDS[fi]);
        const std::string cur = SaveEditor::GetWStr32(gPlayerSav, h);
        const std::string guide = t.title;
        W::RowLink(Ui::HashStr(id), s.Row(false, last), t, cur.empty() ? empty : cur, [h, guide]() {
            const std::string was = SaveEditor::GetWStr32(gPlayerSav, h);
            const std::string nv = ShowKeyboard(guide, was, 30);
            if (nv != was) { SaveEditor::SetWStr32(gPlayerSav, h, nv); gPlayerSavDirty = true; Sound::Play(Sound::Voice::Ok, { 0.5f, false, 0.6f }); }
        }, UiIcons::Id::Pencil, false, 0, !cur.empty());
    };
    auto langRow = [&](const char* id, const W::RowText& t, const char* field, bool isName, bool last) {
        const uint32_t lh = SaveEditor::Hash(field);
        const uint32_t cur = SaveEditor::GetEnum(gPlayerSav, lh);
        int li = 0;
        for (int k = 0; k < LANG_OPTION_COUNT; k++) if (SaveEditor::Hash(LANG_OPTIONS[k].name) == cur) li = k;
        const Ui::Id rid = Ui::HashStr(id);
        static std::vector<std::string> sLangLabels;
        if (sLangLabels.empty()) for (int k = 0; k < LANG_OPTION_COUNT; k++) sLangLabels.push_back(LANG_OPTIONS[k].label);
        W::RowChoice(rid, s.Row(false, last), t, sLangLabels, li, [lh, cur, li, isName](int d) {
                          if (isName) { if (!gNameLangUndoValid) { gNameLangUndo = (int32_t)cur; gNameLangUndoValid = true; } }
                          else        { if (!gIslandLangUndoValid) { gIslandLangUndo = (int32_t)cur; gIslandLangUndoValid = true; } }
                          const int n = std::max(0, std::min(LANG_OPTION_COUNT - 1, li + d));
                          SaveEditor::SetEnum(gPlayerSav, lh, SaveEditor::Hash(LANG_OPTIONS[n].name));
                          gPlayerSavDirty = true;
                      });
        sPlUndo.push_back({ rid, isName ? gNameLangUndoValid : gIslandLangUndoValid, [isName]() {
            if (isName) TUndoNameLang(0); else TUndoIslandLang(0);
        } });
    };
    auto numberRow = [&](const char* id, const W::RowText& t, int fi, int lo, int hi, int stepBy, const std::string& shown,
                         bool typeable, bool last) {
        const uint32_t v = PlGet(fi);
        const Ui::Id rid = Ui::HashStr(id);
        // Where the value sits, for the sound and the end stops: only the
        // field's real limits stop it (1.x's min/max; -1 = none).
        const auto& fd = PLAYER_FIELDS[fi];
        const int32_t sv = fd.isInt ? (int32_t)v : (int32_t)std::min<uint32_t>(v, 0x7fffffffu);
        const bool atLo = fd.isInt ? (fd.minVal != -1 && sv <= fd.minVal) : (v == 0 || (fd.minVal >= 0 && sv <= fd.minVal));
        const bool atHi = fd.maxVal != -1 && sv >= fd.maxVal;
        float frac = hi > lo ? ((float)sv - lo) / (hi - lo) : 0.5f;
        frac = atLo ? 0.f : atHi ? 1.f : std::max(0.02f, std::min(0.98f, frac));
        // Every number can be typed (A, or a tap on it), not only some.
        (void)typeable;
        W::RowStepper(rid, s.Row(false, last), t, shown, frac, hi > lo ? (float)stepBy / (hi - lo) : 0.f,
                      [fi, stepBy](int d) { PlAdjust(fi, d * stepBy); },
                      std::function<void()>([fi]() { gPlayerFieldSel = fi; TPlayerNumKbd(0); }),
                      Tx("hint.type"));
        PlUndoField(rid, fi);
    };

    // ── You ──
    {
        s.Begin(Tx("pl.card.you"), 5);
        keyboardRow("pl.name", { Tx("pl.name"), Tx("pl.name.desc") }, fName, nullptr, std::string(), false);
        keyboardRow("pl.pron", { Tx("pl.pron"), Tx("pl.pron.desc") }, fName, "Player.HowToCallName", Tx("pl.pron.same"), false);
        langRow("pl.lang", { Tx("pl.lang"), Tx("pl.lang.desc") }, "Player.NameRegionLanguageID", true, false);
        {
            static const SDL_Color SKIN[] = { {246,217,189,255},{236,193,157,255},{210,160,122,255},{176,122,84,255},{130,85,51,255},{90,57,28,255} };
            const uint32_t cur = PlGet(fSkin);
            const Ui::Id rid = Ui::HashStr("pl.skin");
            const SDL_Rect rr = s.Row();
            W::RowSwatches(rid, rr, { Tx("pl.skin"), Tx("pl.skin.desc") }, SKIN, 6, (int)std::min(cur, 5u), [fSkin](int d) {
                const uint32_t c = PlGet(fSkin);
                PlKeepUndo(fSkin, c);
                const int n = std::max(0, std::min(5, (int)c + d));
                SaveEditor::SetUInt(gPlayerSav, PFHash(PLAYER_FIELDS[fSkin]), (uint32_t)n);
                gPlayerSavDirty = true;
                Art::SetSkinTone(n);
            }, []() { sPlHand = true; Motion::Forget(Ui::HashStr("pl.handstage")); }, Tx("pl.hand.see"));
            PlUndoField(rid, fSkin);
            // The player's own hand, in that tone (the game's photo), pointing
            // at the colours from the left, upright, its fingertip level with
            // them; it nudges toward them while highlighted.
            int hw = 0, hh = 0;
            const int tone = (int)std::min(cur, 5u);
            if (SDL_Texture* ht = Art::Get(Art::HandPoint(tone), &hw, &hh)) {
                // Where the fingertip is in each picture, as a share of its height.
                static const float kTipY[6] = { 0.22f, 0.17f, 0.12f, 0.22f, 0.16f, 0.25f };
                const int x0 = rr.x + rr.w - 22 - (6 * 34 + 5 * 10);
                const int dh = 36;
                const float nudge = Motion::Reduced() || !Focus::Is(rid) ? 0.f : (0.5f + 0.5f * sinf((float)Motion::Now() * 4.f)) * 5.f;
                const int top = rr.y + rr.h / 2 - (int)lroundf(kTipY[tone] * dh);
                const int tip = x0 - 12 + (int)nudge;            // the fingertip, at the right
                // The picture points left with the forearm trailing off to the
                // right: only the hand and the wrist are drawn, mirrored, the
                // wrist in thin strips that fade out where the arm would go on.
                const int sw = hw * 48 / 100, solid = sw * 62 / 100, strips = 14;
                auto dx = [&](int s) { return tip - (int)lroundf((float)s * dh / std::max(1, hh)); };
                auto piece = [&](int s0, int s1, float alpha) {
                    if (dx(s0) - dx(s1) <= 0) return;
                    const SDL_Rect src{ s0, 0, s1 - s0, hh };
                    DrawTextureFlipped(ht, &src, SDL_Rect{ dx(s1), top, dx(s0) - dx(s1), dh }, (uint8_t)lroundf(255.f * alpha), true);
                };
                piece(0, solid, 1.f);
                for (int k = 0; k < strips; k++)
                    piece(solid + (sw - solid) * k / strips, solid + (sw - solid) * (k + 1) / strips, 1.f - (k + 0.5f) / strips);
            }
        }
        // The birthday on one row: the date as the language writes it; A
        // opens a calendar, ◀ ▶ move it a day.
        {
            const int d = (int)PlGet(fDay), mo = (int)PlGet(fMon), yr = (int32_t)PlGet(fYear);
            auto setDate = [fDay, fMon, fYear](int ny, int nm, int nd) {
                const int dy = ny - (int32_t)PlGet(fYear), dm = nm - (int)PlGet(fMon), dd = nd - (int)PlGet(fDay);
                if (dy) PlAdjust(fYear, dy);
                if (dm) PlAdjust(fMon, dm);
                if (dd) PlAdjust(fDay, dd);
            };
            const Ui::Id rid = Ui::HashStr("pl.bday");
            W::RowStepper(rid, s.Row(false, true), { Tx("pl.bday"), Tx("pl.bday.desc") },
                          (mo >= 1 && mo <= 12) ? DateText(yr, mo, d) : std::to_string(d) + "/" + std::to_string(mo) + "/" + std::to_string(yr),
                          0.5f, 0.f, [d, mo, yr, setDate](int dd) {
                              int nd = d + dd, nm = std::max(1, std::min(12, mo)), ny = yr;
                              auto dim = [](int y_, int m_) { static const int k[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
                                  return m_ == 2 && (y_ % 4 == 0 && (y_ % 100 != 0 || y_ % 400 == 0)) ? 29 : k[m_ - 1]; };
                              if (nd < 1) { if (--nm < 1) { nm = 12; ny--; } nd = dim(ny, nm); }
                              else if (nd > dim(ny, nm)) { nd = 1; if (++nm > 12) { nm = 1; ny++; } }
                              setDate(ny, nm, nd);
                          },
                          [d, mo, yr, setDate]() {
                              W::OpenCalendar(Ui::HashStr("pl.bday"), Tx("pl.bday"), yr, mo, d, std::min(1900, yr), std::max(2100, yr), setDate);
                          },
                          Tx("hint.date"));
            // X puts back all three.
            const bool can = gPlayerUndoValid[fDay] || gPlayerUndoValid[fMon] || gPlayerUndoValid[fYear];
            sPlUndo.push_back({ rid, can, [fDay, fMon, fYear]() {
                for (int fi : { fYear, fMon, fDay }) if (gPlayerUndoValid[fi]) { gPlayerFieldSel = fi; TUndoPlayer(0); }
            } });
        }
        s.y = s.End();
    }
    // ── Money ──
    {
        s.Begin(Tx("pl.card.money"), 2);
        const uint32_t cur = SaveEditor::GetEnum(gPlayerSav, PFHash(PLAYER_FIELDS[fCur]));
        const std::string sym = CurrencySymbol(cur);
        numberRow("pl.money", { Tx("pl.money"), Tx("pl.money.desc") }, fMoney, 0, 9999999, 100,
                  sym + " " + Thousands(PlGet(fMoney)), true, false);
        int ci = std::max(0, CurrencyIndex(cur));
        const Ui::Id rid = Ui::HashStr("pl.cur");
        static std::vector<std::string> sCurLabels;
        if (sCurLabels.empty())
            for (int k = 0; k < CURRENCY_LABEL_COUNT; k++) sCurLabels.push_back(std::string(CURRENCY_LABELS[k].symbol) + "  " + CURRENCY_LABELS[k].name);
        W::RowChoice(rid, s.Row(false, true), { Tx("pl.currency"), Tx("pl.currency.desc") }, sCurLabels, ci, [fCur, ci, cur](int d) {
                          PlKeepUndo(fCur, cur);
                          const int n = std::max(0, std::min(CURRENCY_LABEL_COUNT - 1, ci + d));
                          SaveEditor::SetEnum(gPlayerSav, PFHash(PLAYER_FIELDS[fCur]), CURRENCY_LABELS[n].hash);
                          gPlayerSavDirty = true;
                      });
        PlUndoField(rid, fCur);
        s.y = s.End();
    }
    // ── Island ──
    {
        const bool wishes = WishesAvailable();
        s.Begin(Tx("pl.card.island"), 7 + (wishes ? 2 : 0));
        keyboardRow("pl.island", { Tx("pl.island"), Tx("pl.island.desc") }, fIsland, nullptr, std::string(), false);
        keyboardRow("pl.ipron", { Tx("pl.island.pron"), Tx("pl.pron.desc") }, fIsland, "Player.HowToCallIslandName", Tx("pl.pron.same"), false);
        langRow("pl.ilang", { Tx("pl.island.lang"), Tx("pl.lang.desc") }, "Player.IslandNameRegionLanguageID", false, false);
        {
            uint32_t sz = PlGet(fSize);
            if (sz < 1 || sz > 4) sz = 1;
            const Ui::Id rid = Ui::HashStr("pl.size");
            // The four sizes the game has, as the island's width x height in
            // tiles (as in the WebUI).
            static const std::vector<std::string> kSizes = { "50\xC3\x97" "36", "70\xC3\x97" "50", "90\xC3\x97" "64", "120\xC3\x97" "80" };
            W::RowSegments(rid, s.Row(), { Tx("pl.size"), Tx("pl.size.desc") }, kSizes, (int)sz - 1, [fSize, sz](int i) {
                PlKeepUndo(fSize, sz);
                SaveEditor::SetAnyScalar(gPlayerSav, PFHash(PLAYER_FIELDS[fSize]), (uint32_t)(i + 1));
                gPlayerSavDirty = true;
            });
            PlUndoField(rid, fSize);
        }
        {
            const uint32_t cur = SaveEditor::GetEnum(gPlayerSav, PFHash(PLAYER_FIELDS[fReg]));
            int ri = 0;
            for (int k = 0; k < REGION_OPTION_COUNT; k++) if (SaveEditor::Hash(REGION_OPTIONS[k].name) == cur) ri = k;
            const Ui::Id rid = Ui::HashStr("pl.region");
            static std::vector<std::string> sRegLabels;
            if (sRegLabels.empty()) for (int k = 0; k < REGION_OPTION_COUNT; k++) sRegLabels.push_back(REGION_OPTIONS[k].label);
            W::RowChoice(rid, s.Row(), { Tx("pl.region"), Tx("pl.region.desc") }, sRegLabels, ri, [fReg, ri, cur](int d) {
                              PlKeepUndo(fReg, cur);
                              const int n = std::max(0, std::min(REGION_OPTION_COUNT - 1, ri + d));
                              SaveEditor::SetEnum(gPlayerSav, PFHash(PLAYER_FIELDS[fReg]), SaveEditor::Hash(REGION_OPTIONS[n].name));
                              gPlayerSavDirty = true;
                          });
            PlUndoField(rid, fReg);
        }
        numberRow("pl.fount", { Tx("pl.fountain"), Tx("pl.fountain.desc") }, fFount, 0, 10, 1, std::to_string(PlGet(fFount)), false, false);
        numberRow("pl.wishes", { Tx("pl.wishes"), Tx("pl.wishes.desc") }, fWish, 0, 300, 1, std::to_string(PlGet(fWish)), true, !wishes);
        if (wishes) {
            const int granted = WishesLiberatedCount();
            W::RowLink(Ui::HashStr("pl.wish.all"), s.Row(), { Tx("pl.wishes.unlock"),
                       TokenFill(Tx("pl.wishes.unlock.desc"), "{n}", std::to_string(WishesData::WISH_COUNT)) }, std::string(),
                       []() { sPlAsk = 1; Motion::Forget(Ui::HashStr("pl.ask")); }, UiIcons::Id::Sparkle);
            W::RowLink(Ui::HashStr("pl.wish.reset"), s.Row(false, true), { Tx("pl.wishes.reset"),
                       TokenFill(Tx("pl.wishes.reset.desc"), "{n}", std::to_string(granted)) }, std::string(),
                       []() { sPlAsk = 2; Motion::Forget(Ui::HashStr("pl.ask")); }, UiIcons::Id::Undo, true);
        }
        s.y = s.End();
    }
    // ── Time ──
    {
        s.Begin(Tx("pl.card.time"), 3);
        numberRow("pl.boot", { Tx("pl.boot"), Tx("pl.boot.desc") }, fBoot, 0, 9999, 1,
                  TokenFill(Tx("pl.boot.times"), "{n}", std::to_string(PlGet(fBoot))), true, false);
        {
            const bool on = SaveEditor::GetUInt64(gPlayerSav, PFHash(PLAYER_FIELDS[fPen])) != 0;
            W::RowLink(Ui::HashStr("pl.penalty"), s.Row(), { Tx("pl.penalty"), Tx("pl.penalty.desc") },
                       Tx(on ? "player.lastpenalty.active" : "player.lastpenalty.cleared"), [fPen]() {
                           gPlayerFieldSel = fPen;
                           const bool was = SaveEditor::GetUInt64(gPlayerSav, PFHash(PLAYER_FIELDS[fPen])) != 0;
                           TPlayerNumKbd(0);
                           if (was) { Sound::Play(Sound::Voice::Ok); Ui::ToastShow(Tx("pl.penalty.done"), Ui::Tone::Good); }
                           else     { Sound::Play(Sound::Voice::Info); Ui::ToastShow(Tx("player.lastpenalty.already")); }
                       }, on ? UiIcons::Id::Alert : UiIcons::Id::Check);
        }
        {
            const uint64_t st = SaveEditor::GetUInt64(gPlayerSav, PFHash(PLAYER_FIELDS[fDon]));
            std::string shown = Tx("player.donation.never");
            // The day, as the birthday shows it, and how long ago.
            time_t tt = st ? (time_t)st : time(nullptr);
            struct tm tmv{};
            const bool have = localtime_r(&tt, &tmv) != nullptr;
            if (st && have) {
                const int64_t days = DaysSinceStamp(st);
                shown = DateText(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
                if (days >= 0) shown += "  \xC2\xB7  " + std::to_string(days) + " " + Tx(days == 1 ? "player.donation.dayssince.one" : "player.donation.dayssince");
            }
            const Ui::Id rid = Ui::HashStr("pl.don");
            const int cy = have ? tmv.tm_year + 1900 : 2026, cm = have ? tmv.tm_mon + 1 : 1, cd = have ? tmv.tm_mday : 1;
            const int ch = st && have ? tmv.tm_hour : 12, cmin = st && have ? tmv.tm_min : 0;
            W::RowStepper(rid, s.Row(false, true), { Tx("pl.donation"), Tx("pl.donation.desc") }, shown, 0.5f, 0.f,
                          [fDon, st](int d) {
                              gPlayerFieldSel = fDon;
                              const uint64_t now = (uint64_t)time(nullptr);
                              if (d < 0) PlayerSetStamp(st > SECS_PER_DAY ? st - SECS_PER_DAY : 0);
                              else       PlayerSetStamp(st ? st + SECS_PER_DAY : now);
                          },
                          // A: the calendar (the time of day stays as it was).
                          [fDon, cy, cm, cd, ch, cmin]() {
                              W::OpenCalendar(Ui::HashStr("pl.doncal"), Tx("pl.donation"), cy, cm, cd, std::min(2000, cy), std::max(2100, cy),
                                              [fDon, ch, cmin](int y, int m, int d) {
                                                  struct tm t{};
                                                  t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d;
                                                  t.tm_hour = ch; t.tm_min = cmin; t.tm_isdst = -1;
                                                  const time_t v = mktime(&t);
                                                  if (v <= 0) return;
                                                  gPlayerFieldSel = fDon;
                                                  PlayerSetStamp((uint64_t)v);
                                              });
                          }, Tx("hint.date"));
            // Y: today, or never collected.
            if (Focus::Is(rid))
                Focus::Command(Input::Y, Tx("pl.donation.more"), []() { sPlDonationMenu = true; Motion::Forget(Ui::HashStr("pl.donmenu")); });
            PlUndoField(rid, fDon);
        }
        s.y = s.End();
    }
    Focus::EndScroll((float)(s.y + (int)sPlScroll.offset - (hy - 6)));

    // X: undo the highlighted row.
    for (const PlUndo& u : sPlUndo) {
        if (u.can && Focus::Is(u.id)) {
            auto fn = u.fn;
            Focus::Command(Input::X, Tx("hint.undo"), [fn]() { fn(); Sound::Play(Sound::Voice::Undo); });
        }
    }

    if (sPlDonationMenu) {
        std::vector<W::MenuItem> items;
        items.push_back({ Tx("player.donation.settoday"), "", UiIcons::Id::Clock, false, [fDon]() {
            gPlayerFieldSel = fDon; PlayerSetStamp((uint64_t)time(nullptr)); sPlDonationMenu = false; }, false });
        items.push_back({ Tx("player.donation.clear"), Tx("player.donation.never.hint"), UiIcons::Id::Close, false, [fDon]() {
            gPlayerFieldSel = fDon; PlayerSetStamp(0); sPlDonationMenu = false; }, false });
        W::Menu(Ui::HashStr("pl.donmenu"), Tx("pl.donation"), items, []() { sPlDonationMenu = false; }, Tx("hint.close"), 0,
                Focus::RectOf(Ui::HashStr("pl.don")));
    }
    if (sPlAsk) {
        const bool all = sPlAsk == 1;
        W::Dialog(Ui::HashStr("pl.ask"), all ? UiIcons::Id::Sparkle : UiIcons::Id::Undo, all ? Ui::Tone::Accent : Ui::Tone::Bad,
                  Tx(all ? "pl.wishes.unlock.ask" : "pl.wishes.reset.ask"), Tx("pl.wishes.ask.body"),
                  { { Tx("picker.cancel"), W::Kind::Neutral, []() { sPlAsk = 0; } },
                    { Tx(all ? "pl.wishes.unlock.go" : "pl.wishes.reset.go"), all ? W::Kind::Primary : W::Kind::Danger, [all]() {
                        sPlAsk = 0;
                        const int n = all ? WishesBulkUnlock() : WishesBulkReset();
                        Sound::Play(n > 0 ? Sound::Voice::Done : Sound::Voice::Info);
                        Ui::ToastShow(TokenFill(Tx(all ? "pl.wishes.unlocked" : "pl.wishes.reset.done"), "{n}", std::to_string(n)),
                                      n > 0 ? Ui::Tone::Good : Ui::Tone::Neutral);
                    } } },
                  0, []() { sPlAsk = 0; }, Tx("hint.cancel"));
    }
    if (sPlHand && fSkin >= 0) PlHandStage(fSkin);
    else if (sPlHand) sPlHand = false;
}
