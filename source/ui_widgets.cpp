// ui_widgets.cpp — TomoToolNX. See ui_widgets.h.
//
// Sizes are LLM-Selfbot's desktop panel scaled about ×1.4 (it is designed for
// a monitor at arm's length; this is a 720p screen across a room or in hand).

#include "ui_widgets.h"
#include "ui_input.h"
#include "ui_art.h"
#include "ui_motion.h"
#include "ui_sound.h"
#include "i18n.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace W {
namespace {
double sArrived = -10;
double sContentArrived[4] = { -10, -10, -10, -10 };   // parts of the page changing (ArriveContent)
double sControlsArrived = -10; // when switches and sliders last made their entrance
bool   sArriveSound = true;
int    sSwitchRun = 0;        // switches sliding on, this arrival
int    sSliderRun = 0;

struct SwitchState { float from = 0, to = 0; double start = -1; bool init = false; };
struct SliderState { float shown = 0, from = 0, to = 0; double start = -1; float dur = 0.2f; bool init = false; float lastNotchFrac = -1; bool sweeping = false; };
struct ValueState  { std::string last; double bump = -10; };
struct GlideState  { Motion::Tween x, w; bool init = false; };
struct MarkState   { Motion::Tween y, h; double land = -10; SDL_Rect last{}; bool init = false; };
struct PopState    { double since = -1; };
std::unordered_map<Id, SwitchState> sSwitch;
std::unordered_map<Id, SliderState> sSlider;
std::unordered_map<Id, ValueState>  sValue;
std::unordered_map<Id, GlideState>  sGlide;
std::unordered_map<Id, MarkState>   sMarks;
std::unordered_map<Id, Id>          sActiveIcon;    // group → id of the item that last became active
std::unordered_map<Id, double>      sIconPop;

SDL_Color InkOn(SDL_Color c) { return ColorLuma(c) > 150 ? SDL_Color{ 14, 14, 22, 255 } : SDL_Color{ 250, 250, 255, 255 }; }
SDL_Color Ink()   { return gTheme.text; }
SDL_Color Muted() { return gTheme.dim; }
SDL_Color Faint() { return gTheme.faint; }
SDL_Color Acc()   { return gTheme.gold; }

std::string T(const char* key) { return Lang::T(key); }

void Value(Id id, const std::string& text, int x, int cy, Font f, SDL_Color col, bool right) {
    ValueState& v = sValue[id];
    if (v.last != text) { if (!v.last.empty()) v.bump = Motion::Now(); v.last = text; }
    float s = 1.f; SDL_Color c = col;
    const float bt = (float)((Motion::Now() - v.bump) / 0.18);
    if (bt >= 0 && bt < 1 && !Motion::Reduced()) {
        const float k = 1.f - Motion::Ease(Motion::Curve::Swift, bt);
        s = 1.f + 0.1f * k;
        c = Mix(col, Acc(), k);
    }
    const int w = TextW(text, f);
    const int cx = right ? x - w / 2 : x + w / 2;
    PushXform(0, 0, s, (float)cx, (float)cy, 1.f);
    if (right) DrawTextR(text, x, cy, c, f); else DrawTextVC(text, x, cy, c, f);
    PopXform();
}

void RowBar(Id id, SDL_Rect r) {
    // LLM-Selfbot's row accent: a 2 px bar on the left that grows when the row
    // is focused.
    static std::unordered_map<Id, double> since;
    double& s = since[id];
    if (!Focus::Is(id)) { s = -1; return; }
    if (s < 0) s = Motion::Now();
    const float e = Motion::Reduced() ? 1.f : Motion::Ease(Motion::Curve::Spring, (float)((Motion::Now() - s) / 0.3));
    const int full = r.h - 22;
    const int h = (int)(full * (0.35f + 0.65f * std::min(1.f, e)));
    FillRectRounded(r.x + 6, r.y + (r.h - h) / 2, 3, h, 2, Alpha(Acc(), 230));
}

// The common part of a row: node, plate, bar, title and description.
void RowBase(Id id, SDL_Rect r, const RowText& t, uint16_t flags, std::function<void()> press,
             std::function<void(int)> adjust, const std::string& hint, int rightReserve,
             SDL_Rect ctl = SDL_Rect{}, int pivot = -1, int dead = 0, int leftReserve = 0) {
    Focus::Node n;
    n.id = id; n.r = r; n.radius = 14; n.flags = flags; n.press = std::move(press); n.adjust = std::move(adjust); n.hint = hint;
    n.ctl = ctl; n.pivot = pivot; n.dead = dead;
    Focus::Add(std::move(n));
    Focus::DrawPlate(id);
    RowBar(id, r);
    const int tx = r.x + 22 + leftReserve, tw = r.w - 44 - rightReserve - leftReserve;
    const bool focused = Focus::Is(id);
    // Too long for the row: the end shows while the row has the highlight.
    if (t.desc.empty()) {
        Marquee(id, t.title, tx, r.y + r.h / 2, tw, focused ? Ink() : Mix(Ink(), Muted(), 0.15f), Font::Row, focused, Anchor::Middle);
    } else {
        const int th = TextH(Font::Row), dh = TextH(Font::Body);
        const int top = r.y + (r.h - (th + dh + 2)) / 2;
        Marquee(id, t.title, tx, top, tw, focused ? Ink() : Mix(Ink(), Muted(), 0.15f), Font::Row, focused);
        Marquee(Ui::IdMix(id, 0xDE5Cu), t.desc, tx, top + th + 2, tw, Muted(), Font::Body, focused);
    }
}

void ArrowButton(int cx, int cy, bool left, bool lit) {
    FillDisc((float)cx, (float)cy, 17.f, lit ? Alpha(Acc(), 60) : Alpha(Ink(), 16));
    UiIcons::Draw(left ? UiIcons::Id::ChevronLeft : UiIcons::Id::ChevronRight, cx, cy, 20, lit ? Ink() : Muted());
}

}  // namespace

std::string Ellipsize(const std::string& s, Font f, int w) { return FitText(s, f, w); }

// ── Arrival ──────────────────────────────────────────────────────────────────
void Arrive(bool withSound) {
    sArrived = Motion::Now();
    sControlsArrived = sArrived;
    sArriveSound = withSound;
    sSwitchRun = 0;
    sSliderRun = 0;
    sSwitch.clear();
    sSlider.clear();
}
void ArriveContent(int part) {
    const int p = part < 0 ? 0 : part > 3 ? 3 : part;
    sContentArrived[p] = Motion::Now();
    sControlsArrived = sContentArrived[p];
    sArriveSound = false;
    sSwitchRun = 0;
    sSliderRun = 0;
    sSwitch.clear();
    sSlider.clear();
}
void HoldArrival() {
    // Blocks stay hidden (and switches and sliders silent) until Arrive().
    sArrived = 1e12;
    sControlsArrived = sArrived;
    sArriveSound = false;
    sSwitch.clear();
    sSlider.clear();
}
double ArrivedAt() { return sArrived; }

void BeginBlock(SDL_Rect r) {
    if (Motion::Reduced()) {
        const float a = std::max(0.f, std::min(1.f, (float)((Motion::Now() - sArrived) / 0.2)));
        PushXform(0, 0, 1, 0, 0, a);
        return;
    }
    const double delay = std::min(0.24, std::max(0, r.y - 70) * 0.0003 + std::min(std::max(0, r.x - 280), 600) * 0.00006);
    const float t = (float)((Motion::Now() - sArrived - delay) / 0.44);
    if (t >= 1.f) { PushXform(0, 0, 1, 0, 0, 1); return; }
    const float e = Motion::Ease(Motion::Curve::Spring, std::max(0.f, t));
    const float a = std::max(0.f, std::min(1.f, e));
    PushXform(0, -12.f * (1 - e), 0.975f + 0.025f * e, r.x + r.w / 2.f, r.y + r.h / 2.f, a);
}
// The part that changed (a new tab): it fades in and settles from a little
// below, on its own, while the rest of the page stays still.
void BeginContentBlock(SDL_Rect r, int part) {
    const double since = std::max(sArrived, sContentArrived[part < 0 ? 0 : part > 3 ? 3 : part]);
    if (since == sArrived) { BeginBlock(r); return; }   // the whole page is arriving anyway
    const float t = (float)((Motion::Now() - since) / 0.30);
    if (t >= 1.f) { PushXform(0, 0, 1, 0, 0, 1); return; }
    if (Motion::Reduced()) { PushXform(0, 0, 1, 0, 0, std::max(0.f, std::min(1.f, t * 1.5f))); return; }
    const float e = Motion::Ease(Motion::Curve::Swift, std::max(0.f, t));
    PushXform(0, 10.f * (1 - e), 1, r.x + r.w / 2.f, r.y + r.h / 2.f, std::max(0.f, std::min(1.f, e * 1.15f)));
}
void EndBlock() { PopXform(); }

void BeginPress(Id id, SDL_Rect r) {
    const float s = Motion::PressScale(id);
    PushXform(0, 0, s, r.x + r.w / 2.f, r.y + r.h / 2.f, 1);
}
void EndPress() { PopXform(); }

// ── Surfaces ─────────────────────────────────────────────────────────────────
void Card(SDL_Rect r) {
    FillRectRounded(r.x, r.y, r.w, r.h, 20, Alpha(gTheme.panel, 222));
    DrawRectRounded(r.x, r.y, r.w, r.h, 20, Alpha(Mix(gTheme.border, Ink(), 0.05f), 210));
}
void Hairline(int x, int y, int w) { FillRectSquare(x, y, w, 1, Alpha(gTheme.border, 170)); }
void Eyebrow(int x, int y, const std::string& text, SDL_Color c) { DrawText(UpperUtf8(text), x, y, c, Font::Xs); }
void IconTile(int x, int y, int size, UiIcons::Id icon, SDL_Color tint) {
    FillRectRounded(x, y, size, size, size * 3 / 10, Alpha(tint, 34));
    if (icon != UiIcons::Id::Count) UiIcons::Draw(icon, x + size / 2, y + size / 2, size * 46 / 100, tint);
}

int PillW(const std::string& text, bool dot) { return TextW(text, Font::Xs) + 24 + (dot ? 14 : 0); }
void Pill(int x, int y, const std::string& text, Ui::Tone tone, bool dot) {
    const SDL_Color c = Ui::ToneColor(tone);
    const int w = PillW(text, dot), h = 28;
    FillRectRounded(x, y, w, h, h / 2, tone == Ui::Tone::Neutral ? Alpha(Ink(), 16) : Alpha(c, 40));
    int tx = x + 12;
    if (dot) { FillDisc((float)(x + 15), (float)(y + h / 2), 4.f, c); tx += 14; }
    DrawTextVC(text, tx, y + h / 2, tone == Ui::Tone::Neutral ? Muted() : Mix(c, Ink(), 0.25f), Font::Xs);
}

int PageHeader(int x, int y, int w, UiIcons::Id icon, const std::string& eyebrow, const std::string& title,
               const std::string& desc, const std::string& pill, Font titleFont, std::function<void()> back) {
    const int tile = 56;
    if (back) {
        Button(Ui::HashStr("page.back"), { x, y + 2, tile, tile }, std::string(), Kind::Neutral,
               [back]() { Sound::Play(Sound::Voice::Release); back(); }, UiIcons::Id::ChevronLeft);
    } else {
        IconTile(x, y + 2, tile, icon, Acc());
    }
    const int tx = x + tile + 20;
    const int pw = pill.empty() ? 0 : W::PillW(pill) + 16;
    const int tw = w - (tx - x) - pw;
    int ty = y;
    if (!eyebrow.empty()) { Eyebrow(tx, ty, eyebrow, Faint()); ty += TextH(Font::Xs) + 2; }
    // A page's title is alone in its place: too long, it runs by on its own.
    Marquee(Ui::IdMix(Ui::HashStr("page.title"), (uint32_t)std::hash<std::string>()(title)), title, tx, ty, tw, Ink(), titleFont, true);
    ty += TextH(titleFont);
    if (!desc.empty()) {
        for (const std::string& l : Ui::WrapLines(desc, Font::Body, tw, 2)) { DrawText(l, tx, ty, Muted(), Font::Body); ty += TextH(Font::Body); }
    }
    if (!pill.empty()) W::Pill(x + w - W::PillW(pill), y + 4, pill, Ui::Tone::Neutral);
    return std::max(y + tile + 4, ty) + 18;
}

// ── Sidebar ──────────────────────────────────────────────────────────────────
void SidebarGroup(int x, int y, const std::string& label, float alpha) {
    if (alpha <= 0.01f) return;
    const SDL_Color c = Faint();
    Eyebrow(x, y, label, Alpha(c, (int)(c.a * std::min(1.f, alpha))));
}

