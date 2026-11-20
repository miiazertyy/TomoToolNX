#pragma once
// ui_kit.h — TomoToolNX
// The parts the screens are assembled from: cards, buttons, pills, key hints,
// switches, bars. One definition of each, so a button looks and behaves like a
// button wherever it is. Built on the drawing layer in ui_draw.h.

#include "ui_draw.h"
#include "ui_icons.h"
#include <string>
#include <utility>
#include <vector>

namespace Ui {

// What a thing is for decides its colour, never the other way round.
enum class Tone {
    Neutral,   // an ordinary action
    Accent,    // the action the screen is there for (tinted with the accent)
    Primary,   // the same, as a solid accent button: at most one per screen
    Second,    // the second accent: informational, "open", "pick"
    Good,      // create / confirm / all fine
    Bad,       // delete / discard / failed
    Warn,      // needs attention, not broken
};
SDL_Color ToneColor(Tone t);

// ── Containers ───────────────────────────────────────────────────────────────
// A card: the surface things sit on. `raised` adds a soft shadow.
void Card(int x, int y, int w, int h, bool raised = false);
// A tile inside a card (a stat, an option): one step lighter, no outline.
void Tile(int x, int y, int w, int h, bool focused = false);
// The frame of a dialog, drawn over a dimmed screen. `tone` tints its top edge.
void Dialog(int x, int y, int w, int h, Tone tone = Tone::Neutral);
// The top of a dialog: an icon in a circle of the tone's colour, then the
// title, centred on the dialog. Returns the y below the title.
int  DialogHead(int x, int y, int w, UiIcons::Id icon, Tone tone, const std::string& title);
// A sunken field: search boxes, value boxes.
void Well(int x, int y, int w, int h, bool active = false);

// ── Text ─────────────────────────────────────────────────────────────────────
// Break text into lines no wider than maxW. Words wrap at spaces; text without
// spaces (Chinese, Japanese) wraps between characters. Past maxLines the last
// line ends in an ellipsis.
std::vector<std::string> WrapLines(const std::string& text, Font f, int maxW, int maxLines = 4);
// Wrapped and centred on cx, starting at y. Returns the y below the last line.
int TextBlockC(const std::string& text, int cx, int y, int maxW, SDL_Color col, Font f = Font::Sm,
               int maxLines = 4, int lineGap = 4);
// Small capitals above a group: "BACKUPS", "ON THIS CONSOLE".
void Eyebrow(const std::string& text, int x, int y, SDL_Color col = COL_FAINT);
void EyebrowC(const std::string& text, int cx, int y, SDL_Color col = COL_FAINT);

// ── Things to press ──────────────────────────────────────────────────────────
// A button. `focused` means the controller is on it (tint deepens; the caller
// usually also asks for the gliding focus ring at the same rectangle).
void Button(int x, int y, int w, int h, const std::string& label,
            Tone tone = Tone::Neutral, bool focused = false,
            Font font = Font::Md, bool enabled = true);
// The same with an icon before the label (label may be empty: icon only).
void IconButton(int x, int y, int w, int h, UiIcons::Id icon, const std::string& label,
                Tone tone = Tone::Neutral, bool focused = false,
                Font font = Font::Sm, bool enabled = true);

// A button with the controller button that runs it drawn as a key cap
// before the label ("[A] Delete"). For dialogs.
void KeyButton(int x, int y, int w, int h, const std::string& key, const std::string& label,
               Tone tone = Tone::Neutral, bool focused = false, Font font = Font::Md);
// Only the face of a button, for screens that draw their own label on top.
// A grey `edge` makes a plain button, a coloured one tints it.
void Face(int x, int y, int w, int h, SDL_Color edge, bool focused = false);

// A small rounded label: a state, a count, a tag. 22 px tall.
int  PillW(const std::string& label, bool dot = false);
void Pill(int x, int y, const std::string& label, Tone tone = Tone::Neutral, bool dot = false);

// A choice in a row of choices (filters, sub-tabs): quiet until picked.
int  ChipW(const std::string& label, Font font = Font::Sm);
void Chip(int x, int y, int w, int h, const std::string& label, bool picked, bool focused = false,
          Font font = Font::Sm);

// An on/off switch, 44 x 24, centred vertically on cy.
void Toggle(int x, int cy, bool on, bool focused = false);

// ── Showing an amount ────────────────────────────────────────────────────────
void Progress(int x, int y, int w, int h, float frac, SDL_Color col);
// A spinner for work with no known length; `cx, cy` is its centre.
void Spinner(int cx, int cy, int r, SDL_Color col);

// ── Button hints ─────────────────────────────────────────────────────────────
// The row at the bottom of every screen: which buttons do what right now.
// The text is the same string the footer has always used, in any language
// ("A  select    B  back", "ZL/ZR scroll    Up/Down select"); it is split into
// key + label pairs and each key is drawn as a small key cap.
std::vector<std::pair<std::string, std::string>> ParseHints(const std::string& hints);
int  KeyCapW(const std::string& key);
void KeyCap(int x, int cy, const std::string& key, bool lit = false);
// Laid out centred on cx. Returns the width used.
int  HintsAt(const std::string& hints, int cx, int cy, int maxW = SCREEN_W - 48);
// In the footer.
void Hints(const std::string& hints);

// ── Passing messages ─────────────────────────────────────────────────────────
// A message that slides up above the footer and leaves by itself.
void ToastShow(const std::string& text, Tone tone = Tone::Neutral, float seconds = 3.0f);
void ToastDraw();     // once per frame, after the screen
void ToastClear();

}  // namespace Ui
