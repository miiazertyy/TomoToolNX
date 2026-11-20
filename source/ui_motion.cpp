// ui_motion.cpp — TomoToolNX. See ui_motion.h.

#include "ui_motion.h"
#include "ui_draw.h"

#include <switch.h>

#include <cmath>
#include <unordered_map>

namespace Motion {
namespace {
double sStartTick = -1;
double sLastReal  = 0;
double sNow       = 0;
float  sDt        = 1.f / 60.f;
float  sSlow      = 1.f;
double sFixed     = 0;
uint64_t sFrame   = 0;

// The two CSS linear() curves, with every stop given a position (the
// unpositioned ones spaced evenly between their neighbours, as CSS does).
struct Stop { float x, y; };
const Stop kSpring[] = {
    {0.000f, 0.000f}, {0.014f, 0.006f}, {0.028f, 0.025f}, {0.061f, 0.101f}, {0.189f, 0.539f}, {0.253f, 0.721f},
    {0.315f, 0.849f}, {0.381f, 0.937f}, {0.418f, 0.968f}, {0.457f, 0.991f}, {0.501f, 1.006f}, {0.550f, 1.015f},
    {0.639f, 1.017f}, {1.000f, 1.001f},
};
const Stop kJelly[] = {
    {0.000f, 0.000f}, {0.0105f, 0.009f}, {0.021f, 0.035f}, {0.044f, 0.141f}, {0.067f, 0.281f}, {0.129f, 0.723f},
    {0.167f, 0.938f}, {0.186f, 1.017f}, {0.205f, 1.077f}, {0.224f, 1.121f}, {0.243f, 1.149f}, {0.257f, 1.159f},
    {0.271f, 1.163f}, {0.285f, 1.161f}, {0.299f, 1.154f}, {0.321f, 1.129f}, {0.392f, 1.051f}, {0.431f, 1.017f},
    {0.4705f, 0.991f}, {0.510f, 0.977f}, {0.538f, 0.974f}, {0.571f, 0.975f}, {0.698f, 0.997f}, {0.769f, 1.003f},
    {1.000f, 1.000f},
};
template <size_t N>
float Linear(const Stop (&s)[N], float t) {
    if (t <= 0) return 0;
    if (t >= 1) return s[N - 1].y;
    for (size_t i = 1; i < N; i++)
        if (t <= s[i].x) {
            const float u = (t - s[i - 1].x) / (s[i].x - s[i - 1].x);
            return s[i - 1].y + (s[i].y - s[i - 1].y) * u;
        }
    return s[N - 1].y;
}

struct Fx { double press = -1; double seen = -1; uint64_t usedFrame = 0; };
std::unordered_map<uint32_t, Fx> sFx;
}  // namespace

void BeginFrame() {
    const double real = (double)armGetSystemTick() / (double)armGetSystemTickFreq();
    if (sStartTick < 0) { sStartTick = real; sLastReal = real; }
    double d = real - sLastReal;
    sLastReal = real;
    if (d > 0.1) d = 0.1;               // a stall (mount, keyboard) must not jump animations
    if (d < 0) d = 0;
    if (sFixed > 0) d = sFixed * sSlow; // recording: one frame is one fixed step
    sDt = (float)(d / sSlow);
    sNow += d / sSlow;
    sFrame++;
}
double Now() { return sNow; }
float  Dt() { return sDt; }
void   SetSlow(float f) { sSlow = f < 1.f ? 1.f : f; }
void   SetFixedStep(double s) { sFixed = s > 0 ? s : 0; }
bool   Reduced() { return gReduceMotion; }

float Bezier(float x1, float y1, float x2, float y2, float t) {
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    // Solve x(u) = t for u (Newton, then bisection), return y(u).
    auto bx = [&](float u) { const float v = 1 - u; return 3 * v * v * u * x1 + 3 * v * u * u * x2 + u * u * u; };
    auto by = [&](float u) { const float v = 1 - u; return 3 * v * v * u * y1 + 3 * v * u * u * y2 + u * u * u; };
    auto dx = [&](float u) { const float v = 1 - u; return 3 * v * v * x1 + 6 * v * u * (x2 - x1) + 3 * u * u * (1 - x2); };
    float u = t;
    for (int i = 0; i < 6; i++) {
        const float e = bx(u) - t, d = dx(u);
        if (fabsf(e) < 1e-4f) return by(u);
        if (fabsf(d) < 1e-6f) break;
        u -= e / d;
        if (u < 0 || u > 1) break;
    }
    float lo = 0, hi = 1; u = t;
    for (int i = 0; i < 24; i++) {
        const float e = bx(u) - t;
        if (fabsf(e) < 1e-4f) break;
        if (e > 0) hi = u; else lo = u;
        u = (lo + hi) / 2;
    }
    return by(u);
}

float Ease(Curve c, float t) {
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    switch (c) {
        case Curve::Linear:   return t;
        case Curve::Spring:   return Linear(kSpring, t);
        case Curve::Jelly:    return Linear(kJelly, t);
        case Curve::Swift:    return Bezier(0.2f, 0.9f, 0.25f, 1.f, t);
        case Curve::Knob:     return Bezier(0.3f, 1.3f, 0.5f, 1.f, t);
        case Curve::EaseOut:  return Bezier(0.f, 0.f, 0.58f, 1.f, t);
        case Curve::BackOut: {
            const float s = 1.70158f; const float u = t - 1;
            return u * u * ((s + 1) * u + s) + 1;
        }
        case Curve::CubicOut: { const float u = t - 1; return u * u * u + 1; }
        case Curve::CubicIn:  return t * t * t;
        case Curve::QuintOut: { const float u = 1 - t; return 1 - u * u * u * u * u; }
    }
    return t;
}

void Tween::To(float v, float duration, Curve c) {
    if (v == to && t0 >= 0) return;
    from = Value();
    to = v;
    dur = duration;
    curve = c;
    t0 = sNow;
    if (Reduced() || duration <= 0) { from = to; t0 = -1; }
}
float Tween::Progress() const {
    if (t0 < 0 || dur <= 0) return 1.f;
    const float p = (float)((sNow - t0) / dur);
    return p >= 1.f ? 1.f : (p < 0 ? 0.f : p);
}
float Tween::Value() const {
    const float p = Progress();
    if (p >= 1.f) return to;
    return from + (to - from) * Ease(curve, p);
}

void Press(uint32_t id) {
    Fx& f = sFx[id];
    f.press = sNow;
    f.usedFrame = sFrame;
}
float PressScale(uint32_t id) {
    auto it = sFx.find(id);
    if (it == sFx.end() || it->second.press < 0 || Reduced()) return 1.f;
    it->second.usedFrame = sFrame;
    const float t = (float)((sNow - it->second.press) / 0.3);
    if (t >= 1.f) { it->second.press = -1; return 1.f; }
    // keyframes: 0.96 at 0, 1.012 at 0.55, 1 at 1, ease-out between
    if (t < 0.55f) return 0.96f + (1.012f - 0.96f) * Ease(Curve::EaseOut, t / 0.55f);
    return 1.012f + (1.f - 1.012f) * Ease(Curve::EaseOut, (t - 0.55f) / 0.45f);
}
double FirstSeen(uint32_t id) {
    Fx& f = sFx[id];
    if (f.seen < 0 || sFrame - f.usedFrame > 2) f.seen = sNow;
    f.usedFrame = sFrame;
    return f.seen;
}
void Forget(uint32_t id) { sFx.erase(id); }
void Sweep() {
    if (sFrame % 120 != 0) return;
    for (auto it = sFx.begin(); it != sFx.end();)
        if (sFrame - it->second.usedFrame > 600) it = sFx.erase(it); else ++it;
}

}  // namespace Motion
