#pragma once
// ui_focus.h — TomoToolNX
// Point and press: one highlight that moves to anything on screen.
//
// Every frame, the things that can be pressed register a node (a rectangle and
// what A, ◀ ▶ or the stick do with it). Input is resolved at the start of the
// next frame against exactly what was on screen, so a tap or a press can never
// reach something that is gone. The D-pad moves the highlight to the nearest
// node that way (LLM-Selfbot's spatial navigation: ahead + off-axis × 4 +
// drift × 0.1); with nothing that way the highlight bumps. The highlight is a
// filled plate that springs from one thing to the next (420 ms, --spring).

#include "ui_input.h"

#include <SDL2/SDL.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Ui {
using Id = uint32_t;
constexpr Id HashStr(const char* s, Id h = 2166136261u) { return *s ? HashStr(s + 1, (h ^ (uint8_t)*s) * 16777619u) : h; }
inline Id IdMix(Id a, uint32_t b) { return (a ^ (b * 0x9e3779b1u + 0x7f4a7c15u + (a << 6) + (a >> 2))) * 16777619u; }
}  // namespace Ui

namespace Focus {

enum Flag : uint16_t {
    Disabled = 1 << 0,   // drawn, but the highlight skips it
    AdjustX  = 1 << 1,   // ◀ ▶ change its value instead of moving the highlight
    Default  = 1 << 2,   // where the highlight lands when a page or a sheet opens
    OwnSound = 1 << 3,   // its press plays its own sound (switches)
    NoPlate  = 1 << 4,   // it shows its own focus
    Capture  = 1 << 5,   // every direction goes to `move` (the map canvas)
    Ring     = 1 << 6,   // the highlight is an outline around it, nothing behind
                         // (buttons, chips, cards); otherwise a tinted plate (rows)
};

struct Node {
    Ui::Id id = 0;
    SDL_Rect r{};
    int radius = 12;
    uint16_t flags = 0;
    std::function<void()> press;          // A or a tap
    std::function<void(int)> adjust;      // ◀ -1 / ▶ +1 (AdjustX)
    std::function<void(int, int)> move;   // Capture: dx, dy
    std::string hint;                     // what A does, for the hint bar
    // AdjustX nodes: a tap inside `ctl` steps the value instead of pressing:
    // left of `pivot` (default: the middle of ctl) is -1, right of it +1.
    // Within `dead` px around the pivot a tap presses as usual.
    SDL_Rect ctl{};
    int pivot = -1;
    int dead = 0;
};

void   Add(Node n);
bool   Is(Ui::Id id);                     // has the highlight (on the top layer)
Ui::Id Current();
void   Set(Ui::Id id);                    // put the highlight there (no sound)
bool   Has(Ui::Id id);                    // registered this frame
SDL_Rect RectOf(Ui::Id id);               // this frame's rectangle (zero if absent)
// Is the highlight inside `r` (by where it was last frame)? For a card whose
// title should react while one of its rows or buttons has the highlight.
bool   CurrentIn(SDL_Rect r);

// Layers: only the top one takes input. The base layer always exists.
enum class Layer : uint8_t { Base, Sheet, Dialog, Keys };
void PushLayer(Layer kind, std::function<void()> onBack, const std::string& backHint, SDL_Rect panel = SDL_Rect{});
void PopLayer();
bool LayerIsTop();                        // is the layer being built the top one?

// A scrolling area. Nodes added between Begin and End belong to it; it keeps
// the highlight in view, scrolls with the right stick, and drags with fling.
struct Scroll {
    float offset = 0, target = 0, vel = 0, content = 0;
    // Following the highlight: the list glides with it, the same curve and
    // time as the highlight's own move, so the highlight never wobbles.
    float from = 0, prev = 0;
    double t0 = -1;
};
void BeginScroll(Ui::Id id, SDL_Rect view, Scroll& s);
void EndScroll(float contentHeight);

// Buttons other than A and B, with their hint.
void Command(uint32_t btn, const std::string& hint, std::function<void()> fn);
// B on the base layer.
void SetBaseBack(std::function<void()> fn, const std::string& hint);
// Where the highlight lands on a page it has not been on yet (the page's
// content rather than a sidebar), when no node is marked Default.
void PreferArea(SDL_Rect r);
// Is a command already registered for this button on the layer being built?
bool HasCommand(uint32_t btn);
// Is a sheet or dialog open in what is being built?
bool LayerOpen();

void Resolve(const Input::Frame& in);
void BeginBuild();
void EndBuild();

// The plate under the focused thing: the focused widget calls this before
// drawing its content. A widget that draws itself moved (a card that lifts
// when highlighted) passes where its node rect is drawn, so the plate stays
// centred around it.
void DrawPlate(Ui::Id id, const SDL_Rect* at = nullptr);
SDL_Color PlateColor();
// The highlight's outline (and a plate's edge) as DrawPlate placed them, drawn
// once the page is done, so the things drawn after the highlighted one (the
// next tile, a neighbour) never cover it. The frame calls it last.
void DrawRings();

struct Hint { std::string key, label; };
void Hints(std::vector<Hint>& out);

// Remember where the highlight was on each page.
void EnterPage(uint32_t page);
void SetLogger(void (*fn)(const char*));
// Focused node's centre on screen (0..1), for the backdrop's light.
bool FocusCentre(float& x01, float& y01);

}  // namespace Focus