void SidebarMarks(Id groupId, SDL_Rect a, bool follow) {
    MarkState& m = sMarks[groupId];
    const double now = Motion::Now();
    if (!m.init) { m.init = true; m.y.Snap((float)a.y); m.h.Snap((float)a.h); m.land = now; m.last = a; }
    if (follow) { m.y.Snap((float)a.y); m.h.Snap((float)a.h); m.last = a; }
    else if (a.y != m.last.y || a.h != m.last.h) {
        m.y.To((float)a.y, 0.4f, Motion::Curve::Swift);
        m.h.To((float)a.h, 0.3f, Motion::Curve::Swift);
        m.land = now; m.last = a;
    }
    // The pill lands where the item is (scale 0.94 × 0.7 → 1, 0.34 s, swift).
    float e = Motion::Reduced() ? 1.f : Motion::Ease(Motion::Curve::Swift, (float)((now - m.land) / 0.34));
    e = std::min(1.f, e);
    const int pw = (int)(a.w * (0.94f + 0.06f * e)), ph = (int)(a.h * (0.7f + 0.3f * e));
    const int px = a.x + (a.w - pw) / 2, py = a.y + (a.h - ph) / 2;
    FillRectRounded(px, py, pw, ph, 13, Alpha(Acc(), (int)(41 * e)));
    DrawRectRounded(px, py, pw, ph, 13, Alpha(Acc(), (int)(56 * e)));
    // The bar slides from the previous item and glows.
    const int by = (int)m.y.Value(), bh = (int)m.h.Value();
    const int bx = a.x - 12;
    FillRectRounded(bx - 3, by + 2, 10, bh - 4, 5, Alpha(Acc(), 40));
    FillRectRounded(bx, by + 4, 4, bh - 8, 2, Acc());
}

void SidebarItem(Id id, SDL_Rect r, UiIcons::Id icon, const std::string& label, bool active, bool dot,
                 std::function<void()> go, float words) {
    words = std::min(1.f, std::max(0.f, words));
    Focus::Node n;
    n.id = id; n.r = r; n.radius = 13; n.press = std::move(go); n.hint = T("hint.open");
    Focus::Add(std::move(n));
    BeginPress(id, r);
    Focus::DrawPlate(id);
    // The icon pops when the item becomes the active one.
    float s = 1.f;
    if (active) {
        double& t = sIconPop[id];
        if (t == 0) t = Motion::Now();
        const float u = (float)((Motion::Now() - t) / 0.5);
        if (u < 1 && !Motion::Reduced()) {
            // 0.7 → 1.18 (55 %) → 1, on --jelly
            const float j = Motion::Ease(Motion::Curve::Jelly, u);
            s = 0.7f + 0.3f * j;
        }
    } else {
        sIconPop[id] = 0;
    }
    const SDL_Color ic = active ? Acc() : (Focus::Is(id) ? Ink() : Muted());
    PushXform(0, 0, s, (float)(r.x + 26), (float)(r.y + r.h / 2), 1);
    UiIcons::Draw(icon, r.x + 26, r.y + r.h / 2, 24, ic);
    PopXform();
    if (words > 0.01f && r.w - 70 > 8) {
        const SDL_Color tc = active || Focus::Is(id) ? Ink() : Muted();
        Marquee(id, label, r.x + 52, r.y + r.h / 2, r.w - 70, Alpha(tc, (int)(tc.a * words)), Font::Row, Focus::Is(id) && words > 0.99f, Anchor::Middle);
    }
    if (dot) {
        // at the end of the words; on the icon's shoulder when they are gone
        const float dx = (r.x + r.w - 18) * words + (r.x + 38) * (1.f - words);
        const float dy = (r.y + r.h / 2) * words + (r.y + 11) * (1.f - words);
        FillDisc(dx, dy, 5.f, gTheme.warn);
    }
    EndPress();
}

// ── Rows ─────────────────────────────────────────────────────────────────────
namespace {
// How far a long text has run (px), or -1 at rest: a pause, a steady run to
// the end, a pause, a glide back, and again.
float MarqueeRun(Id id, int over, bool active) {
    static std::unordered_map<Id, double> since;
    double& t0 = since[id];
    if (!active || over <= 0 || Motion::Reduced()) { t0 = -1; return -1.f; }
    if (t0 < 0) t0 = Motion::Now();
    const float speed = 60.f, hold0 = 0.9f, hold1 = 1.3f, back = 0.45f;
    const float run = over / speed, period = hold0 + run + hold1 + back;
    const float t = (float)fmod(Motion::Now() - t0, (double)period);
    if (t < hold0) return 0.f;
    if (t < hold0 + run) return (t - hold0) * speed;
    if (t < hold0 + run + hold1) return (float)over;
    return over * (1.f - Motion::Ease(Motion::Curve::Swift, (t - hold0 - run - hold1) / back));
}
// `text` from `x` at the vertical middle `cy`, `off` px along, shown only
// between x and x + w.
void RunText(const std::string& text, int x, int cy, int w, float off, SDL_Color col, Font f) {
    SDL_Rect outer{};
    const bool oc = SDL_RenderIsClipEnabled(gRen) == SDL_TRUE;
    if (oc) SDL_RenderGetClipRect(gRen, &outer);
    const int th = TextH(f);
    SDL_Rect clip = XformRect(x, cy - th, w, th * 2);
    if (oc && !SDL_IntersectRect(&clip, &outer, &clip)) return;
    SDL_RenderSetClipRect(gRen, &clip);
    DrawTextVC(text, x - (int)lroundf(off), cy, col, f);
    if (oc) SDL_RenderSetClipRect(gRen, &outer); else SDL_RenderSetClipRect(gRen, nullptr);
}
}  // namespace

void MarqueeR(Id id, const std::string& text, int rx, int cy, int maxW, SDL_Color col, Font f, bool active) {
    const int w = TextW(text, f);
    const float off = maxW > 8 ? MarqueeRun(id, w - maxW, active) : -1.f;
    if (off < 0) { DrawTextR(w <= maxW ? text : Ellipsize(text, f, maxW), rx, cy, col, f); return; }
    RunText(text, rx - maxW, cy, maxW, off, col, f);
}

void Marquee(Id id, const std::string& text, int x, int y, int maxW, SDL_Color col, Font f, bool active, Anchor a) {
    const int w = TextW(text, f);
    const float off = maxW > 8 ? MarqueeRun(id, w - maxW, active) : -1.f;
    if (off < 0) {
        const std::string s = w <= maxW ? text : Ellipsize(text, f, maxW);
        if (a == Anchor::Top) DrawText(s, x, y, col, f);
        else if (a == Anchor::Middle) DrawTextVC(s, x, y, col, f);
        else DrawTextC(s, x, y, col, f);
        return;
    }
    const int cy = a == Anchor::Top ? y + TextH(f) / 2 : y;
    RunText(text, a == Anchor::Centre ? x - maxW / 2 : x, cy, maxW, off, col, f);
}

void RowLink(Id id, SDL_Rect r, const RowText& t, const std::string& value, std::function<void()> press,
             UiIcons::Id icon, bool danger, uint16_t flags, bool valueInk) {
    const int vw = value.empty() ? 0 : std::min(TextW(value, Font::Body), r.w / 3);
    BeginPress(id, r);
    RowBase(id, r, t, flags, std::move(press), nullptr, T("hint.open"), vw + 40);
    const int cy = r.y + r.h / 2;
    int x = r.x + r.w - 22;
    if (icon != UiIcons::Id::Count) { UiIcons::Draw(icon, x - 10, cy, 22, danger ? gTheme.red : Faint()); x -= 32; }
    // Too long for its place: it scrolls by while the row is highlighted.
    if (!value.empty()) MarqueeR(id, value, x, cy, r.w / 3, valueInk ? Ink() : Muted(), valueInk ? Font::Row : Font::Body, Focus::Is(id));
    if (danger) {
        // Danger rows show their title in red.
    }
    EndPress();
}

void RowToggle(Id id, SDL_Rect r, const RowText& t, bool on, std::function<void(bool)> set) {
    // The row itself stays still (squashing the whole box read as a wobble):
    // the knob slides and the row lights up softly for a moment.
    static std::unordered_map<Id, double> flash;
    auto it = flash.find(id);
    if (it != flash.end()) {
        const float k = (float)((Motion::Now() - it->second) / 0.45);
        if (k >= 1.f) flash.erase(it);
        else FillRectRounded(r.x, r.y, r.w, r.h, 14, Alpha(Acc(), (int)(46.f * (1.f - k) * (1.f - k))));
    }
    RowBase(id, r, t, Focus::OwnSound,
            [set, on, id]() { Sound::Play(on ? Sound::Voice::Off : Sound::Voice::On); flash[id] = Motion::Now(); set(!on); }, nullptr,
            T(on ? "hint.turn_off" : "hint.turn_on"), 90);
    SwitchDraw(id, r.x + r.w - 22 - 58, r.y + r.h / 2, on, Focus::Is(id));
}

int SwatchDots(int x, int cy, const std::vector<SDL_Color>& dots, int r) {
    if (dots.empty()) return 0;
    const int step = r + r / 2 + 1;              // overlapping a little
    // Right to left, so the first colour sits on top; each with a thin edge in
    // the card colour so they read as separate dots.
    for (int i = (int)dots.size() - 1; i >= 0; i--) {
        const float cx = (float)(x + r + i * step);
        FillDisc(cx, (float)cy, (float)r + 1.6f, Alpha(gTheme.panel, 255));
        FillDisc(cx, (float)cy, (float)r, Alpha(dots[i], 255));
    }
    return 2 * r + (int)(dots.size() - 1) * step;
}

void RowStepper(Id id, SDL_Rect r, const RowText& t, const std::string& value, float frac, float stepFrac,
                std::function<void(int)> step, std::function<void()> press, const std::string& pressHint, bool enabled,
                const std::vector<SDL_Color>& dots, Art::Id art) {
    if (!enabled) {
        // Shown for what it says, skipped by the highlight.
        PushXform(0, 0, 1, 0, 0, 0.45f);
        RowBase(id, r, t, Focus::Disabled, nullptr, nullptr, std::string(), std::max(110, TextW(value, Font::Row) + 24) + 96);
        const int cy = r.y + r.h / 2, right = r.x + r.w - 22;
        const int vw = std::max(110, TextW(value, Font::Row) + 24);
        DrawTextVC(value, right - 42 - vw / 2 - TextW(value, Font::Row) / 2, cy, Muted(), Font::Row);
        PopXform();
        return;
    }
    auto adjust = [step, frac, stepFrac](int d) {
        if ((d < 0 && frac <= 0.0001f) || (d > 0 && frac >= 0.9999f)) {
            Sound::Play(Sound::Voice::Seat, { d < 0 ? 0.f : 1.f, true });
            return;
        }
        step(d);
        const float p = std::max(0.f, std::min(1.f, frac + d * stepFrac));
        Sound::Play(Sound::Voice::Tick, { p });
    };
    // Narrow rows (side panels) keep more room for the title.
    // A picture before the value (a flag), else colour dots (a theme).
    const bool hasArt = art != Art::Id::Count && Art::Has(art);
    const int dotsW = hasArt ? 30 + 10 : dots.empty() ? 0 : 2 * 7 + (int)(dots.size() - 1) * 11 + 10;
    const int tw = TextW(value, Font::Row);
    const int vw = std::max(r.w < 420 ? 64 : 110, tw + 24 + dotsW);
    const int right = r.x + r.w - 22;
    // Touch: the left arrow steps down, the right arrow up; with a list behind
    // the value, a tap on the value opens it.
    const SDL_Rect ctl{ right - vw - 84, r.y, vw + 84 + 22, r.h };
    RowBase(id, r, t, Focus::AdjustX, press, adjust, press ? pressHint : std::string(), vw + 96, ctl,
            right - 42 - vw / 2, press ? vw : 0);
    const int cy = r.y + r.h / 2;
    const bool f = Focus::Is(id);
    ArrowButton(right - 17, cy, false, f);
    const int left = right - 34 - 8 - vw / 2 - (dotsW + tw) / 2;
    if (hasArt) Art::Draw(art, SDL_Rect{ left, cy - 15, 30, 30 });
    else if (dotsW) SwatchDots(left, cy, dots);
    Value(id, value, left + dotsW, cy, Font::Row, f ? Ink() : Mix(Ink(), Muted(), 0.2f), false);
    ArrowButton(right - 42 - vw - 8 - 17, cy, true, f);
}

