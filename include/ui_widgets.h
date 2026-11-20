#pragma once
// ui_widgets.h — TomoToolNX
// The pieces the 2.0 screens are built from, after LLM-Selfbot's desktop panel
// (sidebar, page header, setting cards and rows, switches, sliders, chips) with
// the controller feel of its Big Picture mode (the focus plate, the sounds).
// Each widget draws itself and registers what pressing it does (ui_focus.h).

#include "ui_art.h"
#include "ui_draw.h"
#include "ui_focus.h"
#include "ui_icons.h"
#include "ui_kit.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace W {
using Ui::Id;

// ── Arrival ──────────────────────────────────────────────────────────────────
// The shell calls Arrive() when a page or a sheet opens: blocks drop in, lists
// domino in and switches slide on from that moment, with their sounds.
void   Arrive(bool withSound = true);
// Keep the page's blocks hidden until Arrive(): the app's first frames, which
// are slow, so the drop-in and its sounds start once the picture is moving.
void   HoldArrival();
// Only a part of the page changed (a new tab, another Mii): what is drawn in
// BeginContentBlock() for that part (0..3) arrives, the rest stays still
// (LLM-Selfbot's tabs).
void   ArriveContent(int part = 0);
double ArrivedAt();
// A block that drops into place (LLM-Selfbot's cascade): from 12 px above at
// scale 0.975, delayed by how far down the page it sits.
void BeginBlock(SDL_Rect r);
void BeginContentBlock(SDL_Rect r, int part = 0);
void EndBlock();
// The press squash around a widget (scale about its centre).
void BeginPress(Id id, SDL_Rect r);
void EndPress();

// ── Surfaces ─────────────────────────────────────────────────────────────────
void Card(SDL_Rect r);
void Hairline(int x, int y, int w);
void Eyebrow(int x, int y, const std::string& text, SDL_Color c);
void IconTile(int x, int y, int size, UiIcons::Id icon, SDL_Color tint);
// A page header: icon tile, eyebrow, title, description and an optional pill.
// Returns the y below it. With `back`, a sub-page: the icon tile becomes a
// Back button (for touch; B does the same).
int  PageHeader(int x, int y, int w, UiIcons::Id icon, const std::string& eyebrow, const std::string& title,
                const std::string& desc, const std::string& pill = std::string(), Font titleFont = Font::Title,
                std::function<void()> back = nullptr);
void Pill(int x, int y, const std::string& text, Ui::Tone tone, bool dot = false);
int  PillW(const std::string& text, bool dot = false);

// ── Sidebar ──────────────────────────────────────────────────────────────────
void SidebarGroup(int x, int y, const std::string& label, float alpha = 1.f);
// words: how much of the item's name shows (1 all, 0 the icon alone, for a
// folded sidebar; its dot then sits on the icon's shoulder).
void SidebarItem(Id id, SDL_Rect r, UiIcons::Id icon, const std::string& label, bool active, bool dot,
                 std::function<void()> go, float words = 1.f);
// The marks of the active item: the tinted pill (lands) and the accent bar
// (slides from the previous item). Call before the items. follow: the item
// itself is moving (the sidebar folding): the marks go with it, no landing.
void SidebarMarks(Id groupId, SDL_Rect active, bool follow = false);

// ── Rows (in a card) ─────────────────────────────────────────────────────────
constexpr int kRowH = 76;          // a row with a title and a description
constexpr int kRowHSmall = 60;     // title only
struct RowText { std::string title, desc; };
// Opens something: a value on the right and a chevron.
// valueInk: the value is the thing itself (a name), drawn as such; else it is
// a hint beside the row (a folder, "same as the name") and stays dim.
void RowLink  (Id id, SDL_Rect r, const RowText& t, const std::string& value, std::function<void()> press,
               UiIcons::Id icon = UiIcons::Id::ChevronRight, bool danger = false, uint16_t flags = 0, bool valueInk = false);
void RowToggle(Id id, SDL_Rect r, const RowText& t, bool on, std::function<void(bool)> set);
// ◀ value ▶: Left/Right step it; A (optional) opens a keyboard or a list.
// `frac` (0..1) is where the value sits in its range, for the sound's pitch.
// `dots`: colour dots shown in front of the value (the theme row).
void RowStepper(Id id, SDL_Rect r, const RowText& t, const std::string& value, float frac, float stepFrac,
                std::function<void(int)> step, std::function<void()> press = nullptr, const std::string& pressHint = "",
                bool enabled = true, const std::vector<SDL_Color>& dots = {}, Art::Id art = Art::Id::Count);
