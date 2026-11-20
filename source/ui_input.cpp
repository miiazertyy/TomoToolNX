// ui_input.cpp — TomoToolNX. See ui_input.h.

#include "ui_input.h"
#include "ui_draw.h"

#include <cmath>
#include <string>
#include <vector>

namespace Input {
namespace {
PadState* sPad = nullptr;
Frame     sF;
double    sNext[4] = { 0, 0, 0, 0 };   // next repeat time per direction: up, down, left, right
// Touch tracking
bool      sDown = false;
SDL_FingerID sFid = 0;
double    sDownAt = 0;
double    sLastMoveAt = 0;
float     sVx = 0, sVy = 0;
constexpr float kSlop = 12.f;           // px a tap may wander
constexpr double kFirstRepeat = 0.38, kNextRepeat = 0.13;
u64       sInjDown = 0, sInjHeld = 0;  // the tour player's buttons for the next Poll
double    sUseTime = -1;
bool      sStickAnalog = false;         // the next Poll leaves the left stick out of the D-pad
// Tappable button hints: drawn this frame (next), tested on the next Poll.
struct Zone { SDL_Rect r; u64 left, right; };
std::vector<Zone> sZones, sZonesNext;
SDL_Rect  sFlashRect{ 0, 0, 0, 0 };
double    sFlashAt = -10;
u64 KeyButton(const std::string& k) {
    if (k == "A") return HidNpadButton_A;
    if (k == "B") return HidNpadButton_B;
    if (k == "X") return HidNpadButton_X;
    if (k == "Y") return HidNpadButton_Y;
    if (k == "L") return HidNpadButton_L;
    if (k == "R") return HidNpadButton_R;
    if (k == "ZL") return HidNpadButton_ZL;
    if (k == "ZR") return HidNpadButton_ZR;
    if (k == "+") return HidNpadButton_Plus;
    if (k == "-" || k == "\xE2\x88\x92") return HidNpadButton_Minus;
    return 0;
}

double Seconds() { return sUseTime >= 0 ? sUseTime : (double)armGetSystemTick() / (double)armGetSystemTickFreq(); }
}  // namespace

void Init(PadState* pad) { sPad = pad; }
const Frame& Last() { return sF; }
void Inject(u64 down, u64 held) { sInjDown |= down; sInjHeld |= held; }
void HintZone(const std::string& key, SDL_Rect r) {
    if (r.w <= 0 || r.h <= 0) return;
    u64 left = 0, right = 0;
    if (key == "\xE2\x97\x80\xE2\x96\xB6" || key == "\xE2\x97\x80 \xE2\x96\xB6") { left = HidNpadButton_Left; right = HidNpadButton_Right; }
    else {
        // "L R", "ZL ZR", "L/R", "ZL/ZR": two buttons, one a half; else one.
        std::string k = key;
        for (char& c : k) if (c == '/') c = ' ';
        const size_t sp = k.find(' ');
        if (sp != std::string::npos) {
            left = KeyButton(k.substr(0, sp));
            size_t q = sp;
            while (q < k.size() && k[q] == ' ') q++;
            right = KeyButton(k.substr(q));
            if (!left || !right) { left = left | right; right = 0; }
        } else left = KeyButton(k);
    }
    if (!left) return;
    sZonesNext.push_back({ r, left, right });
}
bool HintFlash(SDL_Rect r, float& t) {
    const double age = Seconds() - sFlashAt;
    if (age < 0 || age > 0.28) return false;
    if (r.x != sFlashRect.x || r.y != sFlashRect.y || r.w != sFlashRect.w) return false;
    t = (float)(age / 0.28);
    return true;
}
void UseTime(double seconds) { sUseTime = seconds; }
void StickAsAnalog() { sStickAnalog = true; }

bool TakeTap(SDL_Rect r) {
    Frame& f = sF;
    if (!f.tap || f.x < r.x || f.y < r.y || f.x >= r.x + r.w || f.y >= r.y + r.h) return false;
    f.tap = f.pressed = f.released = false;
    return true;
}

const Frame& Poll() {
    const double now = Seconds();
    Frame& f = sF;
    sZones.swap(sZonesNext);
    sZonesNext.clear();
    f.events.clear();
    f.down = f.held = 0;
    f.stepX = f.stepY = 0;
    f.repeat = false;
    f.pressed = f.released = f.tap = false;
    f.dx = f.dy = 0;

    if (sPad) {
        padUpdate(sPad);
        f.rawDown = padGetButtonsDown(sPad);
        f.rawHeld = padGetButtons(sPad);
        const HidAnalogStickState rs = padGetStickPos(sPad, 1);
        auto dz = [](int v) {
            float t = (float)v / 32767.f;
            const float d = 0.18f;
            if (fabsf(t) < d) return 0.f;
            return (t > 0 ? t - d : t + d) / (1.f - d);
        };
        f.rx = dz(rs.x);
        f.ry = -dz(rs.y);
        const HidAnalogStickState ls = padGetStickPos(sPad, 0);
        f.lx = dz(ls.x);
        f.ly = -dz(ls.y);
    }
    if (sInjDown || sInjHeld) {
        f.rawDown |= sInjDown;
        f.rawHeld |= sInjHeld | sInjDown;
        sInjDown = sInjHeld = 0;
    }
    const bool stickIsPad = !sStickAnalog;
    sStickAnalog = false;
    auto map = [stickIsPad](u64 raw) {
        if (!stickIsPad) raw &= ~(u64)(HidNpadButton_StickLUp | HidNpadButton_StickLDown | HidNpadButton_StickLLeft | HidNpadButton_StickLRight);
        uint32_t b = 0;
        if (raw & HidNpadButton_A) b |= A;
        if (raw & HidNpadButton_B) b |= B;
        if (raw & HidNpadButton_X) b |= X;
        if (raw & HidNpadButton_Y) b |= Y;
        if (raw & HidNpadButton_L) b |= L;
        if (raw & HidNpadButton_R) b |= R;
        if (raw & HidNpadButton_ZL) b |= ZL;
        if (raw & HidNpadButton_ZR) b |= ZR;
        if (raw & HidNpadButton_Plus) b |= Plus;
        if (raw & HidNpadButton_Minus) b |= Minus;
        if (raw & (HidNpadButton_Up | HidNpadButton_StickLUp)) b |= Up;
        if (raw & (HidNpadButton_Down | HidNpadButton_StickLDown)) b |= Down;
        if (raw & (HidNpadButton_Left | HidNpadButton_StickLLeft)) b |= Left;
        if (raw & (HidNpadButton_Right | HidNpadButton_StickLRight)) b |= Right;
        return b;
    };
    f.down = map(f.rawDown);
    f.held = map(f.rawHeld);

    // Directions: a step on press, then repeats while held. Vertical wins a
    // diagonal, so a stick pushed a little off-axis still walks a column.
    const uint32_t dirs[4] = { Up, Down, Left, Right };
    int sx = 0, sy = 0; bool rep = false;
    for (int i = 0; i < 4; i++) {
        const uint32_t d = dirs[i];
        bool step = false;
        if (f.down & d) { step = true; sNext[i] = now + kFirstRepeat; }
        else if ((f.held & d) && now >= sNext[i]) { step = true; rep = true; sNext[i] = now + kNextRepeat; }
        if (!step) continue;
        if (i == 0) sy = -1; else if (i == 1) sy = 1; else if (i == 2) sx = -1; else sx = 1;
    }
    if (sy != 0) sx = 0;
    f.stepX = sx; f.stepY = sy; f.repeat = rep;

    // Touch: one finger at a time.
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        f.events.push_back(ev);
        if (ev.type == SDL_FINGERDOWN) {
            sDown = true; sFid = ev.tfinger.fingerId; sDownAt = now; sLastMoveAt = now;
            f.x = f.startX = ev.tfinger.x * SCREEN_W;
            f.y = f.startY = ev.tfinger.y * SCREEN_H;
            f.dragging = false; f.pressed = true; sVx = sVy = 0;
        } else if (ev.type == SDL_FINGERMOTION && sDown && ev.tfinger.fingerId == sFid) {
            const float mx = ev.tfinger.dx * SCREEN_W, my = ev.tfinger.dy * SCREEN_H;
            f.x = ev.tfinger.x * SCREEN_W; f.y = ev.tfinger.y * SCREEN_H;
            f.dx += mx; f.dy += my;
            if (!f.dragging && hypotf(f.x - f.startX, f.y - f.startY) > kSlop) f.dragging = true;
            const double dt = now - sLastMoveAt;
            if (dt > 0.0005) {
                const float ivx = (float)(mx / dt), ivy = (float)(my / dt);
                sVx = sVx * 0.6f + ivx * 0.4f; sVy = sVy * 0.6f + ivy * 0.4f;
            }
            sLastMoveAt = now;
        } else if (ev.type == SDL_FINGERUP && sDown && ev.tfinger.fingerId == sFid) {
            f.x = ev.tfinger.x * SCREEN_W; f.y = ev.tfinger.y * SCREEN_H;
            f.released = true;
            if (!f.dragging && now - sDownAt < 0.6) f.tap = true;
            // A finger that stopped before lifting does not fling.
            const bool still = now - sLastMoveAt > 0.08;
            f.vx = still ? 0.f : sVx; f.vy = still ? 0.f : sVy;
            sDown = false;
        }
    }
    f.touching = sDown;
    if (!sDown && !f.released) f.dragging = false;
    // A tap on a button hint is that button.
    if (f.tap) {
        auto in = [](const SDL_Rect& r, float x, float y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; };
        for (const Zone& z : sZones) {
            if (!in(z.r, f.startX, f.startY) || !in(z.r, f.x, f.y)) continue;
            const bool rightHalf = z.right && f.x >= z.r.x + z.r.w / 2.f;
            const u64 b = rightHalf ? z.right : z.left;
            f.rawDown |= b;
            f.rawHeld |= b;
            f.down = map(f.rawDown);
            f.held = map(f.rawHeld);
            if (b & HidNpadButton_Left) { f.stepX = -1; f.stepY = 0; }
            if (b & HidNpadButton_Right) { f.stepX = 1; f.stepY = 0; }
            f.tap = f.pressed = f.released = false;   // the button, not a touch on the page
            sFlashRect = z.r;
            sFlashAt = now;
            break;
        }
    }
    return f;
}

}  // namespace Input