void RowItem(Id id, SDL_Rect r, SDL_Texture* pic, const RowText& t, const std::vector<SDL_Color>& colours, int sel,
             std::function<void(int)> step, std::function<void()> press, const std::string& pressHint) {
    const int n = (int)colours.size();
    const bool many = n > 1 && step != nullptr;
    const int dotR = 9, gap = 24;
    const int dotsW = n > 0 ? (n - 1) * gap + 2 * dotR : 0;
    const int right = r.x + r.w - 22;
    const int vw = many ? dotsW + 20 : dotsW;
    auto adjust = [step, sel, n](int d) {
        if ((d < 0 && sel <= 0) || (d > 0 && sel >= n - 1)) { Sound::Play(Sound::Voice::Seat, { d < 0 ? 0.f : 1.f, true }); return; }
        step(d);
        Sound::Play(Sound::Voice::Tick, { n > 1 ? (float)(sel + d) / (n - 1) : 0.5f });
    };
    const int ps = r.h - 14;                       // the picture's tile
    const SDL_Rect ctl{ right - vw - 84, r.y, vw + 84 + 22, r.h };
    BeginPress(id, r);
    RowBase(id, r, t, many ? Focus::AdjustX : 0, press, many ? std::function<void(int)>(adjust) : nullptr, press ? pressHint : std::string(),
            many ? vw + 96 : (n == 1 ? dotsW + 50 : 40), many ? ctl : SDL_Rect{}, right - 42 - vw / 2, press && many ? vw : 0, ps + 12);
    const int cy = r.y + r.h / 2;
    const bool f = Focus::Is(id);
    const SDL_Rect pb{ r.x + 14, r.y + 7, ps, ps };
    FillRectRounded(pb.x, pb.y, pb.w, pb.h, 12, Alpha(Ink(), f ? 22 : 14));
    if (pic) {
        // A little bigger when highlighted, as if picked up.
        const int g = f ? 1 : 4;
        Art::DrawFit(pic, SDL_Rect{ pb.x + g, pb.y + g, pb.w - 2 * g, pb.h - 2 * g });
    } else {
        DashRectRounded((float)pb.x + 1.f, (float)pb.y + 1.f, (float)pb.w - 2.f, (float)pb.h - 2.f, 11.f, 1.5f, 6.f, 5.f, Alpha(Ink(), 70));
        UiIcons::Draw(UiIcons::Id::Plus, pb.x + pb.w / 2, pb.y + pb.h / 2, 20, Alpha(Ink(), 110));
    }
    if (n > 0) {
        const int x0 = (many ? right - 42 - vw / 2 : right - 28 - (press ? 22 : 0) - dotsW / 2) - dotsW / 2 + dotR;
        for (int i = 0; i < n; i++) {
            const float dx = (float)(x0 + i * gap);
            const SDL_Color c = colours[i];
            FillDisc(dx, (float)cy + 1.5f, (float)dotR, SDL_Color{ 0, 0, 0, 60 });
            FillDisc(dx, (float)cy, (float)dotR, SDL_Color{ c.r, c.g, c.b, 255 });
            StrokeArc(dx, (float)cy, (float)dotR - 0.5f, 1.f, 0.f, 360.f, Alpha(Ink(), 50), Alpha(Ink(), 50));
            if (i == sel && n > 1) StrokeArc(dx, (float)cy, (float)dotR + 4.f, 2.5f, 0.f, 360.f, f ? Acc() : Alpha(Ink(), 200), f ? Acc() : Alpha(Ink(), 200));
        }
    }
    if (many) {
        ArrowButton(right - 17, cy, false, f);
        ArrowButton(right - 42 - vw - 8 - 17, cy, true, f);
    } else if (press) {
        UiIcons::Draw(UiIcons::Id::ChevronRight, right - 10, cy, 22, Faint());
    }
    EndPress();
}

void RowSlider(Id id, SDL_Rect r, const RowText& t, float frac, float stepFrac, const std::string& value,
               std::function<void(int)> step, std::function<void()> press) {
    auto adjust = [step, frac, stepFrac](int d) {
        if ((d < 0 && frac <= 0.0001f) || (d > 0 && frac >= 0.9999f)) {
            Sound::Play(Sound::Voice::Seat, { d < 0 ? 0.f : 1.f, true });
            return;
        }
        step(d);
        Sound::Play(Sound::Voice::Tick, { std::max(0.f, std::min(1.f, frac + d * stepFrac)) });
    };
    const int sw = 240, vw = 78;
    const int cy = r.y + r.h / 2, right = r.x + r.w - 22;
    const int rx = right - vw;
    // Touch: a tap on the groove steps toward the tap.
    const int gx = rx - 18 - sw;
    // A press (A, or a tap on the readout) types the value.
    const bool typed = press != nullptr;
    RowBase(id, r, t, Focus::AdjustX, std::move(press), adjust, typed ? T("hint.type") : std::string(), sw + vw + 30,
            SDL_Rect{ gx - 20, r.y, sw + 40 + (typed ? vw + 18 : 0), r.h }, gx + (int)(sw * std::max(0.f, std::min(1.f, frac))),
            0);
    // readout
    FillRectRounded(rx, cy - 17, vw, 34, 11, Alpha(Ink(), 12));
    Value(id, value, rx + vw / 2 - TextW(value, Font::Body) / 2, cy, Font::Body, Ink(), false);
    SliderDraw(id, rx - 18 - sw, cy, sw, frac, Focus::Is(id));
}

void RowSegments(Id id, SDL_Rect r, const RowText& t, const std::vector<std::string>& options, int current,
                 std::function<void(int)> pick) {
    const int n = (int)options.size();
    if (n <= 0) return;
    const int cur = std::max(0, std::min(n - 1, current)), gap = 6, padX = 16, h = 36;
    std::vector<int> ws(n);
    int total = 0;
    for (int i = 0; i < n; i++) { ws[i] = TextW(options[i], Font::Body) + 2 * padX; total += ws[i] + (i ? gap : 0); }
    const int right = r.x + r.w - 22, x0 = right - total, cy = r.y + r.h / 2;
    std::vector<SDL_Rect> seg(n);
    for (int i = 0, x = x0; i < n; i++) { seg[i] = SDL_Rect{ x, cy - h / 2, ws[i], h }; x += ws[i] + gap; }
    auto adjust = [seg, cur, n, pick](int d) {
        const Input::Frame& in = Input::Last();
        if (in.tap || in.released) {                    // a finger: the one under it
            for (int i = 0; i < n; i++)
                if (in.x >= seg[i].x - gap / 2 && in.x < seg[i].x + seg[i].w + gap / 2) {
                    if (i != cur) { pick(i); Sound::Play(Sound::Voice::Tick, { n > 1 ? (float)i / (n - 1) : 0.5f }); }
                    return;
                }
        }
        const int k = cur + d;
        if (k < 0 || k >= n) { Sound::Play(Sound::Voice::Seat, { d < 0 ? 0.f : 1.f, true }); return; }
        pick(k);
        Sound::Play(Sound::Voice::Tick, { n > 1 ? (float)k / (n - 1) : 0.5f });
    };
    RowBase(id, r, t, Focus::AdjustX, nullptr, adjust, std::string(), total + 20,
            SDL_Rect{ x0 - 10, r.y, total + 20, r.h }, seg[cur].x + seg[cur].w / 2, seg[cur].w);
    // The track, then the chosen one sliding to its place.
    FillRectRounded(x0 - 4, cy - h / 2 - 4, total + 8, h + 8, (h + 8) / 2, Alpha(Ink(), 12));
    static std::unordered_map<Id, Motion::Tween> slide;
    Motion::Tween& g = slide[id];
    const float want = (float)seg[cur].x;
    if (g.t0 < 0 && g.to == 0) g.Snap(want);
    if (g.to != want) g.To(want, 0.3f, Motion::Curve::Spring);
    const float sx = Motion::Reduced() ? want : g.Value();
    const bool f = Focus::Is(id);
    FillRectRounded((int)lroundf(sx), seg[cur].y, seg[cur].w, h, h / 2, f ? Acc() : Alpha(Acc(), 200));
    for (int i = 0; i < n; i++) {
        const bool on = i == cur;
        const SDL_Color ink = on ? (ColorLuma(Acc()) > 150 ? SDL_Color{ 20, 18, 28, 255 } : SDL_Color{ 250, 250, 255, 255 }) : Muted();
        DrawTextC(options[i], seg[i].x + seg[i].w / 2, cy, ink, Font::Body);
    }
}

void RowSwatch(Id id, SDL_Rect r, const RowText& t, SDL_Color swatch, const std::string& value, std::function<void()> press) {
    BeginPress(id, r);
    RowBase(id, r, t, 0, std::move(press), nullptr, T("hint.change"), 200);
    const int cy = r.y + r.h / 2, right = r.x + r.w - 22;
    FillRectRounded(right - 56, cy - 17, 56, 34, 11, swatch);
    DrawRectRounded(right - 56, cy - 17, 56, 34, 11, Alpha(Ink(), 40));
    if (!value.empty()) DrawTextR(value, right - 70, cy, Muted(), Font::Body);
    EndPress();
}

void RowSwatches(Id id, SDL_Rect r, const RowText& t, const SDL_Color* colors, int count, int selected,
                 std::function<void(int)> step, std::function<void()> press, const std::string& pressHint) {
    const int d = 34, gap = 10;
    const int right = r.x + r.w - 22, total = count * d + (count - 1) * gap, x0 = right - total;
    const int sel = std::max(0, std::min(count - 1, selected));
    const float frac = count > 1 ? (float)sel / (count - 1) : 0.f;
    auto adjust = [step, frac, count](int dd) {
        if ((dd < 0 && frac <= 0.0001f) || (dd > 0 && frac >= 0.9999f)) {
            Sound::Play(Sound::Voice::Seat, { dd < 0 ? 0.f : 1.f, true });
            return;
        }
        step(dd);
        Sound::Play(Sound::Voice::Tick, { std::max(0.f, std::min(1.f, frac + dd / (float)std::max(1, count - 1))) });
    };
    RowBase(id, r, t, Focus::AdjustX, std::move(press), adjust, pressHint, total + 20,
            SDL_Rect{ x0 - 10, r.y, total + 20, r.h }, x0 + sel * (d + gap) + d / 2);
    const int cy = r.y + r.h / 2;
    // The ring glides to the chosen colour.
    static std::unordered_map<Id, Motion::Tween> ring;
    Motion::Tween& g = ring[id];
    const float want = (float)(x0 + sel * (d + gap) + d / 2);
    if (g.t0 < 0 && g.to == 0) g.Snap(want);
    if (g.to != want) g.To(want, 0.36f, Motion::Curve::Spring);
    const float rx = Motion::Reduced() ? want : g.Value();
    for (int i = 0; i < count; i++) {
        const float cx = (float)(x0 + i * (d + gap) + d / 2);
        FillDisc(cx, (float)cy, d / 2.f, colors[i]);
    }
    const bool f = Focus::Is(id);
    StrokeArc(rx, (float)cy, d / 2.f + 5.f, 3.f, 0.f, 360.f, f ? Acc() : Alpha(Ink(), 160), f ? Acc() : Alpha(Ink(), 160));
}

// ── Controls ─────────────────────────────────────────────────────────────────
void SwitchDraw(Id id, int x, int cy, bool on, bool focused) {
    SwitchState& s = sSwitch[id];
    const double now = Motion::Now();
    const float target = on ? 1.f : 0.f;
    if (!s.init) {
        s.init = true;
        s.from = s.to = target; s.start = -1;
        // Arriving on: slide on one after another, a note higher each time.
        if (on && !Motion::Reduced() && now >= sControlsArrived && now - sControlsArrived < 0.25) {
            const int k = sSwitchRun++;
            const double delay = 0.03 + k * 0.04;
            s.from = 0; s.to = 1; s.start = now + delay;
            if (sArriveSound) Sound::Play(Sound::Voice::Slide, { std::min(k, 7) / 7.f, false, 1.f, (float)(delay * 1000 + 30) });
        }
    }
    if (s.to != target) {
        float cur = s.to;
        if (s.start >= 0) {
            const float u = (float)((now - s.start) / 0.3);
            cur = u <= 0 ? s.from : (u >= 1 ? s.to : s.from + (s.to - s.from) * Motion::Ease(Motion::Curve::Knob, u));
        }
        s.from = cur; s.to = target; s.start = Motion::Reduced() ? -1 : now;
    }
    float pos = s.to;
    if (s.start >= 0) {
        const float u = (float)((now - s.start) / (s.from == 0 && s.to == 1 && now - sControlsArrived < 1.5 ? 0.5 : 0.3));
        pos = u <= 0 ? s.from : (u >= 1 ? s.to : s.from + (s.to - s.from) * Motion::Ease(Motion::Curve::Knob, u));
    }
    const int w = 58, h = 34, k = 26, pad = 4;
    const float t = std::max(0.f, std::min(1.f, pos));
    FillRectRounded(x, cy - h / 2, w, h, h / 2, Alpha(Ink(), 22));
    FillRectRounded(x, cy - h / 2, w, h, h / 2, Alpha(Mix(gTheme.panel, Acc(), 0.75f), (int)(255 * t)));
    DrawRectRounded(x, cy - h / 2, w, h, h / 2, Alpha(t > 0.5f ? Acc() : Ink(), focused ? 120 : 30));
    const float kx = x + pad + pos * (w - k - pad * 2) + k / 2.f;
    FillDisc(kx, cy + 1.5f, k / 2.f + 1, SDL_Color{ 0, 0, 0, 70 });
    FillDisc(kx, (float)cy, k / 2.f, Mix(Mix(Ink(), gTheme.panel, 0.5f), SDL_Color{ 243, 244, 248, 255 }, t));
}

