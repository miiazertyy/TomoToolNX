// ui_kit.cpp — TomoToolNX
// Cards, buttons, pills, key hints, switches and bars. See ui_kit.h.

#include "ui_kit.h"
#include "ui_input.h"

#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace Ui {

SDL_Color ToneColor(Tone t) {
    switch (t) {
        case Tone::Accent:
        case Tone::Primary: return gTheme.gold;
        case Tone::Second:  return gTheme.accent;
        case Tone::Good:    return gTheme.green;
        case Tone::Bad:     return gTheme.red;
        case Tone::Warn:    return gTheme.warn;
        default:            return gTheme.text;
    }
}

// Ink that reads on a solid fill of `c`.
static SDL_Color InkOn(SDL_Color c) {
    return ColorLuma(c) > 140 ? SDL_Color{ 14, 14, 20, 255 } : SDL_Color{ 250, 250, 255, 255 };
}

// ── Containers ───────────────────────────────────────────────────────────────
void Card(int x, int y, int w, int h, bool raised) {
    if (raised) DrawShadow(x, y, w, h, UiRadiusFor(w, h));
    FillRect(x, y, w, h, gTheme.panel);
    DrawRect(x, y, w, h, Alpha(gTheme.border, 150));
}

void Tile(int x, int y, int w, int h, bool focused) {
    FillRect(x, y, w, h, focused ? gTheme.sel : gTheme.panel2);
}

void Dialog(int x, int y, int w, int h, Tone tone) {
    const int r = UiRadiusFor(w, h);
    DrawShadow(x, y, w, h, r);
    FillRectRounded(x, y, w, h, r, Mix(gTheme.panel, gTheme.text, 0.025f));
    const SDL_Color edge = tone == Tone::Neutral ? gTheme.border : Mix(gTheme.border, ToneColor(tone), 0.55f);
    DrawRectRounded(x, y, w, h, r, edge);
}

int DialogHead(int x, int y, int w, UiIcons::Id icon, Tone tone, const std::string& title) {
    const SDL_Color c = tone == Tone::Neutral ? gTheme.dim : ToneColor(tone);
    const int cx = x + w / 2;
    FillCircle(cx, y + 46, 25, Alpha(c, 40));
    UiIcons::Draw(icon, cx, y + 46, 26, c);
    DrawTextC(FitText(title, Font::Lg, w - 60), cx, y + 100, gTheme.text, Font::Lg);
    return y + 126;
}

void Well(int x, int y, int w, int h, bool active) {
    FillRect(x, y, w, h, gTheme.well);
    DrawRect(x, y, w, h, active ? Alpha(gTheme.gold, 190) : Alpha(gTheme.border, 200));
}

// ── Text ─────────────────────────────────────────────────────────────────────
static size_t NextCharEnd(const std::string& s, size_t i) {
    if (i >= s.size()) return s.size();
    unsigned char c = (unsigned char)s[i];
    size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
    return std::min(s.size(), i + n);
}

// A character of a script written without spaces (Chinese, Japanese, Korean,
// full-width forms): a line may break before or after any of them. Accented
// Latin letters, Cyrillic, Greek and punctuation belong to their word.
static bool BreaksAnywhere(const std::string& s, size_t i) {
    const unsigned char b = (unsigned char)s[i];
    uint32_t cp = 0;
    if ((b & 0xF0) == 0xE0 && i + 2 < s.size())
        cp = ((uint32_t)(b & 0x0F) << 12) | ((uint32_t)(s[i + 1] & 0x3F) << 6) | (uint32_t)(s[i + 2] & 0x3F);
    else if ((b & 0xF8) == 0xF0 && i + 3 < s.size())
        cp = ((uint32_t)(b & 0x07) << 18) | ((uint32_t)(s[i + 1] & 0x3F) << 12) | ((uint32_t)(s[i + 2] & 0x3F) << 6) | (uint32_t)(s[i + 3] & 0x3F);
    else
        return false;
    return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFF00 && cp <= 0xFFEF) || cp >= 0x20000;
}

// Closing punctuation of those scripts never starts a line: it hangs at the end
// of the line before.
static bool NoLineStart(const std::string& piece) {
    static const char* const kClose[] = { "\xEF\xBC\x8C", "\xE3\x80\x82", "\xE3\x80\x81", "\xEF\xBC\x81", "\xEF\xBC\x9F", "\xEF\xBC\x9A",
                                          "\xEF\xBC\x9B", "\xEF\xBC\x89", "\xE3\x80\x8D", "\xE3\x80\x8F", "\xE3\x80\x91", "\xE3\x80\x8B",
                                          "\xE3\x80\x89", "\xE3\x83\xBB", "\xE3\x83\xBC" };   // ，。、！？：；）」』】》〉・ー
    for (const char* k : kClose) if (piece == k) return true;
    return false;
}

