// shell.inc — TomoToolNX 2.0 interface: the shell and the pages built on it.
// Included by main.cpp (after everything the pages use), so the pages can call
// the same save-editing functions the old screens call.
//
// One frame:  Focus::Resolve (act on what was on screen)  →  build the page
// (widgets draw and register what pressing them does)  →  hint bar  →
// Focus::EndBuild  →  present.

#include <atomic>

#include "mii_art.h"
#include "mii_pack.h"
#include "ui_art.h"
#include "ui_focus.h"
#include "ui_input.h"
#include "ui_motion.h"
#include "ui_widgets.h"

namespace S2 {

static std::string Tx(const char* k) { return Lang::T(k); }

// What the shell is showing.
enum class Page : uint8_t {
    None, Settings, Start, BackupPrompt, BackingUp, Error, UpdateCheck, UpdateAvailable, Applet, Saved, NoSave,
    NewIsland, ApplyIsland, Editor, IslandGen, IslandGenBatch, IslandGenRun,
};
static Page   sShown = Page::None;
static Screen sShownScreen = Screen::UserPick;
static bool   sQuitAsk = false;

// The column on the left of the standalone pages (Settings from the start).
constexpr int kSideW = 300;

static void SidePanel() {
    FillRectSquare(0, 0, kSideW, SCREEN_H, Alpha(gTheme.panel, 120));
    FillRectSquare(kSideW - 1, 0, 1, SCREEN_H, Alpha(gTheme.border, 200));
}

// The app's mark: the TomoToolNX logo (the game's logo held by two hands),
// the name, and a line under it. Without the art, the old accent tile.
// ── The logo, out for a walk (an easter egg) ──
// A tap on the logo: it hops down to the bottom of the screen and wanders
// there like a little desktop pet (walking, stopping, looking around, sitting,
// hopping). A tap on it, or on the place it left, and it jumps back home.
namespace Pet {
enum St { Home, Out, Walk, Back };
St st = Home;
SDL_Rect home{ 18, 20, 86, 54 };       // where Brand drew the logo last
double homeSeen = -1, arrived = -10;   // when Brand last drew; when it came back
float x = 0, y = 0, dir = 1;           // its feet (bottom middle), on screen
float fx = 0, fy = 0, tx = 0, ty = 0;  // a jump: from, to
double t0 = 0, actEnd = 0, landed = -10;
int act = 0;                           // 0 walk, 1 stop, 2 look around, 3 sit, 4 hop
uint32_t rng = 0x9E3779B9u;
float Rand() { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 16777216.f; }
constexpr float kFloor = SCREEN_H - 50.f, kW = 86.f, kH = 54.f;
SDL_Rect Rect() { return SDL_Rect{ (int)(x - kW / 2), (int)(y - kH), (int)kW, (int)kH }; }
void Jump(float toX, float toY, St next) {
    fx = x; fy = y; tx = toX; ty = toY; t0 = Motion::Now(); st = next;
}
void Pick(double now) {
    const float r = Rand();
    act = r < 0.55f ? 0 : r < 0.7f ? 1 : r < 0.8f ? 2 : r < 0.9f ? 3 : 4;
    actEnd = now + (act == 0 ? 2.0 + Rand() * 3.0 : act == 4 ? 0.6 : act == 3 ? 2.0 + Rand() : 1.2 + Rand() * 0.8);
    if (act == 0 && Rand() < 0.3f) dir = -dir;
}
// Before the page reads the frame: a tap on the logo or on the pet.
void Input() {
    if (!Art::Has(Art::Id::Logo)) return;
    const bool homeShown = homeSeen >= 0 && Motion::Now() - homeSeen < 0.2;
    if (st == Home) {
        if (homeShown && Input::TakeTap(home)) {
            x = home.x + home.w / 2.f; y = (float)(home.y + home.h);
            Jump(std::max(60.f, std::min(SCREEN_W - 60.f, x + 140.f + Rand() * 260.f)), kFloor, Out);
            Sound::Play(Sound::Voice::Open);
        }
        return;
    }
    if (st == Walk) {
        const SDL_Rect r = Rect();
        if (Input::TakeTap(SDL_Rect{ r.x - 10, r.y - 10, r.w + 20, r.h + 20 }) || (homeShown && Input::TakeTap(home))) {
            Jump(home.x + home.w / 2.f, (float)(home.y + home.h), Back);
            Sound::Play(Sound::Voice::Whoosh);
        }
    }
}
// After everything else: the pet itself.
void Draw() {
    if (st == Home || !Art::Has(Art::Id::Logo)) return;
    SDL_Texture* tex = Art::Get(Art::Id::Logo);
    if (!tex) return;
    const double now = Motion::Now();
    const float dt = Motion::Dt();
    float sx = 1.f, sy = 1.f, rot = 0.f, lift = 0.f;
    if (st == Out || st == Back) {
        const float dur = st == Out ? 0.75f : 0.85f;
        const float u = std::min(1.f, (float)((now - t0) / dur));
        const float e = Motion::Ease(Motion::Curve::EaseOut, u);
        x = fx + (tx - fx) * e;
        y = fy + (ty - fy) * u;
        const float arc = (st == Out ? 150.f : 210.f) * 4.f * u * (1.f - u);
        lift = arc;
        // A stretch on the way up, a full turn on the way home.
        sy = 1.f + 0.12f * (1.f - fabsf(1.f - 2.f * u));
        sx = 1.f / sy;
        if (st == Back) rot = 360.f * Motion::Ease(Motion::Curve::Swift, u) * (dir > 0 ? 1.f : -1.f);
        if (u >= 1.f) {
            if (st == Out) { st = Walk; landed = now; Pick(now); Sound::Play(Sound::Voice::Seat, { 0.6f }); }
            else { st = Home; arrived = now; Sound::Play(Sound::Voice::Select); return; }
        }
    } else {
        // Walking about.
        if (now >= actEnd) Pick(now);
        const float t = (float)now;
        if (act == 0) {
            x += dir * 58.f * dt;
            if (x < 50.f) { x = 50.f; dir = 1; act = 1; actEnd = now + 0.6; }
            if (x > SCREEN_W - 50.f) { x = SCREEN_W - 50.f; dir = -1; act = 1; actEnd = now + 0.6; }
            lift = fabsf(sinf(t * 9.f)) * 6.f;              // a bouncy step
            rot = sinf(t * 9.f) * 5.f;
        } else if (act == 2) {
            dir = sinf(t * 4.f) > 0 ? 1.f : -1.f;           // looking this way, then that
        } else if (act == 3) {
            sy = 0.84f; sx = 1.1f;                           // sitting
        } else if (act == 4) {
            const float u = 1.f - (float)((actEnd - now) / 0.6);
            lift = 46.f * 4.f * u * (1.f - u);              // a little hop
        }
        // Landing: squashed for a moment.
        const float lu = (float)((now - landed) / 0.3);
        if (lu >= 0 && lu < 1) { const float k = (1.f - lu) * 0.25f; sy *= 1.f - k; sx *= 1.f + k; }
        // Breathing when still.
        if (act != 0 && act != 4) { const float b = sinf(t * 3.f) * 0.02f; sy *= 1.f + b; }
    }
    // Its shadow on the floor, smaller the higher it is.
    const float sh = std::max(0.35f, 1.f - lift / 220.f);
    const float floorY = st == Back ? y : std::max(y, kFloor);
    FillRectRounded((int)(x - 30 * sh), (int)(floorY - 5), (int)(60 * sh), 10, 5, SDL_Color{ 0, 0, 0, (Uint8)(70 * sh) });
    const float w = kW * sx, h = kH * sy;
    const SDL_FRect r{ x - w / 2.f, y - h - lift, w, h };
    const SDL_FPoint pivot{ w / 2.f, h };
    SDL_RenderCopyExF(gRen, tex, nullptr, &r, rot, &pivot, dir < 0 ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
}
}  // namespace Pet

// fold (the island's sidebar folding, 0..1): the words fade and the logo
// shrinks to the folded column's middle (foldCx). textRight: where the words
// must stop (0: anywhere).
static void Brand(int x, int y, const std::string& sub, float fold = 0.f, int foldCx = 0, int textRight = 0) {
    int tx = x + 56;
    const float wk = std::max(0.f, 1.f - fold * 2.5f);          // the words, fading first
    if (Art::Has(Art::Id::Logo)) {
        // open: 86 x 54 at (x - 6, y - 6); folded: 70 x 44 in the column's middle
        const float lw = 86.f - 16.f * fold, lh = 54.f - 10.f * fold;
        const float lx = (x - 6) + ((foldCx - 35) - (x - 6)) * fold, ly = (y - 6) + 2.f * fold;
        const SDL_Rect lr{ (int)lroundf(lx), (int)lroundf(ly), (int)lroundf(lw), (int)lroundf(lh) };
        const float ks = lw / 86.f;
        Pet::home = lr;
        Pet::homeSeen = Motion::Now();
        if (Pet::st == Pet::Home) {
            // A soft shadow under the logo so it sits on any theme; back from
            // a walk, a happy little bounce.
            const float bt = (float)((Motion::Now() - Pet::arrived) / 0.6);
            const float k = bt >= 0 && bt < 1 && !Motion::Reduced() ? sinf(bt * 3.14159f * 3.f) * (1.f - bt) * 0.12f : 0.f;
            FillRectRounded(lr.x + (int)(12 * ks), lr.y + (int)(12 * ks), (int)(66 * ks), (int)(36 * ks), (int)(18 * ks), SDL_Color{ 0, 0, 0, 40 });
            PushXform(0, 0, 1.f + k, lr.x + 43.f * ks, lr.y + 54.f * ks, 1.f);
            Art::Draw(Art::Id::Logo, lr);
            PopXform();
        } else {
            // Its place, kept for it.
            StrokeRectRounded(lr.x + 8.f * ks, lr.y + 8.f * ks, 70.f * ks, 38.f * ks, 14.f * ks, 1.5f, Alpha(gTheme.text, 50));
        }
        tx = x + 86;
    } else {
        const int bx = (int)lroundf(x + ((foldCx - 21) - x) * fold);
        FillRectRounded(bx, y, 42, 42, 13, gTheme.gold);
        UiIcons::Draw(UiIcons::Id::Users, bx + 21, y + 21, 24, ColorLuma(gTheme.gold) > 150 ? SDL_Color{ 14, 14, 22, 255 } : SDL_Color{ 250, 250, 255, 255 });
    }
    if (wk <= 0.01f) return;
    SDL_Rect outer{};
    const bool clipped = SDL_RenderIsClipEnabled(gRen) == SDL_TRUE;
    if (clipped) SDL_RenderGetClipRect(gRen, &outer);
    if (textRight > tx) {
        const SDL_Rect clip{ tx, y - 12, textRight - tx, 60 };
        SDL_RenderSetClipRect(gRen, &clip);
    }
    DrawText("TomoToolNX", tx, y - 1, Alpha(gTheme.text, (int)(gTheme.text.a * wk)), Font::Row);
    DrawText(sub, tx, y + 23, Alpha(gTheme.faint, (int)(gTheme.faint.a * wk)), Font::Xs);
    if (textRight > tx) SDL_RenderSetClipRect(gRen, clipped ? &outer : nullptr);
}

// The wallpaper each place shows (the game's own patterns): what the place is
// about, tinted with the theme. Settings, Look can pin one or turn them off.
static Art::Id WallpaperForPlace(Page p);
// The music each place plays (the game's own pieces, see music.h); nullptr
// on pages that only pass by, which keep what plays.
static const char* MusicForPlace(Page p);

#include "page_settings.inc"
#include "page_start.inc"
#include "page_editor.inc"
#include "page_player.inc"
#include "page_textures.inc"
#include "page_miis.inc"
#include "page_miis2.inc"
#include "page_map.inc"
#include "page_islandgen.inc"

static Art::Id WallpaperForPlace(Page p) {
    if (gWallpaper == "off") return Art::Id::Count;
    for (const auto& w : kWallpapers) if (gWallpaper == w.code && w.pat != Art::Id::Count) return w.pat;
    switch (p) {
        case Page::Editor:
            switch (EdActive()) {
                case OnSwitchMode::MiiStats: return Art::Id::PatFace;
                case OnSwitchMode::UGC:
                    switch (gUgcKindFilter) {
                        case 0:  return Art::Id::PatFood;
                        case 1:  return Art::Id::PatClothes;
                        case 2:  return Art::Id::PatGoods;
                        case 3:  return Art::Id::PatHouse;
                        case 4:  return Art::Id::PatBuilding;
                        case 5:  return Art::Id::PatBuilding;
                        case 6:  return Art::Id::PatGround;
                        case 7:  return Art::Id::PatFace;
                        default: return Art::Id::PatCoordinate;
                    }
                case OnSwitchMode::Player:   return Art::Id::PatHouse;
                case OnSwitchMode::Map:      return Art::Id::PatGround;
                case OnSwitchMode::Backups:  return Art::Id::PatPhoto;
                case OnSwitchMode::Phone:    return Art::Id::PatItemBook;
                case OnSwitchMode::Settings: return Art::Id::PatItemBook;
                default:                     return Art::Id::PatIslandEdit;   // Home
            }
        case Page::Settings:        return Art::Id::PatItemBook;
        case Page::UpdateAvailable: return Art::Id::PatItemBook;
        case Page::Error:
        case Page::UpdateCheck:
        case Page::Applet:          return Art::Id::PatQuiz;
        case Page::BackupPrompt:
        case Page::BackingUp:
        case Page::Saved:           return Art::Id::PatPhoto;
        case Page::NewIsland:
        case Page::ApplyIsland:
        case Page::IslandGen:
        case Page::IslandGenBatch:
        case Page::IslandGenRun:    return Art::Id::PatBuilding;
        case Page::NoSave:          return Art::Id::PatHouse;
        default:                    return Art::Id::PatIslandEdit;            // choose a user
    }
}

// Night as the game has it (Parameter/ClockSystem): from 19:00 to 5:00.
static bool IsNight() {
    const time_t t = time(nullptr);
    struct tm lt;
    if (!localtime_r(&t, &lt)) return false;
    return lt.tm_hour >= 19 || lt.tm_hour < 5;
}
// The Miis: the Mii editor's music, its second take while the Mii's side has
// the highlight (after a moment there, so passing through does not flip it).
static bool MiiMusicTakeB() {
    static bool b = false;
    static double since = -1;
    float fx = 0, fy = 0;
    const bool detail = !sMiiOverview && Focus::FocusCentre(fx, fy) && fx * SCREEN_W >= MiiDX();   // (0..1 across)
    if (detail == b) { since = -1; return b; }
    const double now = Motion::Now();
    if (since < 0) since = now;
    else if (now - since > 0.6) { b = detail; since = -1; }
    return b;
}
static const char* MusicForPlace(Page p) {
    if (gMusicPick != "auto")
        for (int i = 0; i < Music::kPieceCount; i++)
            if (gMusicPick == Music::kPieces[i].code) return Music::kPieces[i].file;
    const bool night = IsNight();
    switch (p) {
        case Page::Start:           return "BGM_Field03_Title_Basic";          // the title screen's
        case Page::Settings:        return "BGM_Spot_Fountain_01";
        case Page::UpdateAvailable: return "BGM_News_Topic_Happy_01";          // the Mii News
        case Page::NoSave:
        case Page::NewIsland:
        case Page::ApplyIsland:
        case Page::IslandGen:
        case Page::IslandGenBatch:
        case Page::IslandGenRun:    return night ? "BGM_Field06_IslandEdit" : "BGM_Field03_IslandEdit";
        case Page::Editor:
            switch (EdActive()) {
                case OnSwitchMode::MiiStats: return MiiMusicTakeB() ? "BGM_MiiEditor_01b" : "BGM_MiiEditor_01a";
                case OnSwitchMode::UGC:
                    return (sTexDetail || gOnSwitchMode == OnSwitchMode::TexSettings) ? "BGM_UGCEditor_02b" : "BGM_UGCEditor_02a";
                case OnSwitchMode::Player:   return "BGM_PhotoStudio";
                case OnSwitchMode::Map:      return night ? "BGM_Field06_IslandEdit" : "BGM_Field03_IslandEdit";
                case OnSwitchMode::Backups:  return "BGM_Zukan_01";               // the encyclopedia's
                case OnSwitchMode::Phone:    return "BGM_Local_Communication_01";
                case OnSwitchMode::Settings: return "BGM_Spot_Fountain_01";
                default:                     return night ? "BGM_Field06_Basic" : "BGM_Field03_Basic";   // Home: the island
            }
        default:                    return nullptr;   // errors, checks, backing up: what plays goes on
    }
}

// ── Quit (first screens) ─────────────────────────────────────────────────────
static void QuitDialog() {
    W::Dialog(Ui::HashStr("quit.dialog"), UiIcons::Id::Exit, Ui::Tone::Neutral, Tx("quit.title"), Tx("quit.body"),
              { { Tx("quit.stay"), W::Kind::Neutral, []() { sQuitAsk = false; Motion::Forget(Ui::HashStr("quit.dialog")); } },
                { Tx("quit.quit"), W::Kind::Primary, []() { gQuitApp = true; } } },
              0, []() { sQuitAsk = false; Motion::Forget(Ui::HashStr("quit.dialog")); }, Tx("quit.stay"));
}

// ── The buttons panel (−) ────────────────────────────────────────────────────
// Every button of the page being shown, in words: what the hint bar says, and
// the ones that are always there.
static bool sKeysOpen = false;
static std::vector<Focus::Hint> sKeysLast, sKeysShown;

static void KeysPanel() {
    const Ui::Id id = Ui::HashStr("keys.panel");
    const double since = Motion::FirstSeen(id), now = Motion::Now();
    if (now - since < 0.0001) Sound::Play(Sound::Voice::Menu);
    struct Row { std::string key, text; };
    std::vector<Row> rows;
    rows.push_back({ "\xE2\x9C\x9B", Tx("keys.move") });
    for (const auto& h : sKeysShown) {
        if (h.key == "-") continue;
        if (h.key == "\xE2\x97\x80\xE2\x96\xB6") rows.push_back({ "\xE2\x97\x80\xE2\x96\xB6", Tx("keys.change") });
        else rows.push_back({ h.key, h.label });
    }
    rows.push_back({ "RS", Tx("keys.scroll") });
    rows.push_back({ "\xE2\x98\x9D", Tx("keys.touch") });
    rows.push_back({ "-", Tx("keys.this") });
    const int rowH = 46, w = 640, h = 96 + (int)rows.size() * rowH + 24;
    const int x = (SCREEN_W - w) / 2, y = std::max(20, (SCREEN_H - h) / 2);
    Focus::PushLayer(Focus::Layer::Keys, []() { sKeysOpen = false; Motion::Forget(Ui::HashStr("keys.panel")); }, Tx("hint.close"), SDL_Rect{ x, y, w, h });
    DimScreen((int)(140 * std::min(1.f, (float)((now - since) / 0.16))));
    const float e = Motion::Reduced() ? 1.f : Motion::Ease(Motion::Curve::Spring, std::min(1.f, (float)((now - since) / 0.42)));
    PushXform(0, 14.f * (1 - e), 0.98f + 0.02f * e, SCREEN_W / 2.f, SCREEN_H / 2.f, std::min(1.f, e * 1.4f));
    FillRectRounded(x, y + 10, w, h, 26, SDL_Color{ 0, 0, 0, 60 });
    FillRectRounded(x, y, w, h, 26, Mix(gTheme.panel, gTheme.text, 0.03f));
    DrawRectRounded(x, y, w, h, 26, Alpha(gTheme.border, 230));
    W::IconTile(x + 28, y + 24, 52, UiIcons::Id::Info, gTheme.gold);
    DrawTextVC(Tx("keys.title"), x + 96, y + 50, gTheme.text, Font::Title);
    int ry = y + 96;
    for (size_t i = 0; i < rows.size(); i++) {
        const float rt = Motion::Reduced() ? 1.f : std::max(0.f, std::min(1.f, (float)((now - since - 0.05 - i * 0.02) / 0.3)));
        PushXform(0, 6.f * (1 - rt), 1, 0, 0, rt);
        const std::string& k = rows[i].key;
        // The console's button shapes, as in the hint bar, centred in a 64 px column.
        const int col = x + 30, kcy = ry + 23;
        const SDL_Color face = Alpha(gTheme.text, 225);
        if (k == "\xE2\x9C\x9B") {                     // the whole D-pad
            FillRectRounded(col + 64 / 2 - 4, kcy - 13, 8, 26, 3, face);
            FillRectRounded(col + 64 / 2 - 13, kcy - 4, 26, 8, 3, face);
        } else if (k == "\xE2\x98\x9D") {              // touch
            UiIcons::Draw(UiIcons::Id::Phone, col + 64 / 2, kcy, 22, gTheme.text);
        } else if (k == "RS") {                        // the right stick
            FillDisc((float)(col + 32), (float)kcy, 13.f, face);
            FillDisc((float)(col + 32), (float)kcy, 9.f, Alpha(gTheme.bg, 255));
            DrawTextC("R", col + 32, kcy, gTheme.text, Font::Xs);
        } else {
            W::ButtonGlyph(k, col + 32 - W::ButtonGlyphW(k) / 2, kcy);
        }
        W::Marquee(Ui::IdMix(Ui::HashStr("keys.row"), (uint32_t)i), rows[i].text, col + 64 + 18, kcy, w - 140, gTheme.dim, Font::Body, true, W::Anchor::Middle);
        PopXform();
        ry += rowH;
    }
    PopXform();
    Focus::PopLayer();
}

// ── The frame ────────────────────────────────────────────────────────────────
static Page PageFor(Screen s) {
    switch (s) {
        case Screen::Settings:        return Page::Settings;
        case Screen::UserPick:        return Page::Start;
        case Screen::BackupPrompt:    return Page::BackupPrompt;
        case Screen::BackingUp:       return Page::BackingUp;
        case Screen::Error:           return Page::Error;
        case Screen::UpdateCheck:     return Page::UpdateCheck;
        case Screen::UpdateAvailable: return Page::UpdateAvailable;
        case Screen::AppletWarning:   return Page::Applet;
        case Screen::SaveFeedback:    return Page::Saved;
        case Screen::IslandGenPrompt: return Page::NoSave;
        case Screen::BundleCreate:    return Page::NewIsland;
        case Screen::BundlePick:      return Page::ApplyIsland;
        case Screen::IslandGenConfig: return Page::IslandGen;
        case Screen::IslandGenBatchLtd: return Page::IslandGenBatch;
        case Screen::IslandGenRun:    return Page::IslandGenRun;
        case Screen::OnSwitch:
            // The editor's places built on the 2.0 kit (and the very first
            // frame, which opens on Home). The others still draw themselves,
            // with their own pop-ups.
            if (!gEditorOpen) return Page::Editor;
            if (gShowFileBrowser || gShowThumbTip || gShowBgRemovePrompt || gShowUgcKindPicker || gShowSaveWarning || gShowBackPrompt)
                return Page::None;
            return EdOwnsMode(gOnSwitchMode) ? Page::Editor : Page::None;
        default:                      return Page::None;
    }
}

static bool Owns() { return PageFor(gScreen) != Page::None; }

// Pages where + must not interrupt (something is being written).
static bool Busy(Page p) { return p == Page::BackingUp || p == Page::Saved || p == Page::IslandGenRun; }

// The app opening. The first frames are slow (every font and icon is prepared
// the first time it is drawn): a sound started then played over a frozen
// screen, its "settle" notes landed before any card could be seen, and the
// load starved the mixer. So the first page waits, hidden, until frames come
// at a steady pace; then its cards drop in with LLM-Selfbot's opening (dawn,
// then a settle per card), picture and sound together.
static bool   sOpened = false;
static int    sSteadyFrames = 0;
static double sFirstFrameAt = -1;
static void OpeningStep() {
    if (sOpened) return;
    const double now = Motion::Now();
    if (sFirstFrameAt < 0) sFirstFrameAt = now;
    sSteadyFrames = Motion::Dt() < 1.f / 30.f ? sSteadyFrames + 1 : 0;
    if (sSteadyFrames >= 3 || now - sFirstFrameAt > 1.6) {
        sOpened = true;
        W::Arrive();
        Sound::Play(Sound::Voice::Dawn);
        Sound::Arrivals(3, 520.f);
    }
}

// While the Mii pictures are being made: a chip in the top right corner
// with how far along it is.
static void MiiPackChip() {
    if (MiiPack::GetState() != MiiPack::State::Running) return;
    const float p = MiiPack::Progress();
    char pct[16];
    snprintf(pct, sizeof pct, "%d%%", (int)(p * 100.f));
    const std::string label = Tx("miipack.making");
    const int barW = 54, h = 36;
    const int w = 16 + 20 + 10 + TextW(label, Font::Xs) + 12 + barW + 10 + TextW("100%", Font::Xs) + 16;
    const int x = SCREEN_W - w - 22, y = 14;
    FillRectRounded(x, y + 6, w, h, h / 2, SDL_Color{ 0, 0, 0, 70 });
    FillRectRounded(x, y, w, h, h / 2, Alpha(gTheme.panel, 245));
    DrawRectRounded(x, y, w, h, h / 2, Alpha(gTheme.border, 220));
    W::Spinner(x + 26, y + h / 2, 8, gTheme.gold);
    int tx = x + 46;
    DrawTextVC(label, tx, y + h / 2, gTheme.dim, Font::Xs);
    tx += TextW(label, Font::Xs) + 12;
    FillRectRounded(tx, y + h / 2 - 3, barW, 6, 3, Alpha(gTheme.text, 30));
    FillRectRounded(tx, y + h / 2 - 3, std::max(6, (int)(barW * p)), 6, 3, gTheme.gold);
    DrawTextVC(pct, tx + barW + 10, y + h / 2, gTheme.text, Font::Xs);
}

static void Frame(const Input::Frame& in) {
    History::Tick();
    // An open island keeps its own work going whatever is shown.
    if (gScreen == Screen::OnSwitch) EditorPump();
    Page want = PageFor(gScreen);
    if ((in.down & Input::Plus) && !Busy(want)) {
        if (want == Page::Editor) {
            // + in the editor: "Leave the island?" (never straight out).
            if (!sLeaveAsk) { sLeaveAsk = true; Motion::Forget(Ui::HashStr("ed.leave.dlg")); Motion::Forget(Ui::HashStr("ed.leave.dlg2")); }
        } else {
            // + asks before quitting; + again (or Quit) quits. Nothing is
            // open on these pages, so quitting loses nothing.
            if (sQuitAsk) { gQuitApp = true; return; }
            sQuitAsk = true;
        }
    }
    Pet::Input();                  // the logo out for a walk takes its own taps
    Focus::Resolve(in);
    if (gQuitApp) return;
    // A page change since last frame (by a press just now, or by the old
    // screens): the content arrives and the highlight goes back to where it
    // was on that page.
    want = PageFor(gScreen);
    if (want != Page::None) {
        if (const char* m = MusicForPlace(want)) Music::Want(m);
    }
    if (want == Page::None) { sShown = Page::None; sQuitAsk = false; return; }   // the old screens take it from here
    // In the editor each place is its own page (focus memory, arrival).
    static OnSwitchMode sShownMode = OnSwitchMode::WebUI;
    const bool modeChanged = want == Page::Editor && EdActive() != sShownMode;
    if (want != sShown || gScreen != sShownScreen || modeChanged) {
        // A page the user went to whooshes in; one that came by itself (the
        // update check ending, a backup finishing) just arrives.
        const bool userCaused = in.down != 0 || in.tap;
        if (sOpened && sShown != Page::None && userCaused) {
            Sound::Play(Sound::Voice::Whoosh);
            Sound::Arrivals(2, 120.f);
        }
        if (want != Page::Editor) { sLeaveAsk = false; sWarnAsk = false; }
        sKeysOpen = false;
        sShown = want;
        sShownScreen = gScreen;
        sShownMode = EdActive();
        sSetLastFocus = 0;
        if (sOpened) W::Arrive(); else W::HoldArrival();
        // In the editor the sidebar stays where it is from place to place: a
        // highlight on it (a tap on an item, L / R from it) stays put instead
        // of going back to where it last was on the new place.
        const Ui::Id onSide = want == Page::Editor && IsSidebarId(Focus::Current()) ? Focus::Current() : 0;
        Focus::EnterPage(want == Page::Editor ? 0x100u + (uint32_t)EdActive() : (uint32_t)want);
        if (onSide) Focus::Set(onSide);
    }
    OpeningStep();
    Focus::BeginBuild();
    Tour::BeginRender(gRen);   // recording: this frame is drawn into a texture
    // A frame starts unclipped, whatever the last one left behind.
    SDL_RenderSetClipRect(gRen, nullptr);
    ClearScreen();
    EdLayout();                    // the island's sidebar: open, folded, or gliding between
    // The place's pattern drifting behind the page (not behind a sidebar).
    {
        const int wallX = want == Page::Settings ? kSideW : want == Page::Editor ? sEdSideW : 0;
        Art::SetWallpaper(WallpaperForPlace(want));
        Art::DrawWallpaper(SDL_Rect{ wallX, 0, SCREEN_W - wallX, SCREEN_H });
    }
    if (want == Page::Editor) PlSyncSkinTone();
    switch (want) {
        case Page::Settings:        PageSettings(); break;
        case Page::Start:           PageStart(); break;
        case Page::BackupPrompt:    PageBackupPrompt(); break;
        case Page::BackingUp:       PageBackingUp(); break;
        case Page::Error:           PageError(); break;
        case Page::UpdateCheck:     PageUpdateCheck(); break;
        case Page::UpdateAvailable: PageUpdateAvailable(); break;
        case Page::Applet:          PageApplet(); break;
        case Page::Saved:           PageSaved(); break;
        case Page::NoSave:          PageNoSave(); break;
        case Page::NewIsland:       PageNewIsland(); break;
        case Page::ApplyIsland:     PageApplyIsland(); break;
        case Page::Editor:          PageEditor(); break;
        case Page::IslandGen:       PageIslandGen(); break;
        case Page::IslandGenBatch:  PageIslandGenBatch(); break;
        case Page::IslandGenRun:    PageIslandGenRun(); break;
        default: break;
    }
    W::DrawChoice();                // a list a row opened, over the page
    W::DrawCalendar();
    if (sQuitAsk && !Busy(want)) QuitDialog();
    // −: every button here, in words (unless the page uses − itself).
    if (!sKeysOpen && !Focus::LayerOpen() && !Focus::HasCommand(Input::Minus))
        Focus::Command(Input::Minus, Tx("keys.short"), []() { sKeysShown = sKeysLast; sKeysOpen = true; Motion::Forget(Ui::HashStr("keys.panel")); });
    if (sKeysOpen) KeysPanel();
    Focus::DrawRings();            // the highlight's outline, over everything on the page
    std::vector<Focus::Hint> hints;
    Focus::Hints(hints);
    if (!sKeysOpen) sKeysLast = hints;
    const int sideW = want == Page::Settings ? kSideW : want == Page::Editor ? sEdSideW : 0;
    W::HintBar(hints, sideW + 36, SCREEN_W - 36, SCREEN_H - 30, Tx("hint.change"));
    MiiPackChip();
    Pet::Draw();
    Focus::EndBuild();
    float fx, fy;
    if (Focus::FocusCentre(fx, fy)) SetBackdropFocus(fx, fy);
    Ui::ToastDraw();
    Tour::Capture(gRen);   // the README clips (debug only)
    SDL_RenderPresent(gRen);
    Motion::Sweep();
    if (gSoundVolumeDirty && !(in.held & (Input::Left | Input::Right))) { gSoundVolumeDirty = false; SaveConfig(); }
    if (!Owns()) { sShown = Page::None; sQuitAsk = false; }
}

}  // namespace S2