void SliderDraw(Id id, int x, int cy, int w, float frac, bool focused) {
    SliderState& s = sSlider[id];
    const double now = Motion::Now();
    frac = std::max(0.f, std::min(1.f, frac));
    if (!s.init) {
        s.init = true;
        s.shown = s.from = s.to = frac;
        if (!Motion::Reduced() && now >= sControlsArrived && now - sControlsArrived < 0.25 && frac > 0.02f) {
            // The opening sweep: from the start to its value in a second.
            s.from = 0; s.to = frac; s.start = now + 0.05 + sSliderRun++ * 0.06; s.dur = 1.0f; s.sweeping = true;
            s.lastNotchFrac = 0;
            if (sArriveSound) Sound::Play(Sound::Voice::Zip, { frac, false, 1.f, (float)((s.start - now) * 1000) });
        }
    }
    if (s.to != frac && !s.sweeping) { s.from = s.shown; s.to = frac; s.start = now; s.dur = 0.2f; }
    if (s.start >= 0) {
        const float u = (float)((now - s.start) / s.dur);
        if (u >= 1) { s.shown = s.to; s.start = -1;
            if (s.sweeping) { s.sweeping = false; if (sArriveSound) Sound::Play(Sound::Voice::Seat, { s.to }); } }
        else if (u > 0) {
            s.shown = s.from + (s.to - s.from) * Motion::Ease(s.sweeping ? Motion::Curve::QuintOut : Motion::Curve::Swift, u);
            if (s.sweeping && sArriveSound) {
                const int a = (int)(s.lastNotchFrac * 20), b = (int)(s.shown * 20);
                if (b > a) Sound::Play(Sound::Voice::Wind, { s.shown, b % 5 == 0 });
                s.lastNotchFrac = s.shown;
            }
        } else s.shown = s.from;
    }
    const float v = s.shown;
    const int th = 14;
    FillRectRounded(x, cy - th / 2, w, th, th / 2, Alpha(Ink(), 18));
    const int fw = (int)(w * v);
    if (fw > 0) FillRectRounded(x, cy - th / 2, std::max(fw, th), th, th / 2, Mix(gTheme.accent, Acc(), 0.55f + 0.45f * v));
    for (int i = 1; i <= 3; i++) FillRectSquare(x + w * i / 4 - 1, cy - th / 2 + 3, 2, th - 6, Alpha(SDL_Color{ 255, 255, 255, 255 }, 34));
    const float kx = x + w * v;
    if (focused) FillDisc(kx, (float)cy, 25.f, Alpha(Acc(), 38));
    FillDisc(kx, (float)cy + 2, 16.f, SDL_Color{ 0, 0, 0, 90 });
    FillDisc(kx, (float)cy, 15.f, Acc());
    FillDisc(kx, (float)cy, 11.f, SDL_Color{ 20, 22, 34, 255 });
    FillDisc(kx, (float)cy, 3.6f, Acc());
}

// ── Buttons, chips, tiles ────────────────────────────────────────────────────
void Button(Id id, SDL_Rect r, const std::string& label, Kind kind, std::function<void()> press,
            UiIcons::Id icon, bool enabled, uint16_t flags) {
    Focus::Node n;
    // The highlight is a ring a few pixels outside the button: a plate behind a
    // filled button read as a dark halo.
    n.id = id; n.r = { r.x - 5, r.y - 5, r.w + 10, r.h + 10 }; n.radius = 19;
    n.flags = (uint16_t)(flags | Focus::Ring | (enabled ? 0 : Focus::Disabled));
    n.press = std::move(press); n.hint = T("hint.select");
    Focus::Add(std::move(n));
    BeginPress(id, r);
    Focus::DrawPlate(id);
    const bool f = Focus::Is(id);
    SDL_Color fill, ink;
    switch (kind) {
        case Kind::Primary: fill = f ? Mix(Acc(), SDL_Color{ 255, 255, 255, 255 }, 0.12f) : Acc(); ink = InkOn(Acc()); break;
        case Kind::Danger:  fill = Mix(gTheme.panel, gTheme.red, f ? 0.32f : 0.2f); ink = Mix(gTheme.red, Ink(), 0.2f); break;
        case Kind::Soft:    fill = Mix(gTheme.panel, Acc(), f ? 0.32f : 0.2f); ink = Mix(Acc(), Ink(), 0.35f); break;
        default:            fill = Mix(gTheme.panel, Ink(), f ? 0.14f : 0.08f); ink = Ink(); break;
    }
    if (!enabled) { fill = Alpha(fill, 110); ink = Faint(); }
    FillRectRounded(r.x, r.y, r.w, r.h, 14, fill);
    if (kind == Kind::Neutral) DrawRectRounded(r.x, r.y, r.w, r.h, 14, Alpha(Ink(), 22));
    const Font fnt = r.h >= 50 ? Font::Row : Font::Body;
    if (icon != UiIcons::Id::Count && !label.empty()) {
        const int gap = 10, is = 22;
        if (TextW(label, fnt) > r.w - 64) {
            // Too long: the icon at the left, the words running by beside it
            // while the button has the highlight.
            const int left = r.x + 16;
            UiIcons::Draw(icon, left + is / 2, r.y + r.h / 2, is, ink);
            Marquee(id, label, left + is + gap, r.y + r.h / 2, r.w - 32 - is - gap, ink, fnt, f, Anchor::Middle);
        } else {
            const int tw = TextW(label, fnt);
            const int left = r.x + (r.w - (is + gap + tw)) / 2;
            UiIcons::Draw(icon, left + is / 2, r.y + r.h / 2, is, ink);
            DrawTextVC(label, left + is + gap, r.y + r.h / 2, ink, fnt);
        }
    } else if (icon != UiIcons::Id::Count) {
        UiIcons::Draw(icon, r.x + r.w / 2, r.y + r.h / 2, 24, ink);
    } else {
        Marquee(id, label, r.x + r.w / 2, r.y + r.h / 2, r.w - 24, ink, fnt, f, Anchor::Centre);
    }
    EndPress();
}

namespace {
constexpr int kChipIcon = 24, kChipIconGap = 8;
bool ChipHasIcon(const std::vector<Art::Id>& icons, size_t i) {
    return i < icons.size() && icons[i] != Art::Id::Count && Art::Has(icons[i]);
}
// Width of a row of chips whose labels get `padX` of room around them.
int ChipsWidth(const std::vector<std::pair<std::string, int>>& items, int padX, const std::vector<Art::Id>& icons) {
    int w = 12;                                   // 6 in front, 6 behind
    for (size_t i = 0; i < items.size(); i++) {
        w += TextW(items[i].first, Font::Body) + padX + (i ? 6 : 0);
        if (items[i].second >= 0) w += TextW(std::to_string(items[i].second), Font::Xs) + 22;
        if (ChipHasIcon(icons, i)) w += kChipIcon + kChipIconGap;
    }
    return w;
}
}  // namespace

int ChipsW(const std::vector<std::pair<std::string, int>>& items, const std::vector<Art::Id>& icons) {
    return ChipsWidth(items, 36, icons);
}

void Chips(Id id, int x, int y, const std::vector<std::pair<std::string, int>>& items, int selected,
           std::function<void(int)> pick, int maxW, const std::vector<Art::Id>& icons) {
    const int h = 56, ch = 44, pad = 6;
    // Fit the room: first by giving each label less padding, and only when
    // that is not enough, by sliding the row (with the chip in use in view).
    int padX = 36;
    int full = ChipsWidth(items, padX, icons);
    if (maxW > 0) while (full > maxW && padX > 18) { padX -= 2; full = ChipsWidth(items, padX, icons); }
    const int total = maxW > 0 ? std::min(maxW, full) : full;
    FillRectRounded(x, y, total, h, 20, Alpha(Ink(), 9));
    DrawRectRounded(x, y, total, h, 20, Alpha(gTheme.border, 190));
    static std::unordered_map<Id, Motion::Tween> slide;
    int shift = 0;
    auto chipW = [&](size_t i) {
        int w = TextW(items[i].first, Font::Body) + padX;
        if (items[i].second >= 0) w += TextW(std::to_string(items[i].second), Font::Xs) + 22;
        if (ChipHasIcon(icons, i)) w += kChipIcon + kChipIconGap;
        return w;
    };
    if (full > total) {
        int fx = -1, fw = 0, cx0 = pad;
        for (size_t i = 0; i < items.size(); i++) {
            const int w = chipW(i);
            if (Focus::Is(Ui::IdMix(id, (uint32_t)i)) || (fx < 0 && (int)i == selected)) { fx = cx0; fw = w; }
            cx0 += w + 6;
        }
        Motion::Tween& t = slide[id];
        float want = t.to;
        if (fx >= 0) {
            if (fx - want < 24) want = (float)std::max(0, fx - 24);
            if (fx + fw - want > total - 24) want = (float)(fx + fw - (total - 24));
        }
        want = std::max(0.f, std::min((float)(full - total), want));
        if (want != t.to) t.To(want, 0.3f, Motion::Curve::Swift);
        shift = (int)(Motion::Reduced() ? t.to : t.Value());
    }
    std::vector<SDL_Rect> rs;
    int cx = x + pad - shift;
    for (size_t i = 0; i < items.size(); i++) {
        const int w = chipW(i);
        rs.push_back({ cx, y + (h - ch) / 2, w, ch });
        cx += w + 6;
    }
    // Everything stays inside the track.
    SDL_Rect oldClip{};
    const bool hadClip = SDL_RenderIsClipEnabled(gRen);
    if (hadClip) SDL_RenderGetClipRect(gRen, &oldClip);
    {
        SDL_Rect c = XformRect(x + 2, y + 2, total - 4, h - 4);
        if (hadClip) SDL_IntersectRect(&c, &oldClip, &c);
        SDL_RenderSetClipRect(gRen, &c);
    }
    // The gliding highlight behind the selected chip, inside its own pill.
    if (selected >= 0 && selected < (int)rs.size()) {
        GlideState& g = sGlide[id];
        const SDL_Rect s = rs[selected];
        if (!g.init) { g.init = true; g.x.Snap((float)s.x); g.w.Snap((float)s.w); }
        g.x.To((float)s.x, 0.46f, Motion::Curve::Spring);
        g.w.To((float)s.w, 0.46f, Motion::Curve::Spring);
        const int gx = (int)g.x.Value(), gw = (int)g.w.Value();
        // One flat pill (an inset gradient read as a box inside the box).
        FillRectRounded(gx, s.y, gw, s.h, 16, Alpha(Acc(), 72));
        StrokeRectRounded(gx + 0.5f, s.y + 0.5f, gw - 1.f, s.h - 1.f, 15.5f, 1.2f, Alpha(Acc(), 150));
    }
    for (size_t i = 0; i < items.size(); i++) {
        const Id cid = Ui::IdMix(id, (uint32_t)i);
        const SDL_Rect r = rs[i];
        Focus::Node n;
        // A ring around the chip, chosen or not, so the highlight glides from
        // tab to tab instead of vanishing on the chosen one.
        n.id = cid; n.r = { r.x - 3, r.y - 3, r.w + 6, r.h + 6 }; n.radius = 19;
        n.flags = Focus::OwnSound | Focus::Ring; n.hint = T("hint.show");
        const int idx = (int)i;
        n.press = [pick, idx]() { Sound::Play(Sound::Voice::Select); pick(idx); };
        Focus::Add(std::move(n));
        BeginPress(cid, r);
        const bool sel = (int)i == selected, f = Focus::Is(cid);
        Focus::DrawPlate(cid);
        if (f && !sel) FillRectRounded(r.x, r.y, r.w, r.h, 16, Alpha(Ink(), 14));
        const SDL_Color ink = sel || f ? Ink() : Muted();
        int lx = r.x + padX / 2;
        if (ChipHasIcon(icons, i)) {
            // The game's art in front of the label: line icons in the label's
            // colour, pictures as they are (a little dimmed when not chosen).
            const SDL_Rect ib{ lx - 2, r.y + (r.h - kChipIcon) / 2, kChipIcon, kChipIcon };
            if (Art::IsMask(icons[i])) Art::Draw(icons[i], ib, ink);
            else Art::Draw(icons[i], ib, SDL_Color{ 255, 255, 255, (Uint8)(sel || f ? 255 : 190) });
            lx += kChipIcon + kChipIconGap;
        }
        DrawTextVC(items[i].first, lx, r.y + r.h / 2, ink, Font::Body);
        if (items[i].second >= 0) {
            const std::string c = std::to_string(items[i].second);
            const int bw = TextW(c, Font::Xs) + 14;
            const int bx = r.x + r.w - padX / 2 + 4 - bw;
            FillRectRounded(bx, r.y + r.h / 2 - 11, bw, 22, 11, sel ? Alpha(Acc(), 70) : Alpha(Ink(), 16));
            DrawTextC(c, bx + bw / 2, r.y + r.h / 2, sel ? Ink() : Faint(), Font::Xs);
        }
        EndPress();
    }
    if (hadClip) SDL_RenderSetClipRect(gRen, &oldClip); else SDL_RenderSetClipRect(gRen, nullptr);
}