std::vector<std::string> WrapLines(const std::string& text, Font f, int maxW, int maxLines) {
    std::vector<std::string> lines;
    if (text.empty() || maxW <= 0 || maxLines <= 0) return lines;
    std::string cur;
    size_t i = 0;
    while (i < text.size()) {
        // The next piece: a word with the spaces after it, or one character
        // of a script written without spaces.
        size_t j = i;
        if (text[i] == ' ') { while (j < text.size() && text[j] == ' ') j++; }
        else {
            while (j < text.size() && text[j] != ' ' && !BreaksAnywhere(text, j)) j = NextCharEnd(text, j);
            if (j == i) j = NextCharEnd(text, i);                 // one character of such a script
            else while (j < text.size() && text[j] == ' ') j++;   // keep the trailing spaces
        }
        const std::string piece = text.substr(i, j - i);
        if (!cur.empty() && TextW(cur + piece, f) > maxW && !NoLineStart(piece)) {
            while (!cur.empty() && cur.back() == ' ') cur.pop_back();
            lines.push_back(cur);
            cur.clear();
            if ((int)lines.size() == maxLines) {
                // Out of room: the rest goes on the last line, shortened.
                lines.back() = FitText(lines.back() + " " + text.substr(i), f, maxW);
                return lines;
            }
            if (piece[0] == ' ') { i = j; continue; }              // no leading spaces on a new line
        }
        cur += piece;
        // A single word wider than the line: cut it.
        if (TextW(cur, f) > maxW && cur.find(' ') == std::string::npos) cur = FitText(cur, f, maxW);
        i = j;
    }
    while (!cur.empty() && cur.back() == ' ') cur.pop_back();
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

int TextBlockC(const std::string& text, int cx, int y, int maxW, SDL_Color col, Font f, int maxLines, int lineGap) {
    const int lh = TextH(f);
    for (const std::string& l : WrapLines(text, f, maxW, maxLines)) {
        DrawTextC(l, cx, y + lh / 2, col, f);
        y += lh + lineGap;
    }
    return y;
}

void Eyebrow(const std::string& text, int x, int y, SDL_Color col) {
    DrawText(UpperUtf8(text), x, y, col, Font::Xs);
}
void EyebrowC(const std::string& text, int cx, int y, SDL_Color col) {
    const std::string up = UpperUtf8(text);
    DrawText(up, cx - TextW(up, Font::Xs) / 2, y, col, Font::Xs);
}

// ── Things to press ──────────────────────────────────────────────────────────
// A button is a tint of its own colour over the card: light at rest, deeper
// when the controller is on it. No hard outline, only a hairline of the tint.
static void ButtonFace(int x, int y, int w, int h, Tone tone, bool focused, bool enabled, SDL_Color& ink) {
    const int r = UiRadiusFor(w, h);
    if (tone == Tone::Primary && enabled) {
        const SDL_Color fill = focused ? Mix(gTheme.gold, SDL_Color{255, 255, 255, 255}, 0.14f) : gTheme.gold;
        FillRectRounded(x, y, w, h, r, fill);
        ink = InkOn(fill);
        return;
    }
    const SDL_Color c = ToneColor(tone);
    if (tone == Tone::Neutral) {
        FillRectRounded(x, y, w, h, r, focused ? gTheme.sel : Mix(gTheme.panel, gTheme.text, 0.07f));
        DrawRectRounded(x, y, w, h, r, Alpha(gTheme.border, focused ? 0 : 170));
        ink = gTheme.text;
    } else {
        FillRectRounded(x, y, w, h, r, Mix(gTheme.panel, c, focused ? 0.30f : 0.13f));
        DrawRectRounded(x, y, w, h, r, Alpha(c, focused ? 0 : 70));
        ink = Mix(c, gTheme.text, focused ? 0.45f : 0.12f);
    }
    if (!enabled) ink = gTheme.faint;
}

void Button(int x, int y, int w, int h, const std::string& label, Tone tone, bool focused, Font font, bool enabled) {
    SDL_Color ink;
    ButtonFace(x, y, w, h, tone, focused, enabled, ink);
    DrawTextC(FitText(label, font, w - 16), x + w / 2, y + h / 2, ink, font);
}

void IconButton(int x, int y, int w, int h, UiIcons::Id icon, const std::string& label,
                Tone tone, bool focused, Font font, bool enabled) {
    SDL_Color ink;
    ButtonFace(x, y, w, h, tone, focused, enabled, ink);
    const int isz = std::min(h - 14, 22);
    if (label.empty()) {
        UiIcons::Draw(icon, x + w / 2, y + h / 2, isz, ink);
        return;
    }
    // A long label (some languages) wins over the icon.
    if (TextW(label, font) > w - isz - 26) {
        DrawTextC(FitText(label, font, w - 16), x + w / 2, y + h / 2, ink, font);
        return;
    }
    const std::string fit = FitText(label, font, w - isz - 26);
    const int tw = TextW(fit, font), gap = 8;
    const int left = x + (w - (isz + gap + tw)) / 2;
    UiIcons::Draw(icon, left + isz / 2, y + h / 2, isz, ink);
    DrawTextVC(fit, left + isz + gap, y + h / 2, ink, font);
}

void KeyButton(int x, int y, int w, int h, const std::string& key, const std::string& label,
               Tone tone, bool focused, Font font) {
    SDL_Color ink;
    ButtonFace(x, y, w, h, tone, focused, true, ink);
    const int kw = KeyCapW(key), gap = 10;
    const std::string fit = FitText(label, font, w - kw - gap - 24);
    const int tw = TextW(fit, font);
    const int left = x + (w - (kw + gap + tw)) / 2;
    KeyCap(left, y + h / 2, key);
    DrawTextVC(fit, left + kw + gap, y + h / 2, ink, font);
}

void Face(int x, int y, int w, int h, SDL_Color edge, bool focused) {
    const int r = UiRadiusFor(w, h);
    if (ColorChroma(edge) < 40) {
        FillRectRounded(x, y, w, h, r, focused ? gTheme.sel : Mix(gTheme.panel, gTheme.text, 0.07f));
        DrawRectRounded(x, y, w, h, r, Alpha(gTheme.border, focused ? 0 : 170));
    } else {
        FillRectRounded(x, y, w, h, r, Mix(gTheme.panel, edge, focused ? 0.30f : 0.13f));
        DrawRectRounded(x, y, w, h, r, Alpha(edge, focused ? 0 : 80));
    }
}

int PillW(const std::string& label, bool dot) {
    return TextW(label, Font::Xs) + 20 + (dot ? 12 : 0);
}

void Pill(int x, int y, const std::string& label, Tone tone, bool dot) {
    const int h = 22, w = PillW(label, dot);
    const SDL_Color c = ToneColor(tone);
    if (tone == Tone::Neutral) {
        FillRectRounded(x, y, w, h, h / 2, Alpha(gTheme.text, 18));
    } else {
        FillRectRounded(x, y, w, h, h / 2, Alpha(c, 38));
    }
    int tx = x + 10;
    if (dot) { FillCircle(x + 12, y + h / 2, 3, c); tx += 12; }
    DrawTextVC(label, tx, y + h / 2, tone == Tone::Neutral ? gTheme.dim : Mix(c, gTheme.text, 0.25f), Font::Xs);
}

int ChipW(const std::string& label, Font font) { return TextW(label, font) + 28; }

void Chip(int x, int y, int w, int h, const std::string& label, bool picked, bool focused, Font font) {
    if (picked) {
        FillRectRounded(x, y, w, h, h / 2, Alpha(gTheme.gold, focused ? 80 : 52));
        DrawRectRounded(x, y, w, h, h / 2, Alpha(gTheme.gold, 90));
    } else {
        FillRectRounded(x, y, w, h, h / 2, Alpha(gTheme.text, focused ? 26 : 12));
    }
    DrawTextC(FitText(label, font, w - 12), x + w / 2, y + h / 2, picked ? gTheme.text : gTheme.dim, font);
}

void Toggle(int x, int cy, bool on, bool focused) {
    const int w = 44, h = 24, y = cy - h / 2;
    FillRectRounded(x, y, w, h, h / 2, on ? gTheme.gold : Mix(gTheme.panel, gTheme.text, 0.16f));
    const int kr = 9;
    const int kx = on ? x + w - 3 - kr : x + 3 + kr;
    FillCircle(kx, cy, kr, on ? InkOn(gTheme.gold) : gTheme.dim);
    if (focused) DrawFocusRing(x - 3, y - 3, w + 6, h + 6, (h + 6) / 2);
}

// ── Showing an amount ────────────────────────────────────────────────────────
void Progress(int x, int y, int w, int h, float frac, SDL_Color col) {
    if (frac < 0.f) frac = 0.f;
    if (frac > 1.f) frac = 1.f;
    FillRectRounded(x, y, w, h, h / 2, Mix(gTheme.panel, gTheme.text, 0.10f));
    int fw = (int)(w * frac + 0.5f);
    if (fw > 0) {
        if (fw < h) fw = h;                         // never thinner than its own cap
        FillRectRounded(x, y, fw, h, h / 2, col);
    }
}

void Spinner(int cx, int cy, int r, SDL_Color col) {
    // Eight dots on a circle, brightest at the head, turning once a second.
    const float head = fmodf(gElapsedSec * 8.f, 8.f);
    for (int i = 0; i < 8; i++) {
        const float ang = (float)i / 8.f * 2.f * (float)M_PI;
        float behind = head - (float)i;
        if (behind < 0.f) behind += 8.f;
        const int a = 40 + (int)(215.f * (1.f - behind / 8.f));
        FillCircle(cx + (int)(cosf(ang) * r), cy + (int)(sinf(ang) * r), std::max(2, r / 6), Alpha(col, a));
    }
}

// ── Button hints ─────────────────────────────────────────────────────────────
static std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && s[a] == ' ') a++;
    while (b > a && s[b - 1] == ' ') b--;
    return s.substr(a, b - a);
}

