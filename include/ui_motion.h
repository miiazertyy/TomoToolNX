#pragma once
// ui_motion.h — TomoToolNX
// Time and easing for the interface, with LLM-Selfbot's curves
// (webui/src/app.css: --spring, --jelly, --swift) so things move the way they
// move there.

#include <cstdint>

namespace Motion {

void   BeginFrame();          // once per frame, before input
double Now();                 // seconds since start (slowed by SetSlow)
float  Dt();                  // seconds since the previous frame (slowed, clamped)
void   SetSlow(float factor); // 1 = normal; 8 = eight times slower (frame captures)
// > 0: every frame moves the clock exactly this far, however long it took
// (the tour recorder, ui_tour.h). 0: real time again.
void   SetFixedStep(double seconds);
bool   Reduced();             // settings: reduce motion

enum class Curve : uint8_t {
    Linear,
    Spring,     // gentle overshoot: entrances, layout moves, the focus plate
    Jelly,      // pronounced wobble: presses, toggles, pops
    Swift,      // cubic-bezier(0.2, 0.9, 0.25, 1): quick, no overshoot
    BackOut,    // svelte backOut: page slides
    CubicOut, CubicIn, QuintOut,
    Knob,       // cubic-bezier(0.3, 1.3, 0.5, 1): a switch's knob, the plate's bump
    EaseOut,    // cubic-bezier(0, 0, 0.58, 1)
};
float Ease(Curve c, float t);       // t in 0..1 (clamped); may overshoot 1
float Bezier(float x1, float y1, float x2, float y2, float t);

// A number on its way somewhere.
struct Tween {
    float  from = 0, to = 0;
    double t0 = -1;
    float  dur = 0.3f;
    Curve  curve = Curve::Swift;
    void  Snap(float v) { from = to = v; t0 = -1; }
    void  To(float v, float duration, Curve c);
    float Value() const;
    float Progress() const;         // 0..1 (1 when idle)
    bool  Busy() const { return Progress() < 1.f; }
};

// The press squash of LLM-Selfbot (scale 0.96 → 1.012 → 1 over 300 ms), per id.
void  Press(uint32_t id);
float PressScale(uint32_t id);
// When an id was first seen in a run of frames: for things that animate in.
double FirstSeen(uint32_t id);
void  Forget(uint32_t id);
void  Sweep();                      // drop state not used for a while (call once per frame)

}  // namespace Motion