void Tile(Id id, SDL_Rect r, UiIcons::Id icon, const std::string& title, const std::string& sub, std::function<void()> press,
          SDL_Color tint) {
    Focus::Node n;
    n.id = id; n.r = { r.x - 5, r.y - 5, r.w + 10, r.h + 10 }; n.radius = 24; n.press = std::move(press); n.hint = T("hint.open");
    n.flags = Focus::Ring;
    Focus::Add(std::move(n));
    BeginPress(id, r);
    Focus::DrawPlate(id);
    Card(r);
    IconTile(r.x + 18, r.y + (r.h - 52) / 2, 52, icon, tint);
    const int tx = r.x + 18 + 52 + 16, tw = r.w - (tx - r.x) - 16;
    const bool tf = Focus::Is(id);
    if (sub.empty()) Marquee(id, title, tx, r.y + r.h / 2, tw, Ink(), Font::Row, tf, Anchor::Middle);
    else {
        const int th = TextH(Font::Row), sh = TextH(Font::Body);
        const int top = r.y + (r.h - th - sh) / 2;
        Marquee(id, title, tx, top, tw, Ink(), Font::Row, tf);
        Marquee(Ui::IdMix(id, 0xDE5Cu), sub, tx, top + th, tw, Muted(), Font::Body, tf);
    }
    EndPress();
}

// Where rect `a` is drawn under PushXform(0, dy, s, centre of r): a card that
// lifts hands this to the plate, so the plate stays centred around it.
static SDL_Rect LiftedRect(SDL_Rect a, SDL_Rect r, float dy, float s) {
    const float cx = r.x + r.w / 2.f, cy = r.y + r.h / 2.f;
    const float x0 = cx + (a.x - cx) * s, y0 = cy + (a.y - cy) * s + dy;
    const float x1 = cx + (a.x + a.w - cx) * s, y1 = cy + (a.y + a.h - cy) * s + dy;
    const int ix = (int)lroundf(x0), iy = (int)lroundf(y0);
    return SDL_Rect{ ix, iy, (int)lroundf(x1) - ix, (int)lroundf(y1) - iy };
}

void ChoiceTile(Id id, SDL_Rect r, UiIcons::Id icon, SDL_Color tint, const std::string& title, const std::string& desc,
                std::function<void()> press, uint16_t flags) {
    const SDL_Rect nr{ r.x - 7, r.y - 7, r.w + 14, r.h + 14 };
    Focus::Node n;
    n.id = id; n.r = nr; n.radius = 28; n.flags = (uint16_t)(flags | Focus::Ring);
    n.press = std::move(press); n.hint = T("hint.select");
    Focus::Add(std::move(n));
    BeginPress(id, r);
    const bool f = Focus::Is(id);
    // Every choice rests the same; only the highlight says which one is
    // picked (a tinted "recommended" tile read as already selected). The
    // highlighted one lifts, brightens and takes the accent border.
    static std::unordered_map<Id, Motion::Tween> lift;
    Motion::Tween& L = lift[id];
    const float want = f ? 1.f : 0.f;
    if (L.to != want) L.To(want, 0.42f, Motion::Curve::Spring);
    const float k = Motion::Reduced() ? want : L.Value();
    const float dy = -4.f * k, s = 1.f + 0.02f * k;
    const SDL_Rect plate = LiftedRect(nr, r, dy, s);
    Focus::DrawPlate(id, &plate);
    PushXform(0, dy, s, r.x + r.w / 2.f, r.y + r.h / 2.f, 1.f);
    FillRectRounded(r.x, r.y + 8, r.w, r.h, 22, SDL_Color{ 0, 0, 0, (uint8_t)(40 + 30 * k) });
    FillRectRounded(r.x, r.y, r.w, r.h, 22, Mix(gTheme.panel, Ink(), 0.03f + 0.04f * k));
    DrawRectRounded(r.x, r.y, r.w, r.h, 22, f ? Alpha(Acc(), 170) : Alpha(gTheme.border, 200));
    const int is = 64;
    IconTile(r.x + (r.w - is) / 2, r.y + 30, is, icon, tint);
    int ty = r.y + 30 + is + 22;
    Marquee(id, title, r.x + r.w / 2, ty + TextH(Font::Title) / 2, r.w - 40, f ? Ink() : Mix(Ink(), Muted(), 0.25f), Font::Title, f,
            Anchor::Centre);
    ty += TextH(Font::Title) + 6;
    Ui::TextBlockC(desc, r.x + r.w / 2, ty, r.w - 48, Muted(), Font::Body, 3, 2);
    PopXform();
    EndPress();
}

void PersonCard(Id id, SDL_Rect r, SDL_Texture* picture, const std::string& name, const std::string& sub,
                std::function<void()> press, uint16_t flags) {
    const SDL_Rect nr{ r.x - 8, r.y - 8, r.w + 16, r.h + 16 };
    Focus::Node n;
    n.id = id; n.r = nr; n.radius = 30; n.flags = (uint16_t)(flags | Focus::Ring);
    n.press = std::move(press); n.hint = T("hint.open");
    Focus::Add(std::move(n));
    BeginPress(id, r);
    const bool f = Focus::Is(id);
    // The card lifts a little when the highlight is on it; the plate goes up
    // with it so the outline stays even all round.
    static std::unordered_map<Id, Motion::Tween> lift;
    Motion::Tween& L = lift[id];
    const float want = f ? 1.f : 0.f;
    if (L.to != want) L.To(want, 0.42f, Motion::Curve::Spring);
    const float k = Motion::Reduced() ? want : L.Value();
    const float dy = -4.f * k, s = 1.f + 0.025f * k;
    const SDL_Rect plate = LiftedRect(nr, r, dy, s);
    Focus::DrawPlate(id, &plate);
    PushXform(0, dy, s, r.x + r.w / 2.f, r.y + r.h / 2.f, 1.f);
    const SDL_Color face = Mix(gTheme.panel, Ink(), f ? 0.07f : 0.03f);
    FillRectRounded(r.x, r.y + 10, r.w, r.h, 24, SDL_Color{ 0, 0, 0, (uint8_t)(50 + 30 * k) });
    FillRectRounded(r.x, r.y, r.w, r.h, 24, face);
    DrawRectRounded(r.x, r.y, r.w, r.h, 24, f ? Alpha(Acc(), 150) : Alpha(gTheme.border, 210));
    const int pad = 18, av = r.w - pad * 2;
    if (picture) {
        DrawTexture(picture, nullptr, SDL_Rect{ r.x + pad, r.y + pad, av, av });
        MaskCorners(r.x + pad, r.y + pad, av, av, 18, face);
    } else if (!name.empty()) {
        FillRectRounded(r.x + pad, r.y + pad, av, av, 18, Alpha(AvatarColor((uint32_t)std::hash<std::string>()(name)), 60));
        Avatar(r.x + r.w / 2, r.y + pad + av / 2, av / 3, name, (uint32_t)std::hash<std::string>()(name));
    }
    const int ny = r.y + pad + av + 18;
    Marquee(id, name, r.x + r.w / 2, ny + TextH(Font::Title) / 2, r.w - 24, f ? Ink() : Mix(Ink(), Muted(), 0.25f), Font::Title, f, Anchor::Centre);
    if (!sub.empty()) Marquee(Ui::IdMix(id, 0xDE5Cu), sub, r.x + r.w / 2, ny + TextH(Font::Title) + 14, r.w - 24, Muted(), Font::Body, f, Anchor::Centre);
    PopXform();
    EndPress();
}

void Spinner(int cx, int cy, int r, SDL_Color c) {
    const float t = (float)Motion::Now();
    const float a0 = fmodf(t * 330.f, 360.f);
    const float len = 70.f + 160.f * (0.5f + 0.5f * sinf(t * 2.6f));
    StrokeArc((float)cx, (float)cy, (float)r, 5.f, 0.f, 360.f, Alpha(c, 34), Alpha(c, 34));
    StrokeArc((float)cx, (float)cy, (float)r, 5.f, a0, a0 + len, Alpha(c, 90), c);
}

void ProgressBar(SDL_Rect r, float frac) {
    frac = std::max(0.f, std::min(1.f, frac));
    FillRectRounded(r.x, r.y, r.w, r.h, r.h / 2, Alpha(Ink(), 18));
    const int fw = (int)(r.w * frac);
    if (fw > 0) {
        FillRectRounded(r.x, r.y, std::max(fw, r.h), r.h, r.h / 2, Mix(gTheme.accent, Acc(), 0.55f + 0.45f * frac));
        // A soft light running along the fill.
        const float u = fmodf((float)Motion::Now() * 0.6f, 1.f);
        const int gx = r.x + (int)(fw * u);
        const int g0 = std::max(r.x, gx - 30), g1 = std::min(r.x + fw, gx + 30);
        if (g1 - g0 > r.h) FillRectRounded(g0, r.y, g1 - g0, r.h, r.h / 2, Alpha(SDL_Color{ 255, 255, 255, 255 }, 40));
    }
}

// ── People ───────────────────────────────────────────────────────────────────
SDL_Color AvatarColor(uint32_t seed) {
    uint32_t h = seed * 2654435761u;
    const float hue = (float)(h % 360);
    return HSVtoRGB(hue, 0.42f, 0.86f);
}
void Avatar(int cx, int cy, int r, const std::string& name, uint32_t seed, int face, SDL_Color hair, SDL_Texture* picture) {
    (void)hair;
    // The disc in the Mii's gender colour (blue for a boy, pink for a girl),
    // its own colour when that is not known.
    const SDL_Color c = face == 0 ? SDL_Color{ 92, 148, 236, 255 } : face == 1 ? SDL_Color{ 238, 118, 166, 255 } : AvatarColor(seed);
    FillDisc((float)cx, (float)cy, (float)r, c);
    // The Mii as it really looks, when the parts pack is there (its head
    // centred, a little hair past the disc).
    if (picture) {
        int pw = 0, ph = 0;
        SDL_QueryTexture(picture, nullptr, nullptr, &pw, &ph);
        if (pw > 0) {
            const float k = r / (50.f * pw / 128.f);
            const int dw = (int)lroundf(pw * k), dh = (int)lroundf(ph * k);
            DrawTexture(picture, nullptr, SDL_Rect{ cx - dw / 2, cy - (int)lroundf(57.f * pw / 128.f * k), dw, dh });
            return;
        }
    }
    // Otherwise its initial, in the middle of the disc.
    std::string ini;
    if (!name.empty()) {
        const unsigned char b = (unsigned char)name[0];
        const size_t n = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : 4;
        ini = UpperUtf8(name.substr(0, std::min(n, name.size())));
    }
    const Font f = r >= 44 ? Font::Xl : r >= 30 ? Font::Title : r >= 20 ? Font::Row : Font::Xs;
    DrawTextInkC(ini, cx, cy, ColorLuma(c) > 140 ? SDL_Color{ 30, 26, 40, 255 } : SDL_Color{ 250, 250, 255, 255 }, f);
}