std::vector<std::pair<std::string, std::string>> ParseHints(const std::string& hints) {
    std::vector<std::pair<std::string, std::string>> out;
    // 1. Groups are separated by three or more spaces ("A  select    B  back").
    //    The bracket style ("[A] confirm  [B] cancel") separates groups with
    //    two, so there a new group starts at every "  [".
    std::vector<std::string> groups;
    {
        std::string cur;
        for (size_t i = 0; i < hints.size();) {
            if (hints[i] == ' ') {
                size_t j = i;
                while (j < hints.size() && hints[j] == ' ') j++;
                const size_t run = j - i;
                const bool bracketNext = j < hints.size() && hints[j] == '[';
                if (run >= 3 || (run == 2 && bracketNext && !cur.empty() && cur[0] == '[')) {
                    if (!Trim(cur).empty()) groups.push_back(Trim(cur));
                    cur.clear();
                } else {
                    cur.append(run, ' ');
                }
                i = j;
            } else {
                cur += hints[i++];
            }
        }
        if (!Trim(cur).empty()) groups.push_back(Trim(cur));
    }
    // 2. Inside a group the key comes first: in brackets, or before a double
    //    space, or else it is the first word.
    for (const std::string& g : groups) {
        std::string key, label;
        if (g[0] == '[') {
            const size_t close = g.find(']');
            if (close != std::string::npos) { key = g.substr(1, close - 1); label = Trim(g.substr(close + 1)); }
            else label = g;
        } else {
            const size_t dbl = g.find("  ");
            if (dbl != std::string::npos) { key = Trim(g.substr(0, dbl)); label = Trim(g.substr(dbl)); }
            else {
                const size_t sp = g.find(' ');
                if (sp != std::string::npos) { key = g.substr(0, sp); label = Trim(g.substr(sp)); }
                else label = g;       // one word: nothing to split, show it as text
            }
        }
        // A "key" this long is a sentence, not a button name.
        if (key.size() > 22) { label = g; key.clear(); }
        // Labels start with a capital whatever the language file says.
        out.emplace_back(key, key.empty() ? label : CapFirstUtf8(label));
    }
    return out;
}

