// ui_tour.cpp — TomoToolNX. See ui_tour.h.
#include "ui_tour.h"
#include "ui_draw.h"
#include "ui_input.h"
#include "ui_motion.h"

#include <SDL2/SDL_image.h>
#include <switch.h>
#include <sys/stat.h>
#include <strings.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace Tour {
namespace {

enum class Op { Wait, Press, Hold, Tap, Drag, Record, Stop, Quit };
struct Cmd { Op op = Op::Wait; u64 btn = 0; float a = 0, b = 0, c = 0, d = 0, t = 0; std::string name; };

std::vector<Cmd> sCmds;
size_t sPc = 0;
bool   sLoaded = false, sQuit = false;
double sStart = -1;          // when the running command began (the app's clock)
int    sFrames = 0;          // frames it has run
float  sLastX = 0, sLastY = 0;
int    sUpAt = -1;           // a drag: the frame the finger lifted
bool        sRec = false;
std::string sDir;
int         sShot = 0;
// The frame drawn into a texture (sFrame), copied into the left quarter of a
// texture four times as wide (sWide) to be read back. Ryujinx's read-back
// returns only a quarter of each row (it copies the row's width in bytes, not
// in pixels); from the wide texture that quarter is the whole frame, and on a
// real console the left quarter is the frame just the same.
SDL_Texture* sFrame = nullptr;
SDL_Texture* sWide = nullptr;
bool         sDrawing = false;
std::vector<uint8_t> sBuf;
constexpr double kStep = 1.0 / 30.0;
constexpr SDL_FingerID kFinger = 0x7A11;

u64 ButtonOf(const char* s) {
    static const struct { const char* n; u64 b; } k[] = {
        { "A", HidNpadButton_A },       { "B", HidNpadButton_B },         { "X", HidNpadButton_X },
        { "Y", HidNpadButton_Y },       { "L", HidNpadButton_L },         { "R", HidNpadButton_R },
        { "ZL", HidNpadButton_ZL },     { "ZR", HidNpadButton_ZR },       { "PLUS", HidNpadButton_Plus },
        { "MINUS", HidNpadButton_Minus }, { "UP", HidNpadButton_Up },     { "DOWN", HidNpadButton_Down },
        { "LEFT", HidNpadButton_Left }, { "RIGHT", HidNpadButton_Right },
    };
    for (const auto& e : k) if (strcasecmp(s, e.n) == 0) return e.b;
    return 0;
}

void Finger(Uint32 type, float x, float y, float dx = 0, float dy = 0) {
    SDL_Event ev{};
    ev.type = type;
    ev.tfinger.fingerId = kFinger;
    ev.tfinger.x = x / SCREEN_W;
    ev.tfinger.y = y / SCREEN_H;
    ev.tfinger.dx = dx / SCREEN_W;
    ev.tfinger.dy = dy / SCREEN_H;
    ev.tfinger.pressure = 1.f;
    SDL_PushEvent(&ev);
}

void MkdirP(const std::string& p) {
    for (size_t i = 1; i <= p.size(); i++)
        if (i == p.size() || p[i] == '/') mkdir(p.substr(0, i).c_str(), 0777);
}

}  // namespace

bool Load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (char* h = strchr(line, '#')) *h = 0;
        char w[32] = {}, s1[64] = {};
        float a = 0, b = 0, c = 0, d = 0, t = 0;
        if (sscanf(line, "%31s", w) != 1) continue;
        Cmd cmd;
        if (!strcasecmp(w, "wait") && sscanf(line, "%*s %f", &t) == 1) { cmd.op = Op::Wait; cmd.t = t; }
        else if (!strcasecmp(w, "press") && sscanf(line, "%*s %63s", s1) == 1 && ButtonOf(s1)) { cmd.op = Op::Press; cmd.btn = ButtonOf(s1); }
        else if (!strcasecmp(w, "hold") && sscanf(line, "%*s %63s %f", s1, &t) == 2 && ButtonOf(s1)) { cmd.op = Op::Hold; cmd.btn = ButtonOf(s1); cmd.t = t; }
        else if (!strcasecmp(w, "tap") && sscanf(line, "%*s %f %f", &a, &b) == 2) { cmd.op = Op::Tap; cmd.a = a; cmd.b = b; }
        else if (!strcasecmp(w, "drag") && sscanf(line, "%*s %f %f %f %f %f", &a, &b, &c, &d, &t) == 5) {
            cmd.op = Op::Drag; cmd.a = a; cmd.b = b; cmd.c = c; cmd.d = d; cmd.t = t;
        }
        else if (!strcasecmp(w, "record") && sscanf(line, "%*s %63s", s1) == 1) { cmd.op = Op::Record; cmd.name = s1; }
        else if (!strcasecmp(w, "stop")) cmd.op = Op::Stop;
        else if (!strcasecmp(w, "quit")) cmd.op = Op::Quit;
        else continue;
        sCmds.push_back(cmd);
    }
    fclose(f);
    sLoaded = !sCmds.empty();
    return sLoaded;
}