// ── Layers ───────────────────────────────────────────────────────────────────
void Dialog(Id id, UiIcons::Id icon, Ui::Tone tone, const std::string& title, const std::string& body,
            const std::vector<DialogButton>& buttons, int focusIndex, std::function<void()> onBack,
            const std::string& backHint) {
    const double since = Motion::FirstSeen(id);
    const double now = Motion::Now();
    if (now - since < 0.0001) {
        Sound::Play(Sound::Voice::Open);
        if (focusIndex >= 0 && focusIndex < (int)buttons.size()) Focus::Set(Ui::IdMix(id, (uint32_t)focusIndex));
    }
    const int w = 640;
    // Paragraphs on "\n", each wrapped.
    std::vector<std::string> lines;
    {
        size_t a = 0;
        while (a <= body.size()) {
            const size_t b = body.find('\n', a);
            const std::string para = body.substr(a, b == std::string::npos ? std::string::npos : b - a);
            if (!para.empty()) for (auto& l : Ui::WrapLines(para, Font::Body, w - 80, 4)) lines.push_back(l);
            if (b == std::string::npos) break;
            a = b + 1;
        }
        if (lines.size() > 7) lines.resize(7);
    }
    const int bodyH = (int)lines.size() * (TextH(Font::Body) + 4);
    const int h = 40 + 64 + 18 + TextH(Font::Title) + 8 + bodyH + 30 + 56 + 30;
    const int x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
    // Over everything, wherever it is drawn from; the clip comes back after.
    SDL_Rect outerClip{};
    const bool outerClipped = SDL_RenderIsClipEnabled(gRen) == SDL_TRUE;
    if (outerClipped) SDL_RenderGetClipRect(gRen, &outerClip);
    SDL_RenderSetClipRect(gRen, nullptr);
    Focus::PushLayer(Focus::Layer::Dialog, std::move(onBack), backHint, SDL_Rect{ x, y, w, h });
    const float ft = std::min(1.f, (float)((now - since) / 0.16));
    DimScreen((int)(150 * (Motion::Reduced() ? 1.f : ft)));
    // jelly-pop: scale .82 → 1.04 (45 %) → .985 (70 %) → 1, rising from 6 px
    float s = 1, dy = 0, a = 1;
    const float t = (float)((now - since) / 0.52);
    if (!Motion::Reduced() && t < 1) {
        auto seg = [](float u) { return Motion::Ease(Motion::Curve::EaseOut, u); };
        if (t < 0.45f) { const float u = seg(t / 0.45f); s = 0.82f + (1.04f - 0.82f) * u; dy = 6 - 8 * u; a = u; }
        else if (t < 0.7f) { const float u = seg((t - 0.45f) / 0.25f); s = 1.04f + (0.985f - 1.04f) * u; dy = -2 + 2 * u; }
        else { const float u = seg((t - 0.7f) / 0.3f); s = 0.985f + 0.015f * u; }
    }
    PushXform(0, dy, s, SCREEN_W / 2.f, SCREEN_H / 2.f, a);
    FillRectRounded(x, y + 10, w, h, 26, SDL_Color{ 0, 0, 0, 60 });
    FillRectRounded(x, y, w, h, 26, Mix(gTheme.panel, Ink(), 0.03f));
    DrawRectRounded(x, y, w, h, 26, tone == Ui::Tone::Neutral ? Alpha(gTheme.border, 230) : Alpha(Ui::ToneColor(tone), 120));
    const SDL_Color tc = tone == Ui::Tone::Neutral ? Acc() : Ui::ToneColor(tone);
    IconTile(SCREEN_W / 2 - 32, y + 36, 64, icon, tc);
    int ty = y + 36 + 64 + 18;
    Marquee(Ui::IdMix(id, 0x7171u), title, SCREEN_W / 2, ty + TextH(Font::Title) / 2, w - 60, Ink(), Font::Title, true, Anchor::Centre);
    ty += TextH(Font::Title) + 8;
    for (const std::string& l : lines) { DrawTextC(l, SCREEN_W / 2, ty + TextH(Font::Body) / 2, Muted(), Font::Body); ty += TextH(Font::Body) + 4; }
    const int n = (int)buttons.size(), gap = 14;
    const int bw = std::min(260, (w - 60 - gap * (n - 1)) / std::max(1, n));
    const int total = bw * n + gap * (n - 1);
    int bx = SCREEN_W / 2 - total / 2;
    const int by = y + h - 30 - 56;
    for (int i = 0; i < n; i++) {
        const DialogButton& b = buttons[i];
        Button(Ui::IdMix(id, (uint32_t)i), SDL_Rect{ bx, by, bw, 56 }, b.label, b.kind, b.fn, UiIcons::Id::Count, b.enabled);
        bx += bw + gap;
    }
    PopXform();
    Focus::PopLayer();
    if (outerClipped) SDL_RenderSetClipRect(gRen, &outerClip); else SDL_RenderSetClipRect(gRen, nullptr);
}

namespace {
struct ChoiceState {
    bool open = false;
    Id id = 0;
    std::string title;
    std::vector<std::string> options;
    std::vector<Art::Id> art;
    int current = 0;
    std::function<void(int)> pick;
    SDL_Rect anchor{};
} sChoice;
}  // namespace

void OpenChoice(Id id, const std::string& title, std::vector<std::string> options, int current, std::function<void(int)> pick,
                SDL_Rect anchor, std::vector<Art::Id> art) {
    if (options.empty()) return;
    sChoice.open = true;
    sChoice.id = Ui::IdMix(id, 0x43484F49u);       // the list's own id
    sChoice.title = title;
    sChoice.options = std::move(options);
    sChoice.art = std::move(art);
    sChoice.current = std::max(0, std::min((int)sChoice.options.size() - 1, current));
    sChoice.pick = std::move(pick);
    sChoice.anchor = anchor;
    Motion::Forget(sChoice.id);
}

void DrawChoice() {
    if (!sChoice.open) return;
    std::vector<MenuItem> items;
    for (int i = 0; i < (int)sChoice.options.size(); i++) {
        MenuItem it{ sChoice.options[i], "", UiIcons::Id::Count, false, [i]() {
            sChoice.open = false;
            if (sChoice.pick) sChoice.pick(i);
        }, i == sChoice.current };
        if (i < (int)sChoice.art.size()) it.art = sChoice.art[i];
        items.push_back(std::move(it));
    }
    Menu(sChoice.id, sChoice.title, items, []() { sChoice.open = false; }, T("hint.close"), sChoice.current, sChoice.anchor);
}

void RowChoice(Id id, SDL_Rect r, const RowText& t, const std::vector<std::string>& options, int current,
               std::function<void(int)> step, bool enabled, std::vector<Art::Id> art) {
    const int n = std::max(1, (int)options.size());
    const int cur = std::max(0, std::min(n - 1, current));
    const std::string title = t.title;
    auto press = [id, title, options, cur, step, art]() {
        OpenChoice(id, title, options, cur, [cur, step](int pick) { if (pick != cur && step) step(pick - cur); }, Focus::RectOf(id), art);
    };
    RowStepper(id, r, t, options.empty() ? std::string() : options[cur], n > 1 ? (float)cur / (n - 1) : 0.f, n > 1 ? 1.f / (n - 1) : 1.f,
               step, press, T("hint.choose"), enabled, {}, cur < (int)art.size() ? art[cur] : Art::Id::Count);
}

// ── A calendar ───────────────────────────────────────────────────────────────
namespace {
struct CalState {
    bool open = false;
    Id id = 0;
    std::string title;
    int y = 2000, m = 1;              // the month shown
    int sy = 2000, sm = 1, sd = 1;    // the day chosen
    int lo = 1900, hi = 2100;
    std::function<void(int, int, int)> pick;
} sCal;

int DaysIn(int y, int m) {
    static const int kDays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2) return (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 29 : 28;
    return kDays[(m - 1 + 12) % 12];
}
// 0 = Monday.
int Weekday(int y, int m, int d) {
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y -= 1;
    const int sun0 = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
    return (sun0 + 6) % 7;
}
void CalMonth(int d) {
    int m = sCal.m + d, y = sCal.y;
    while (m < 1) { m += 12; y--; }
    while (m > 12) { m -= 12; y++; }
    if (y < sCal.lo || y > sCal.hi) { Sound::Play(Sound::Voice::Seat, { 0.5f, true, 0.6f }); return; }
    sCal.m = m; sCal.y = y;
    Sound::Play(Sound::Voice::Tick, { m / 12.f });
}
void CalYear(int d) {
    const int y = std::max(sCal.lo, std::min(sCal.hi, sCal.y + d));
    if (y == sCal.y) { Sound::Play(Sound::Voice::Seat, { 0.5f, true, 0.6f }); return; }
    sCal.y = y;
    Sound::Play(Sound::Voice::Tick, { 0.5f });
}
}  // namespace

void OpenCalendar(Id id, const std::string& title, int y, int m, int d, int yearLo, int yearHi, std::function<void(int, int, int)> pick) {
    sCal.open = true;
    sCal.id = Ui::IdMix(id, 0x43414Cu);
    sCal.title = title;
    sCal.lo = yearLo; sCal.hi = yearHi;
    sCal.sy = std::max(yearLo, std::min(yearHi, y));
    sCal.sm = std::max(1, std::min(12, m));
    sCal.sd = std::max(1, std::min(DaysIn(sCal.sy, sCal.sm), d));
    sCal.y = sCal.sy; sCal.m = sCal.sm;
    sCal.pick = std::move(pick);
    Motion::Forget(sCal.id);
}

void DrawCalendar() {
    if (!sCal.open) return;
    const double since = Motion::FirstSeen(sCal.id), now = Motion::Now();
    if (now - since < 0.0001) {
        Sound::Play(Sound::Voice::Open);
        Focus::Set(Ui::IdMix(sCal.id, 100u + (uint32_t)sCal.sd));
    }
    const int cw = 66, ch = 50, gap = 6;
    const int w = 7 * cw + 6 * gap + 56, h = 470;
    const int x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2 - 10;
    SDL_Rect outerClip{};
    const bool outerClipped = SDL_RenderIsClipEnabled(gRen) == SDL_TRUE;
    if (outerClipped) SDL_RenderGetClipRect(gRen, &outerClip);
    SDL_RenderSetClipRect(gRen, nullptr);
    Focus::PushLayer(Focus::Layer::Sheet, []() { sCal.open = false; }, T("hint.close"), SDL_Rect{ x, y, w, h });
    const float t = Motion::Reduced() ? 1.f : std::min(1.f, (float)((now - since) / 0.28));
    DimScreen((int)(120 * t));
    const float e = Motion::Ease(Motion::Curve::Spring, t);
    PushXform(0, 18.f * (1 - e), 0.96f + 0.04f * e, x + w / 2.f, y + h / 2.f, std::min(1.f, t * 1.4f));
    FillRectRounded(x, y + 10, w, h, 26, SDL_Color{ 0, 0, 0, 70 });
    FillRectRounded(x, y, w, h, 26, Mix(gTheme.panel, Ink(), 0.035f));
    DrawRectRounded(x, y, w, h, 26, Alpha(gTheme.border, 230));
    DrawTextVC(sCal.title, x + 28, y + 34, Faint(), Font::Body);
    // The month, its arrows; years on ZL ZR.
    const int my = y + 84;
    auto arrow = [&](uint32_t k, int ax, bool left, std::function<void()> fn) {
        const Id aid = Ui::IdMix(sCal.id, k);
        Focus::Node n;
        n.id = aid; n.r = SDL_Rect{ ax - 26, my - 26, 52, 52 }; n.radius = 26; n.flags = Focus::OwnSound | Focus::Ring;
        n.press = std::move(fn); n.hint = T(left ? "cal.prev" : "cal.next");
        Focus::Add(std::move(n));
        BeginPress(aid, SDL_Rect{ ax - 26, my - 26, 52, 52 });
        Focus::DrawPlate(aid);
        ArrowButton(ax, my, left, Focus::Is(aid));
        EndPress();
    };
    arrow(1, x + 50, true, []() { CalMonth(-1); });
    arrow(2, x + w - 50, false, []() { CalMonth(1); });
    std::string head = T("cal.head");
    {
        auto put = [&head](const std::string& key, const std::string& v) {
            const size_t p = head.find(key);
            if (p != std::string::npos) head.replace(p, key.size(), v);
        };
        put("{month}", T(("cal.mon." + std::to_string(sCal.m)).c_str()));
        put("{y}", std::to_string(sCal.y));
    }
    DrawTextC(head, x + w / 2, my, Ink(), Font::Title);
    // The weekdays, then the days.
    std::vector<std::string> wd;
    {
        const std::string all = T("cal.wd");
        size_t p = 0;
        while (wd.size() < 7) {
            const size_t q = all.find(',', p);
            wd.push_back(all.substr(p, q == std::string::npos ? std::string::npos : q - p));
            if (q == std::string::npos) break;
            p = q + 1;
        }
        while (wd.size() < 7) wd.push_back("");
    }
    const int gx = x + 28, gy = my + 44;
    for (int c = 0; c < 7; c++) DrawTextC(wd[c], gx + c * (cw + gap) + cw / 2, gy + 10, c >= 5 ? Alpha(Acc(), 220) : Faint(), Font::Xs);
    const int first = Weekday(sCal.y, sCal.m, 1), days = DaysIn(sCal.y, sCal.m);
    for (int d = 1; d <= days; d++) {
        const int cell = first + d - 1, c = cell % 7, r = cell / 7;
        const SDL_Rect cr{ gx + c * (cw + gap), gy + 28 + r * (ch + gap), cw, ch };
        const Id did = Ui::IdMix(sCal.id, 100u + (uint32_t)d);
        const bool chosen = sCal.y == sCal.sy && sCal.m == sCal.sm && d == sCal.sd;
        Focus::Node n;
        n.id = did; n.r = cr; n.radius = 14; n.flags = Focus::OwnSound; n.hint = T("hint.select");
        n.press = [d]() {
            sCal.open = false;
            Sound::Play(Sound::Voice::Ok);
            if (sCal.pick) sCal.pick(sCal.y, sCal.m, d);
        };
        Focus::Add(std::move(n));
        BeginPress(did, cr);
        if (chosen) FillRectRounded(cr.x, cr.y, cr.w, cr.h, 14, Acc());
        else FillRectRounded(cr.x, cr.y, cr.w, cr.h, 14, Alpha(Ink(), 10));
        Focus::DrawPlate(did);
        const SDL_Color ink = chosen ? (ColorLuma(Acc()) > 150 ? SDL_Color{ 20, 18, 28, 255 } : SDL_Color{ 250, 250, 255, 255 })
                                     : (c >= 5 ? Mix(Ink(), Acc(), 0.35f) : Ink());
        DrawTextC(std::to_string(d), cr.x + cr.w / 2, cr.y + cr.h / 2, ink, Font::Row);
        EndPress();
    }
    PopXform();
    // L R: months; ZL ZR: years.
    Focus::Command(Input::L, T("cal.month"), []() { CalMonth(-1); });
    Focus::Command(Input::R, T("cal.month"), []() { CalMonth(1); });
    Focus::Command(Input::ZL, T("cal.year"), []() { CalYear(-1); });
    Focus::Command(Input::ZR, T("cal.year"), []() { CalYear(1); });
    if (outerClipped) SDL_RenderSetClipRect(gRen, &outerClip); else SDL_RenderSetClipRect(gRen, nullptr);
    Focus::PopLayer();
}