static const int kCapH = 20;

int KeyCapW(const std::string& key) {
    return std::max(kCapH + 2, TextW(key, Font::Xs) + 14);
}

void KeyCap(int x, int cy, const std::string& key, bool lit) {
    const int w = KeyCapW(key), y = cy - kCapH / 2;
    FillRectRounded(x, y, w, kCapH, 7, lit ? Alpha(gTheme.gold, 70) : Alpha(gTheme.text, 24));
    DrawTextC(key, x + w / 2, cy, lit ? gTheme.text : Mix(gTheme.dim, gTheme.text, 0.55f), Font::Xs);
}

int HintsAt(const std::string& hints, int cx, int cy, int maxW) {
    const auto items = ParseHints(hints);
    if (items.empty()) return 0;
    // Measure, then tighten the spacing until the row fits; if it still does
    // not, fall back to the smaller face for the labels.
    Font lf = Font::Sm;
    int gapItem = 22;
    const int gapKey = 7;
    auto measure = [&]() {
        int w = 0;
        for (size_t i = 0; i < items.size(); i++) {
            if (i) w += gapItem;
            if (!items[i].first.empty())  w += KeyCapW(items[i].first);
            if (!items[i].first.empty() && !items[i].second.empty()) w += gapKey;
            if (!items[i].second.empty()) w += TextW(items[i].second, lf);
        }
        return w;
    };
    int total = measure();
    while (total > maxW && gapItem > 10) { gapItem -= 3; total = measure(); }
    if (total > maxW) { lf = Font::Xs; total = measure(); }
    int x = cx - total / 2;
    if (x < 8) x = 8;
    for (size_t i = 0; i < items.size(); i++) {
        if (i) x += gapItem;
        if (!items[i].first.empty()) {
            // A tap on a hint presses its button (Input::HintZone).
            const int iw = KeyCapW(items[i].first) + (items[i].second.empty() ? 0 : gapKey + TextW(items[i].second, lf));
            const SDL_Rect zone{ x - 10, cy - 22, iw + 20, 44 };
            Input::HintZone(items[i].first, zone);
            float ft = 0.f;
            if (Input::HintFlash(zone, ft))
                FillRectRounded(zone.x, zone.y + 4, zone.w, zone.h - 8, (zone.h - 8) / 2, Alpha(gTheme.gold, (int)(110 * (1.f - ft))));
            KeyCap(x, cy, items[i].first);
            x += KeyCapW(items[i].first);
            if (!items[i].second.empty()) x += gapKey;
        }
        if (!items[i].second.empty()) {
            DrawTextVC(items[i].second, x, cy, gTheme.dim, lf);
            x += TextW(items[i].second, lf);
        }
    }
    return total;
}