bool Active() { return sLoaded && sPc < sCmds.size(); }
bool QuitRequested() { return sQuit; }

void Step() {
    // While recording, held directions repeat on the app's clock too.
    Input::UseTime(sRec ? Motion::Now() : -1);
    while (Active()) {
        Cmd& c = sCmds[sPc];
        const double now = Motion::Now();
        if (sStart < 0) { sStart = now; sFrames = 0; sUpAt = -1; }
        const double el = now - sStart;
        const int fr = sFrames++;
        bool done = false, instant = false;
        switch (c.op) {
            case Op::Wait:
                done = el >= c.t;
                instant = done && fr == 0;
                break;
            case Op::Press:
                // Down for three frames (a quick, real press), then a frame up.
                if (fr == 0) Input::Inject(c.btn, c.btn);
                else if (fr < 3) Input::Inject(0, c.btn);
                done = fr >= 4;
                break;
            case Op::Hold:
                if (fr == 0) Input::Inject(c.btn, c.btn);
                else if (el < c.t) Input::Inject(0, c.btn);
                done = fr >= 2 && el >= c.t + 0.04;
                break;
            case Op::Tap:
                if (fr == 0) Finger(SDL_FINGERDOWN, c.a, c.b);
                else if (fr == 2) Finger(SDL_FINGERUP, c.a, c.b);
                done = fr >= 4;
                break;
            case Op::Drag: {
                if (fr == 0) { Finger(SDL_FINGERDOWN, c.a, c.b); sLastX = c.a; sLastY = c.b; break; }
                if (sUpAt >= 0) { done = fr >= sUpAt + 2; break; }
                const float k = c.t > 0 ? std::min(1.f, (float)(el / c.t)) : 1.f;
                const float x = c.a + (c.c - c.a) * k, y = c.b + (c.d - c.b) * k;
                Finger(SDL_FINGERMOTION, x, y, x - sLastX, y - sLastY);
                sLastX = x; sLastY = y;
                if (k >= 1.f) { Finger(SDL_FINGERUP, x, y); sUpAt = fr; }
                break;
            }
            case Op::Record:
                sDir = std::string("/switch/TomoToolNX/rec/") + c.name;
                MkdirP(sDir);
                sShot = 0;
                sRec = true;
                Motion::SetFixedStep(kStep);
                done = instant = true;
                break;
            case Op::Stop:
                sRec = false;
                Motion::SetFixedStep(0);
                done = instant = true;
                break;
            case Op::Quit:
                sRec = false;
                Motion::SetFixedStep(0);
                sQuit = true;
                done = instant = true;
                break;
        }
        if (!done) return;
        sPc++;
        sStart = -1;
        if (!instant) return;   // the next command starts with the next frame
    }
}

void BeginRender(SDL_Renderer* ren) {
    sDrawing = false;
    if (!sRec || !ren) return;
    if (!sFrame) sFrame = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, SCREEN_W, SCREEN_H);
    if (!sFrame || SDL_SetRenderTarget(ren, sFrame) != 0) return;
    sDrawing = true;
}

void Capture(SDL_Renderer* ren) {
    if (!sDrawing || !ren) return;
    sDrawing = false;
    const int w = SCREEN_W, h = SCREEN_H, ww = SCREEN_W * 4;
    if (!sWide) sWide = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, ww, h);
    if (sWide && SDL_SetRenderTarget(ren, sWide) == 0) {
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_Rect dst{ 0, 0, w, h };
        SDL_SetTextureBlendMode(sFrame, SDL_BLENDMODE_NONE);
        SDL_RenderCopy(ren, sFrame, nullptr, &dst);
        sBuf.resize((size_t)ww * h * 4);
        SDL_Rect all{ 0, 0, ww, h };
        if (SDL_RenderReadPixels(ren, &all, SDL_PIXELFORMAT_ABGR8888, sBuf.data(), ww * 4) == 0) {
            if (SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ABGR8888)) {
                for (int y = 0; y < h; y++)
                    memcpy((uint8_t*)s->pixels + (size_t)y * s->pitch, sBuf.data() + (size_t)y * ww * 4, (size_t)w * 4);
                char path[300];
                snprintf(path, sizeof path, "%s/%05d.png", sDir.c_str(), sShot++);
                IMG_SavePNG(s, path);
                SDL_FreeSurface(s);
            }
        }
    }
    // And the frame on the screen as usual.
    SDL_SetRenderTarget(ren, nullptr);
    SDL_SetTextureBlendMode(sFrame, SDL_BLENDMODE_NONE);
    SDL_RenderCopy(ren, sFrame, nullptr, nullptr);
}

}  // namespace Tour