bool gMenuGrid = false;
void (*gOnMenuGridChanged)() = nullptr;

namespace {
// One thing in a menu's grid: its picture (or icon) with its name under it.
void MenuTile(const MenuItem& it, Id rid, SDL_Rect r) {
    const bool f = Focus::Is(rid);
    if (!f) FillRectRounded(r.x, r.y, r.w, r.h, 18, Alpha(Ink(), it.checked ? 20 : 9));
    if (it.checked) {
        DrawRectRounded(r.x, r.y, r.w, r.h, 18, Alpha(Acc(), 220));
        DrawRectRounded(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 17, Alpha(Acc(), 120));
    }
    const int ps = f ? 92 : 84, pcx = r.x + r.w / 2, pcy = r.y + 12 + 44;
    const SDL_Rect pa{ pcx - ps / 2, pcy - ps / 2, ps, ps };
    if (it.item) {
        if (SDL_Texture* pt = Art::ItemPicture(it.item)) Art::DrawFit(pt, pa);
    } else if (it.art != Art::Id::Count && Art::Has(it.art)) {
        if (Art::IsMask(it.art)) Art::Draw(it.art, pa, Muted());
        else Art::Draw(it.art, pa);
    } else if (!it.dots.empty()) {
        SwatchDots(pcx - 9 * (int)it.dots.size(), pcy, it.dots, 12);
    } else if (it.icon != UiIcons::Id::Count) {
        FillDisc((float)pcx, (float)pcy, 34.f, Alpha(Ink(), 12));
        UiIcons::Draw(it.icon, pcx, pcy, 36, it.danger ? gTheme.red : Muted());
    } else {
        FillDisc((float)pcx, (float)pcy, 30.f, Alpha(Ink(), 12));
    }
    const SDL_Color ink = it.danger ? gTheme.red : Ink();
    const int lh = TextH(Font::Xs);
    int ty = r.y + 12 + 88 + 6;
    for (const std::string& l : Ui::WrapLines(it.label, Font::Xs, r.w - 14, 2)) {
        DrawTextC(l, pcx, ty + lh / 2, ink, Font::Xs);
        ty += lh;
    }
    if (it.checked) {
        FillDisc((float)(r.x + r.w - 18), (float)(r.y + 18), 12.f, Acc());
        UiIcons::Draw(UiIcons::Id::Check, r.x + r.w - 18, r.y + 18, 16,
                      ColorLuma(Acc()) > 150 ? SDL_Color{ 20, 18, 28, 255 } : SDL_Color{ 250, 250, 255, 255 });
    }
}
}  // namespace

void Menu(Id id, const std::string& title, const std::vector<MenuItem>& items, std::function<void()> onClose,
          const std::string& closeHint, int focusIndex, SDL_Rect anchor) {
    const double since = Motion::FirstSeen(id);
    const double now = Motion::Now();
    const bool first = now - since < 0.0001;
    // A long list of pictures (clothes, objects, ground) can also be seen as
    // a grid of tiles: Y, or the switch in the list's corner. The choice is
    // kept for every such list.
    int pictured = 0;
    for (const MenuItem& it : items) if (it.item || (it.art != Art::Id::Count && Art::Has(it.art))) pictured++;
    const bool canGrid = pictured >= 6 && !title.empty();
    const bool dropdown = anchor.w > 0;
    const bool grid = canGrid && gMenuGrid;
    static std::unordered_map<Id, double> viewAt;          // when the view last changed
    if (first) {
        Sound::Play(Sound::Voice::Menu);
        if (focusIndex >= 0 && focusIndex < (int)items.size()) Focus::Set(Ui::IdMix(id, (uint32_t)focusIndex));
        viewAt[id] = since;
    }
    const double shownAt = std::max(since, viewAt[id]);
    const int rowH = 62;
    constexpr int kTileH = 156, kGap = 10, kTileMin = 126;
    int w = 560;
    const int headH = title.empty() || (dropdown && !canGrid) ? 16 : 64;
    int listH = std::min((int)items.size() * rowH, SCREEN_H - 200);
    int x = (SCREEN_W - w) / 2, y;
    int cols = 1;
    bool up = false;
    if (grid) {
        // Wide and in the middle of the screen, as many rows as fit.
        w = std::min(SCREEN_W - 96, 7 * (kTileMin + kGap) + 6);
        cols = std::max(1, (w - 16 + kGap) / (kTileMin + kGap));
        const int rows = ((int)items.size() + cols - 1) / cols;
        listH = std::min(rows * (kTileH + kGap) - kGap + 8, SCREEN_H - 150 - headH);
        x = (SCREEN_W - w) / 2;
        y = (SCREEN_H - (headH + listH + 16)) / 2;
    } else if (anchor.w > 0) {
        // A dropdown under the row's control, right-aligned with it.
        w = std::max(380, std::min(500, anchor.w / 2));
        // Under the right end of a row; under the left edge of something
        // narrower than the menu (a tile).
        x = anchor.w >= w ? anchor.x + anchor.w - w - 8 : anchor.x - 4;
        x = std::max(16, std::min(SCREEN_W - w - 16, x));
        const int below = SCREEN_H - 64 - (anchor.y + anchor.h + 6);
        const int above = anchor.y - 6 - 16;
        const int need = headH + listH + 16;
        if (need <= below) y = anchor.y + anchor.h + 6;
        else if (need <= above) { y = anchor.y - 6 - need; up = true; }
        // Too long for either side: in the middle of the screen, all of it
        // shown, rather than a sliver that scrolls.
        else if (need <= SCREEN_H - 96) y = (SCREEN_H - need) / 2;
        else if (below >= above) { listH = std::max(rowH * 2, below - headH - 16); y = anchor.y + anchor.h + 6; }
        else { listH = std::max(rowH * 2, above - headH - 16); y = anchor.y - 6 - (headH + listH + 16); up = true; }
    } else {
        y = (SCREEN_H - (headH + listH + 16)) / 2;
    }
    const int h = headH + listH + 16;
    // A menu is over everything, wherever it is drawn from (a page's scrolling
    // list, say): the clip it was opened under is put back at the end.
    SDL_Rect outerClip{};
    const bool outerClipped = SDL_RenderIsClipEnabled(gRen) == SDL_TRUE;
    if (outerClipped) SDL_RenderGetClipRect(gRen, &outerClip);
    SDL_RenderSetClipRect(gRen, nullptr);
    Focus::PushLayer(Focus::Layer::Sheet, std::move(onClose), closeHint, SDL_Rect{ x, y, w, h });
    DimScreen((int)((anchor.w > 0 && !grid ? 70 : 120) * std::min(1.f, (float)((now - since) / 0.16))));
    // Switching between the list and the grid: the panel glides to its new
    // place and size.
    static std::unordered_map<Id, Motion::Tween> morph[4];
    const float want[4] = { (float)x, (float)y, (float)w, (float)h };
    float got[4];
    for (int k = 0; k < 4; k++) {
        Motion::Tween& m = morph[k][id];
        if (first || Motion::Reduced()) m.Snap(want[k]);
        else if (m.to != want[k]) m.To(want[k], 0.36f, Motion::Curve::Swift);
        got[k] = m.Value();
    }
    const int px = (int)lroundf(got[0]), pyT = (int)lroundf(got[1]), pw = (int)lroundf(got[2]), ph = (int)lroundf(got[3]);
    // Unroll: the panel opens from its anchored edge over 0.32 s, its rows
    // fading in 14 ms apart.
    const float t = Motion::Reduced() ? 1.f : std::min(1.f, (float)((now - since) / 0.32));
    const float e = Motion::Bezier(0.33f, 1.f, 0.68f, 1.f, t);
    const int shownH = std::max(40, (int)(ph * e));
    const int py = up ? pyT + ph - shownH : pyT;
    FillRectRounded(px, py + 10, pw, shownH, 22, SDL_Color{ 0, 0, 0, 60 });
    FillRectRounded(px, py, pw, shownH, 22, Mix(gTheme.panel, Ink(), 0.035f));
    DrawRectRounded(px, py, pw, shownH, 22, Alpha(gTheme.border, 230));
    SDL_Rect clip{ px, py, pw, shownH };
    SDL_RenderSetClipRect(gRen, &clip);
    if (!title.empty() && !(dropdown && !grid))
        Marquee(Ui::IdMix(id, 0x7171u), title, px + 26, pyT + 34, pw - 52 - (canGrid ? 112 : 0), Faint(), Font::Body, true, Anchor::Middle);
    if (canGrid) {
        // List | grid, a pill that slides to the one shown.
        const Id tid = Ui::IdMix(id, 0x6A1Du);
        const int tw = 96, th = 40;
        const SDL_Rect tr{ px + pw - tw - 18, pyT + 34 - th / 2, tw, th };
        auto toggle = [id]() {
            gMenuGrid = !gMenuGrid;
            viewAt[id] = Motion::Now();
            Sound::Play(Sound::Voice::Tick, { gMenuGrid ? 1.f : 0.f });
            if (gOnMenuGridChanged) gOnMenuGridChanged();
        };
        Focus::Node n;
        n.id = tid; n.r = tr; n.radius = th / 2; n.flags = Focus::OwnSound | Focus::Ring;
        n.hint = gMenuGrid ? T("hint.list") : T("hint.grid");
        n.press = [tr, toggle]() {
            const Input::Frame& in = Input::Last();
            // A finger on the half already shown leaves it as it is.
            if ((in.tap || in.released) && (in.x >= tr.x + tr.w / 2) == gMenuGrid) return;
            toggle();
        };
        Focus::Add(std::move(n));
        Focus::Command(Input::Y, gMenuGrid ? T("hint.list") : T("hint.grid"), toggle);
        Focus::DrawPlate(tid);
        FillRectRounded(tr.x, tr.y, tr.w, tr.h, tr.h / 2, Alpha(Ink(), 14));
        static std::unordered_map<Id, Motion::Tween> knob;
        Motion::Tween& g = knob[id];
        const float to = gMenuGrid ? 1.f : 0.f;
        if (first || Motion::Reduced()) g.Snap(to);
        else if (g.to != to) g.To(to, 0.32f, Motion::Curve::Spring);
        const int half = tr.w / 2;
        const int kx = tr.x + 3 + (int)lroundf(g.Value() * (half - 3));
        FillRectRounded(kx, tr.y + 3, half - 3, tr.h - 6, (tr.h - 6) / 2, Acc());
        const SDL_Color on = ColorLuma(Acc()) > 150 ? SDL_Color{ 20, 18, 28, 255 } : SDL_Color{ 250, 250, 255, 255 };
        UiIcons::Draw(UiIcons::Id::List, tr.x + 2 + half / 2, tr.y + tr.h / 2, 22, gMenuGrid ? Muted() : on);
        UiIcons::Draw(UiIcons::Id::Grid, tr.x + half + half / 2 - 1, tr.y + tr.h / 2, 22, gMenuGrid ? on : Muted());
    }
    static std::unordered_map<Id, Focus::Scroll> scrolls;
    Focus::Scroll& sc = scrolls[id];
    const SDL_Rect view{ x + 8, y + headH, w - 16, listH };
    Focus::BeginScroll(id, view, sc);
    const int tileW = grid ? (view.w - (cols - 1) * kGap) / cols : view.w;
    const int span = grid ? kTileH + kGap : rowH;
    // Rows fade in from the first one in view (a list opened far down, at the
    // current pick, shows at once).
    const int firstShown = std::max(0, (int)(sc.offset / span)) * cols;
    for (size_t i = 0; i < items.size(); i++) {
        const MenuItem& it = items[i];
        const Id rid = Ui::IdMix(id, (uint32_t)i);
        const SDL_Rect r = grid
            ? SDL_Rect{ view.x + (int)(i % cols) * (tileW + kGap), view.y + 4 + (int)(i / cols) * span - (int)sc.offset, tileW, kTileH }
            : SDL_Rect{ view.x, view.y + (int)i * rowH - (int)sc.offset, view.w, rowH - 4 };
        Focus::Node n;
        n.id = rid; n.r = r; n.radius = grid ? 18 : 14; n.flags = Focus::OwnSound; n.hint = T("hint.select");
        auto fn = it.fn;
        n.press = [fn]() { Sound::Play(Sound::Voice::Select); if (fn) fn(); };
        Focus::Add(std::move(n));
        // A long list (hundreds of clothes): only the rows in view are drawn.
        if (r.y + r.h < view.y - span || r.y > view.y + view.h + span) continue;
        const int order = std::min(24, std::max(0, (int)i - firstShown));
        const float delay = grid ? 0.03f * (float)(order / cols + order % cols) : 0.014f * (float)order;
        const float rt = Motion::Reduced() ? 1.f : std::max(0.f, std::min(1.f, (float)((now - shownAt - 0.04 - delay) / 0.24)));
        PushXform(0, -6.f * (1 - rt), 1, 0, 0, rt);
        BeginPress(rid, r);
        Focus::DrawPlate(rid);
        if (grid) {
            MenuTile(it, rid, r);
            EndPress();
            PopXform();
            continue;
        }
        int tx = r.x + 18;
        const SDL_Color ink = it.danger ? gTheme.red : Ink();
        if (!it.dots.empty()) tx = r.x + 18 + SwatchDots(r.x + 18, r.y + r.h / 2, it.dots) + 16;
        else if (it.item) {
            // The item's picture on a soft tile.
            const int as = Focus::Is(rid) ? 48 : 44;
            const SDL_Rect ab{ r.x + 34 - as / 2, r.y + r.h / 2 - as / 2, as, as };
            FillRectRounded(ab.x, ab.y, ab.w, ab.h, 10, Alpha(Ink(), 12));
            if (SDL_Texture* pt = Art::ItemPicture(it.item)) Art::DrawFit(pt, SDL_Rect{ ab.x + 3, ab.y + 3, ab.w - 6, ab.h - 6 });
            tx = r.x + 70;
        }
        else if (it.art != Art::Id::Count && Art::Has(it.art)) {
            // The game's picture, a little bigger when highlighted.
            const int as = Focus::Is(rid) ? 44 : 40;
            const SDL_Rect ab{ r.x + 32 - as / 2, r.y + r.h / 2 - as / 2, as, as };
            if (Art::IsMask(it.art)) Art::Draw(it.art, ab, Muted());
            else Art::Draw(it.art, ab);
            tx = r.x + 66;
        }
        else if (it.icon != UiIcons::Id::Count) { UiIcons::Draw(it.icon, r.x + 30, r.y + r.h / 2, 24, it.danger ? gTheme.red : Muted()); tx = r.x + 58; }
        const bool itf = Focus::Is(rid);
        if (it.sub.empty()) Marquee(rid, it.label, tx, r.y + r.h / 2, r.w - (tx - r.x) - 50, ink, Font::Row, itf, Anchor::Middle);
        else {
            Marquee(rid, it.label, tx, r.y + 7, r.w - (tx - r.x) - 50, ink, Font::Row, itf);
            Marquee(Ui::IdMix(rid, 0xDE5Cu), it.sub, tx, r.y + 7 + TextH(Font::Row), r.w - (tx - r.x) - 50, Faint(), Font::Xs, itf);
        }
        if (it.checked) UiIcons::Draw(UiIcons::Id::Check, r.x + r.w - 28, r.y + r.h / 2, 22, Acc());
        EndPress();
        PopXform();
    }
    const int rowsN = ((int)items.size() + cols - 1) / cols;
    Focus::EndScroll(grid ? (float)(rowsN * span + 8) : (float)(items.size() * rowH));
    if (outerClipped) SDL_RenderSetClipRect(gRen, &outerClip); else SDL_RenderSetClipRect(gRen, nullptr);
    Focus::PopLayer();
}