void Hints(const std::string& hints) {
    if (hints.empty()) return;
    HintsAt(hints, SCREEN_W / 2, SCREEN_H - UI_FOOTER_H / 2 - 1);
}

// ── Toast ────────────────────────────────────────────────────────────────────
namespace {
std::string sToastText;
Tone        sToastTone = Tone::Neutral;
float       sToastLeft = 0.f, sToastAge = 0.f;
}  // namespace

void ToastShow(const std::string& text, Tone tone, float seconds) {
    sToastText = text; sToastTone = tone; sToastLeft = seconds; sToastAge = 0.f;
}
void ToastClear() { sToastLeft = 0.f; sToastText.clear(); }

void ToastDraw() {
    if (sToastLeft <= 0.f || sToastText.empty()) return;
    sToastLeft -= gDtSec; sToastAge += gDtSec;
    // Rises into place over the first 180 ms, fades over the last 300.
    float in  = gReduceMotion ? 1.f : std::min(1.f, sToastAge / 0.18f);
    float out = std::min(1.f, std::max(0.f, sToastLeft / 0.30f));
    in = 1.f - (1.f - in) * (1.f - in);
    const int a = (int)(255.f * std::min(in, out));
    if (a <= 0) return;

    const std::string text = FitText(sToastText, Font::Sm, SCREEN_W - 200);
    const int tw = TextW(text, Font::Sm);
    const bool dot = sToastTone != Tone::Neutral;
    const int w = tw + 36 + (dot ? 16 : 0), h = 38;
    const int x = (SCREEN_W - w) / 2;
    const int y = SCREEN_H - UI_FOOTER_H - h - 14 + (int)((1.f - in) * 14.f);
    FillRectRounded(x - 1, y + 3, w + 2, h, h / 2, SDL_Color{ 0, 0, 0, (uint8_t)(a * 90 / 255) });
    FillRectRounded(x, y, w, h, h / 2, Alpha(Mix(gTheme.panel, gTheme.text, 0.10f), a));
    DrawRectRounded(x, y, w, h, h / 2, Alpha(gTheme.border, a));
    int tx = x + 18;
    if (dot) { FillCircle(x + 20, y + h / 2, 4, Alpha(ToneColor(sToastTone), a)); tx += 16; }
    DrawTextVC(text, tx, y + h / 2, Alpha(gTheme.text, a), Font::Sm);
}

}  // namespace Ui