void RowSlider(Id id, SDL_Rect r, const RowText& t, float frac, float stepFrac, const std::string& value,
               std::function<void(int)> step, std::function<void()> press = nullptr);
// A few short options shown side by side (the WebUI's segmented buttons):
// ◀ ▶ step along them, a tap picks the one under the finger.
void RowSegments(Id id, SDL_Rect r, const RowText& t, const std::vector<std::string>& options, int current,
                 std::function<void(int)> pick);
// Something chosen from a list, shown with its picture (nullptr: an empty
// slot, a dashed "+"): what it is over its name, then its colours as dots,
// the one chosen ringed, ◀ ▶ walking them. A press opens the list.
void RowItem(Id id, SDL_Rect r, SDL_Texture* pic, const RowText& t, const std::vector<SDL_Color>& colours, int sel,
             std::function<void(int)> step, std::function<void()> press, const std::string& pressHint);
void RowSwatch(Id id, SDL_Rect r, const RowText& t, SDL_Color swatch, const std::string& value, std::function<void()> press);
// A choice among colours: ◀ ▶ move along them, a tap steps toward the tap.
void RowSwatches(Id id, SDL_Rect r, const RowText& t, const SDL_Color* colors, int count, int selected,
                 std::function<void(int)> step, std::function<void()> press = nullptr, const std::string& pressHint = "");

// ── Things to press ──────────────────────────────────────────────────────────
enum class Kind { Neutral, Primary, Danger, Soft };
void Button(Id id, SDL_Rect r, const std::string& label, Kind kind, std::function<void()> press,
            UiIcons::Id icon = UiIcons::Id::Count, bool enabled = true, uint16_t flags = 0);
// A row of choices with a gliding highlight and optional counts. `icons`, when
// given, puts the game's art in front of each label (Art::Id::Count: none).
void Chips(Id id, int x, int y, const std::vector<std::pair<std::string, int>>& items, int selected,
           std::function<void(int)> pick, int maxW = 0, const std::vector<Art::Id>& icons = {});
int  ChipsW(const std::vector<std::pair<std::string, int>>& items, const std::vector<Art::Id>& icons = {});
// A card that opens something: big icon tile, title, sub.
void Tile(Id id, SDL_Rect r, UiIcons::Id icon, const std::string& title, const std::string& sub, std::function<void()> press,
          SDL_Color tint);
// A big choice ("Back up now" / "Skip"): icon on top, title, a line or two.
// All choices look alike until highlighted; `tint` colours only the icon.
void ChoiceTile(Id id, SDL_Rect r, UiIcons::Id icon, SDL_Color tint, const std::string& title, const std::string& desc,
                std::function<void()> press, uint16_t flags = 0);
// A person's card: picture (or initial), name, a line under it.
void PersonCard(Id id, SDL_Rect r, SDL_Texture* picture, const std::string& name, const std::string& sub,
                std::function<void()> press, uint16_t flags = 0);
// A spinner for work of unknown length.
void Spinner(int cx, int cy, int r, SDL_Color c);
// A progress bar (0..1) with LLM-Selfbot's accent-2 → accent fill.
void ProgressBar(SDL_Rect r, float frac);

// ── People and pictures ──────────────────────────────────────────────────────
// `face`: -1 the initial; 0 a boy's face, 1 a girl's (the game's Mii editor).
// `hair`: the Mii's hair colour (alpha 0: the picture's own black).
// `picture`: the Mii's real picture (mii_art.h), drawn instead when there is one.
void Avatar(int cx, int cy, int r, const std::string& name, uint32_t seed, int face = -1,
            SDL_Color hair = SDL_Color{ 0, 0, 0, 0 }, SDL_Texture* picture = nullptr);
SDL_Color AvatarColor(uint32_t seed);

// ── Value controls (drawn by the rows; usable alone) ─────────────────────────
void SwitchDraw(Id id, int x, int cy, bool on, bool focused);
void SliderDraw(Id id, int x, int cy, int w, float frac, bool focused);