// ── Button glyphs ────────────────────────────────────────────────────────────
// The console's own shapes: round face buttons, pill-shaped shoulder buttons,
// small round + and −, and a D-pad for ◀ ▶. Light glyph, dark letter.
namespace {
const char* const kAdjustKey = "\xE2\x97\x80\xE2\x96\xB6";
bool IsFace(const std::string& k) { return k == "A" || k == "B" || k == "X" || k == "Y"; }
std::vector<std::string> SplitKeys(const std::string& key) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p < key.size()) {
        size_t q = key.find(' ', p);
        if (q == std::string::npos) q = key.size();
        if (q > p) out.push_back(key.substr(p, q - p));
        p = q + 1;
    }
    return out;
}
int OneGlyphW(const std::string& k) {
    if (IsFace(k) || k == "+" || k == "-") return 26;
    if (k == kAdjustKey) return 30;
    return std::max(30, TextW(k, Font::Xs) + 18);
}
void OneGlyph(const std::string& k, int x, int cy, float a) {
    const SDL_Color face = Alpha(Ink(), (int)(225 * a)), mark = Alpha(gTheme.bg, (int)(255 * a));
    if (IsFace(k)) {
        FillDisc((float)x + 13.f, (float)cy, 13.f, face);
        DrawTextC(k, x + 13, cy, mark, Font::Xs);
    } else if (k == "+" || k == "-") {
        FillDisc((float)x + 13.f, (float)cy, 11.f, face);
        FillRectSquare(x + 8, cy - 1, 10, 2, mark);
        if (k == "+") FillRectSquare(x + 12, cy - 5, 2, 10, mark);
    } else if (k == kAdjustKey) {
        // A D-pad with its left and right arms lit.
        const SDL_Color dim = Alpha(Ink(), (int)(70 * a));
        FillRectRounded(x + 11, cy - 13, 8, 26, 3, dim);
        FillRectRounded(x + 2, cy - 4, 26, 8, 3, face);
    } else {
        const int w = OneGlyphW(k);
        FillRectRounded(x, cy - 12, w, 24, 10, face);
        DrawTextC(k, x + w / 2, cy, mark, Font::Xs);
    }
}
}  // namespace

int ButtonGlyphW(const std::string& key) {
    int w = 0;
    for (const auto& k : SplitKeys(key)) w += (w ? 4 : 0) + OneGlyphW(k);
    return w;
}
void ButtonGlyph(const std::string& key, int x, int cy, float alpha) {
    for (const auto& k : SplitKeys(key)) { OneGlyph(k, x, cy, alpha); x += OneGlyphW(k) + 4; }
}

// ── Hint bar ─────────────────────────────────────────────────────────────────
// What the buttons do right now, arranged like the console's own bar: the
// page's actions on the right with A last and B beside it, then the page's
// other buttons further left; the buttons that always work (L R to change
// page, − for help) small and dim on the left. + shows only where nothing else
// gets out, never two hints with the same words, and hints glide in, out and
// along instead of jumping when the highlight moves.
void HintBar(const std::vector<Focus::Hint>& hints, int left, int right, int cy, const std::string& adjustLabel) {
    struct It { std::string key, label; bool sys; int order; int w; };
    std::vector<It> items;
    bool hasB = false;
    for (const auto& h : hints) if (h.key == "B") hasB = true;
    for (const auto& h : hints) {
        It it{ h.key, h.key == kAdjustKey ? adjustLabel : h.label, false, 40, 0 };
        if (it.label.empty()) continue;
        const std::string& k = h.key;
        if (k == "+") { if (hasB) continue; it.order = 85; }
        else if (k == "-") { it.sys = true; it.order = 1; }
        else if (k == "L R" || k == "L" || k == "R") { it.sys = true; it.order = 0; }
        else if (k == "A") it.order = 100;
        else if (k == "B") it.order = 90;
        else if (k == kAdjustKey) it.order = 80;
        else if (k == "X") it.order = 70;
        else if (k == "Y") it.order = 60;
        else if (k == "ZL ZR" || k == "ZL" || k == "ZR") it.order = 50;
        bool dup = false;
        for (const auto& o : items) if (o.label == it.label || o.key == it.key) { dup = true; break; }
        if (dup) continue;
        it.w = ButtonGlyphW(it.key) + 9 + TextW(it.label, Font::Body);
        items.push_back(it);
    }
    std::stable_sort(items.begin(), items.end(), [](const It& a, const It& b) { return a.order < b.order; });

    // Where each one goes: the system group from the left edge, the rest
    // packed against the right edge.
    const int gap = 26, sysGap = 20;
    std::vector<int> tx(items.size(), 0);
    {
        int x = left;
        for (size_t i = 0; i < items.size(); i++) if (items[i].sys) { tx[i] = x; x += items[i].w + sysGap; }
        int r = right;
        for (int i = (int)items.size() - 1; i >= 0; i--) if (!items[i].sys) { r -= items[i].w; tx[i] = r; r -= gap; }
    }

    struct Anim { Motion::Tween x; double born = 0, gone = -1, relabel = -10; std::string label; bool sys = false; int w = 0; };
    static std::unordered_map<std::string, Anim> anims;
    const double now = Motion::Now();
    const bool reduced = Motion::Reduced();
    for (auto& kv : anims) kv.second.gone = kv.second.gone < 0 ? now : kv.second.gone;   // assume gone until seen
    for (size_t i = 0; i < items.size(); i++) {
        auto found = anims.find(items[i].key);
        if (found == anims.end() || (found->second.gone >= 0 && now - found->second.gone > 0.2)) {
            Anim& a = anims[items[i].key];
            a = Anim{};
            a.x.Snap((float)tx[i]);
            a.born = now;
            a.label = items[i].label;
        }
        Anim& a = anims[items[i].key];
        a.gone = -1;
        if (a.x.to != (float)tx[i]) a.x.To((float)tx[i], 0.26f, Motion::Curve::Swift);
        if (a.label != items[i].label) { a.label = items[i].label; a.relabel = now; }
        a.sys = items[i].sys;
        a.w = items[i].w;
    }
    for (auto it = anims.begin(); it != anims.end();) {
        Anim& a = it->second;
        float alpha = reduced ? 1.f : std::min(1.f, (float)((now - a.born) / 0.18));
        if (a.gone >= 0) {
            const float out = reduced ? 1.f : (float)((now - a.gone) / 0.14);
            if (out >= 1.f) { it = anims.erase(it); continue; }
            alpha *= 1.f - out;
        }
        const int x = (int)(reduced ? a.x.to : a.x.Value());
        const float glyphA = a.sys ? 0.55f * alpha : alpha;
        // A tap on it presses it (Input::HintZone); a tapped one flashes.
        const int lw = ButtonGlyphW(it->first) + 9 + TextW(a.label, Font::Body);
        const SDL_Rect zone{ x - 12, cy - 24, lw + 24, 48 };
        if (alpha > 0.5f) Input::HintZone(it->first, zone);
        float ft = 0.f;
        if (Input::HintFlash(zone, ft))
            FillRectRounded(zone.x, zone.y + 4, zone.w, zone.h - 8, (zone.h - 8) / 2, Alpha(Acc(), (int)(110 * (1.f - ft))));
        ButtonGlyph(it->first, x, cy, glyphA);
        float labelA = alpha;
        if (!reduced && now - a.relabel < 0.16) labelA *= 0.35f + 0.65f * (float)((now - a.relabel) / 0.16);
        const SDL_Color ink = a.sys ? Faint() : Muted();
        DrawTextVC(a.label, x + ButtonGlyphW(it->first) + 9, cy, Alpha(ink, (int)(255 * labelA)), Font::Body);
        ++it;
    }
}

}  // namespace W
