#pragma once
// ui_input.h — TomoToolNX
// One read of the controller and the touch screen per frame. Directions repeat
// the way LLM-Selfbot's gamepad code does (first repeat after 380 ms, then
// every 130 ms), measured in time, not frames.

#include <SDL2/SDL.h>
#include <switch.h>
#include <cstdint>
#include <vector>
#include <string>

namespace Input {

enum Btn : uint32_t {
    A = 1u << 0, B = 1u << 1, X = 1u << 2, Y = 1u << 3, L = 1u << 4, R = 1u << 5, ZL = 1u << 6, ZR = 1u << 7,
    Plus = 1u << 8, Minus = 1u << 9, Up = 1u << 10, Down = 1u << 11, Left = 1u << 12, Right = 1u << 13,
};

struct Frame {
    u64 rawDown = 0, rawHeld = 0;   // libnx bits, for the screens that still read them
    uint32_t down = 0, held = 0;    // Btn bits; directions merge the D-pad and the left stick
    int stepX = 0, stepY = 0;       // a move this frame (press or repeat): -1, 0 or +1
    bool repeat = false;            // the step came from holding, not a fresh press
    float rx = 0, ry = 0;           // right stick, -1..1, +y = down the screen
    float lx = 0, ly = 0;           // left stick, the same way
    // One finger
    bool  touching = false, pressed = false, released = false, tap = false, dragging = false;
    float x = 0, y = 0, startX = 0, startY = 0, dx = 0, dy = 0, vx = 0, vy = 0;
    std::vector<SDL_Event> events;  // every SDL event this frame, replayed by the old screens
};

void Init(PadState* pad);
const Frame& Poll();
const Frame& Last();

// For the tour player (ui_tour.h): buttons (HidNpadButton bits) added to the
// next Poll as pressed this frame / held, and the clock the repeats use
// (< 0: real time).
void Inject(u64 down, u64 held);
// The button hints at the bottom of the screen can be tapped: each says where
// it is as it is drawn (key as shown: "A", "B", "+", "-", "L R", "ZL ZR", the
// left/right arrows...), and a tap that starts and ends on one presses its
// button in the next frame instead of touching the page. A pair ("L R")
// presses its left button on its left half and its right one on the other.
void HintZone(const std::string& key, SDL_Rect r);
// This frame's tap, when it is inside r: taken (the page under it never sees
// it). For things drawn over everything (the logo out for a walk).
bool TakeTap(SDL_Rect r);
// A hint tapped a moment ago: how far its flash has run (0..1), or false.
bool HintFlash(SDL_Rect r, float& t);
void UseTime(double seconds);

// For the next Poll only: the left stick does not count as the D-pad (a page
// that reads lx/ly itself, like the map's free cursor, asks for this every
// frame it wants it).
void StickAsAnalog();

}  // namespace Input