// ── Layers ───────────────────────────────────────────────────────────────────
struct DialogButton { std::string label; Kind kind; std::function<void()> fn; bool enabled = true; };
// A centred dialog. Focus starts on `focusIndex` (put Cancel first for
// destructive ones). B runs `onBack`.
void Dialog(Id id, UiIcons::Id icon, Ui::Tone tone, const std::string& title, const std::string& body,
            const std::vector<DialogButton>& buttons, int focusIndex, std::function<void()> onBack,
            const std::string& backHint);
// `dots`: colours shown as overlapping dots in place of the icon (themes).
// `art`: one of the game's pictures in place of the icon (texture kinds).
struct MenuItem { std::string label, sub; UiIcons::Id icon; bool danger; std::function<void()> fn; bool checked;
                  std::vector<SDL_Color> dots = {}; Art::Id art = Art::Id::Count;
                  const char* item = nullptr; };   // a Mii item's picture (Art::ItemPicture), by asset name
// Overlapping colour dots, centred vertically on cy; returns their width.
int SwatchDots(int x, int cy, const std::vector<SDL_Color>& dots, int r = 7);
// A list that unrolls (LLM-Selfbot's menus): picks play `select`. With an
// `anchor` (the row that opened it) it drops down from that row's control,
// or up when there is no room below; without one it sits in the middle.
void Menu(Id id, const std::string& title, const std::vector<MenuItem>& items, std::function<void()> onClose,
          const std::string& closeHint, int focusIndex = 0, SDL_Rect anchor = SDL_Rect{});
// A long menu of pictures can be seen as a grid (Y, or the switch in its
// corner). The choice holds for every such menu; the config keeps it, saved
// through the hook when it changes.
extern bool gMenuGrid;
extern void (*gOnMenuGridChanged)();
// A list to pick one of, opened from a row and drawn over the page by the
// shell (DrawChoice, once a frame after the page). `pick` gets the index.
void OpenChoice(Id id, const std::string& title, std::vector<std::string> options, int current,
                std::function<void(int)> pick, SDL_Rect anchor = SDL_Rect{}, std::vector<Art::Id> art = {});
void DrawChoice();
// A month to pick a day in (birthdays): ◀ ▶ (and L R) change the month, ZL ZR
// the year; A on a day picks it. Drawn over the page by the shell.
void OpenCalendar(Id id, const std::string& title, int y, int m, int d, int yearLo, int yearHi,
                  std::function<void(int y, int m, int d)> pick);
void DrawCalendar();
// A stepper over named options (◀ ▶ walk them) whose press opens them all as
// a list (OpenChoice). `step(d)` moves by d, as RowStepper's does.
void RowChoice(Id id, SDL_Rect r, const RowText& t, const std::vector<std::string>& options, int current,
               std::function<void(int)> step, bool enabled = true, std::vector<Art::Id> art = {});

// ── The hint bar ─────────────────────────────────────────────────────────────
// Page actions packed against `right` (A last), always-there buttons (L R, −)
// small at `left`.
void HintBar(const std::vector<Focus::Hint>& hints, int left, int right, int cy, const std::string& adjustLabel);
// A button drawn the console's way ("A", "L R", "ZL ZR", "+", "-", "◀▶").
int  ButtonGlyphW(const std::string& key);
void ButtonGlyph(const std::string& key, int x, int cy, float alpha = 1.f);

// Helpers
std::string Ellipsize(const std::string& s, Font f, int w);
// Right-aligned text in at most maxW: cut with "..." at rest; while `active`
// (its row highlighted), a longer one scrolls by like a metro's sign, then
// starts over, so all of it can be read.
void MarqueeR(Id id, const std::string& text, int rx, int cy, int maxW, SDL_Color col, Font f, bool active);
// Where Marquee's text stands: by its top-left corner (as DrawText), by its
// left edge and vertical middle (DrawTextVC), or centred (DrawTextC).
enum class Anchor : uint8_t { Top, Middle, Centre };
// The same for any text: whatever does not fit ends in "..." at rest and runs
// by while `active` (its row, tile or card has the highlight). A second line
// of the same row takes its own id (Ui::IdMix(row, n)).
void Marquee(Id id, const std::string& text, int x, int y, int maxW, SDL_Color col, Font f, bool active,
             Anchor a = Anchor::Top);

}  // namespace W
