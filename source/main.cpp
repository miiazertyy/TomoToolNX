// main.cpp
// Tomodachi Life UGC Editor – Switch NRO
// 1. Show user picker → mount save → start HTTP server
// 2. Display IP:port on screen, wait for + to quit

#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include "save_mount.h"
#include "http_server.h"
#include "texture_processor.h"

#include <string>
#include <vector>
#include <cstring>
#include <sys/stat.h>


// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

static const int SCREEN_W = 1280;
static const int SCREEN_H = 720;
static const int HTTP_PORT = 8080;

static const SDL_Color COL_BG       = {14,  14,  22,  255};
static const SDL_Color COL_PANEL    = {28,  28,  44,  255};
static const SDL_Color COL_SEL      = {55,  80,  180, 255};
static const SDL_Color COL_TEXT     = {220, 220, 235, 255};
static const SDL_Color COL_DIM      = {120, 120, 145, 255};
static const SDL_Color COL_GREEN    = {80,  200, 110, 255};
static const SDL_Color COL_RED      = {200, 70,  70,  255};
static const SDL_Color COL_GOLD     = {255, 210, 80,  255};
static const SDL_Color COL_ACCENT   = {100, 140, 255, 255};

static PadState      gPad;
static SDL_Renderer* gRen = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
// Bitmap font (same 5×7 as before)
// ─────────────────────────────────────────────────────────────────────────────

static const uint8_t FONT5x7[][7] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},{0x04,0x04,0x04,0x04,0x00,0x04,0x00},
    {0x0A,0x0A,0x00,0x00,0x00,0x00,0x00},{0x0A,0x1F,0x0A,0x1F,0x0A,0x00,0x00},
    {0x0E,0x15,0x07,0x1C,0x15,0x0E,0x00},{0x19,0x1A,0x04,0x0B,0x13,0x00,0x00},
    {0x06,0x09,0x06,0x15,0x12,0x0D,0x00},{0x06,0x04,0x00,0x00,0x00,0x00,0x00},
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02},{0x08,0x04,0x02,0x02,0x02,0x04,0x08},
    {0x00,0x0A,0x04,0x1F,0x04,0x0A,0x00},{0x00,0x04,0x04,0x1F,0x04,0x04,0x00},
    {0x00,0x00,0x00,0x00,0x06,0x04,0x08},{0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x06,0x00},{0x01,0x02,0x02,0x04,0x08,0x08,0x10},
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},{0x04,0x0C,0x04,0x04,0x04,0x0E,0x00},
    {0x0E,0x11,0x01,0x06,0x08,0x1F,0x00},{0x0E,0x11,0x01,0x06,0x01,0x11,0x0E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x00},{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},{0x1F,0x01,0x02,0x04,0x08,0x08,0x00},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    {0x00,0x06,0x00,0x00,0x06,0x00,0x00},{0x00,0x06,0x00,0x00,0x06,0x04,0x08},
    {0x02,0x04,0x08,0x10,0x08,0x04,0x02},{0x00,0x1F,0x00,0x1F,0x00,0x00,0x00},
    {0x08,0x04,0x02,0x01,0x02,0x04,0x08},{0x0E,0x11,0x01,0x06,0x04,0x00,0x04},
    {0x0E,0x11,0x17,0x15,0x17,0x10,0x0E},{0x04,0x0A,0x11,0x1F,0x11,0x11,0x00},
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},{0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},{0x11,0x1B,0x15,0x11,0x11,0x11,0x11},
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11},{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},{0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04},{0x11,0x11,0x11,0x15,0x15,0x1B,0x11},
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},{0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},{0x0E,0x08,0x08,0x08,0x08,0x08,0x0E},
    {0x10,0x08,0x08,0x04,0x02,0x02,0x01},{0x0E,0x02,0x02,0x02,0x02,0x02,0x0E},
    {0x04,0x0A,0x11,0x00,0x00,0x00,0x00},{0x00,0x00,0x00,0x00,0x00,0x00,0x1F},
    {0x08,0x04,0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F},
    {0x10,0x10,0x1E,0x11,0x11,0x11,0x1E},{0x00,0x00,0x0E,0x10,0x10,0x10,0x0E},
    {0x01,0x01,0x0F,0x11,0x11,0x11,0x0F},{0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E},
    {0x06,0x09,0x08,0x1C,0x08,0x08,0x08},{0x00,0x00,0x0F,0x11,0x0F,0x01,0x0E},
    {0x10,0x10,0x16,0x19,0x11,0x11,0x11},{0x04,0x00,0x0C,0x04,0x04,0x04,0x0E},
    {0x02,0x00,0x06,0x02,0x02,0x12,0x0C},{0x10,0x10,0x12,0x14,0x18,0x14,0x12},
    {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E},{0x00,0x00,0x1A,0x15,0x15,0x11,0x11},
    {0x00,0x00,0x16,0x19,0x11,0x11,0x11},{0x00,0x00,0x0E,0x11,0x11,0x11,0x0E},
    {0x00,0x00,0x1E,0x11,0x1E,0x10,0x10},{0x00,0x00,0x0F,0x11,0x0F,0x01,0x01},
    {0x00,0x00,0x16,0x19,0x10,0x10,0x10},{0x00,0x00,0x0E,0x10,0x0E,0x01,0x0E},
    {0x08,0x08,0x1C,0x08,0x08,0x09,0x06},{0x00,0x00,0x11,0x11,0x11,0x13,0x0D},
    {0x00,0x00,0x11,0x11,0x11,0x0A,0x04},{0x00,0x00,0x11,0x11,0x15,0x15,0x0A},
    {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11},{0x00,0x00,0x11,0x11,0x0F,0x01,0x0E},
    {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F},{0x06,0x08,0x08,0x10,0x08,0x08,0x06},
    {0x04,0x04,0x04,0x00,0x04,0x04,0x04},{0x0C,0x02,0x02,0x01,0x02,0x02,0x0C},
    {0x08,0x15,0x02,0x00,0x00,0x00,0x00},
};
static const int FONT_N = (int)(sizeof(FONT5x7)/sizeof(FONT5x7[0]));

static void DrawText(const std::string& text, int x, int y, SDL_Color col, int scale=2) {
    int cx = x;
    SDL_SetRenderDrawColor(gRen, col.r, col.g, col.b, col.a);
    for (char ch : text) {
        if (ch=='\n'){y+=7*scale+scale;cx=x;continue;}
        int idx=(unsigned char)ch-32;
        if(idx<0||idx>=FONT_N){cx+=5*scale+scale;continue;}
        for(int row=0;row<7;row++){
            uint8_t bits=FONT5x7[idx][row];
            for(int px=0;px<5;px++){
                if(bits&(1<<(4-px))){
                    SDL_Rect r{cx+px*scale,y+row*scale,scale,scale};
                    SDL_RenderFillRect(gRen,&r);
                }
            }
        }
        cx+=5*scale+scale;
    }
}

static void DrawTextC(const std::string& t, int cx, int cy, SDL_Color col, int scale=2) {
    int tw=(int)t.size()*(5*scale+scale);
    int th=7*scale;
    DrawText(t,cx-tw/2,cy-th/2,col,scale);
}

static void FillRect(int x,int y,int w,int h,SDL_Color c){
    SDL_SetRenderDrawColor(gRen,c.r,c.g,c.b,c.a);
    SDL_Rect r{x,y,w,h}; SDL_RenderFillRect(gRen,&r);
}
static void DrawRect(int x,int y,int w,int h,SDL_Color c){
    SDL_SetRenderDrawColor(gRen,c.r,c.g,c.b,c.a);
    SDL_Rect r{x,y,w,h}; SDL_RenderDrawRect(gRen,&r);
}

// ─────────────────────────────────────────────────────────────────────────────
// Screens
// ─────────────────────────────────────────────────────────────────────────────

enum class Screen { UserPick, Mounting, Running, Error };

static Screen            gScreen  = Screen::UserPick;
static std::string       gError;
static std::string       gIP;
static std::vector<SaveMount::UserInfo> gUsers;
static int               gUserSel = 0;

static void DrawUserPick() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);

    DrawTextC("Tomodachi Life UGC Editor", SCREEN_W/2, 100, COL_TEXT, 3);
    DrawTextC("Select your user account", SCREEN_W/2, 155, COL_DIM, 2);

    int listTop = 220;
    int itemH   = 60;
    for (int i=0;i<(int)gUsers.size();i++) {
        bool sel = (i==gUserSel);
        FillRect(SCREEN_W/2-200, listTop+i*itemH, 400, itemH-4,
                 sel ? COL_SEL : COL_PANEL);
        if (sel) DrawRect(SCREEN_W/2-200, listTop+i*itemH, 400, itemH-4, COL_ACCENT);
        DrawTextC(gUsers[i].nickname, SCREEN_W/2, listTop+i*itemH+(itemH-4)/2, COL_TEXT, 2);
    }

    DrawTextC("Up/Down = Navigate    A = Select    + = Quit",
              SCREEN_W/2, SCREEN_H-30, COL_DIM, 2);
    SDL_RenderPresent(gRen);
}

static void DrawMounting() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);
    DrawTextC("Mounting save data...", SCREEN_W/2, SCREEN_H/2, COL_TEXT, 3);
    SDL_RenderPresent(gRen);
}

// ─────────────────────────────────────────────────────────────────────────────
// On-screen activity log (ring buffer, shown in Running screen)
// ─────────────────────────────────────────────────────────────────────────────

struct LogLine { std::string text; SDL_Color col; };
static const int LOG_MAX = 8;
static std::vector<LogLine> gLog;

static void Log(const std::string& msg, SDL_Color col) {
    gLog.push_back({msg, col});
    if ((int)gLog.size() > LOG_MAX) gLog.erase(gLog.begin());
}
static void LogINF(const std::string& msg) { Log(msg, COL_TEXT);  }
static void LogOK (const std::string& msg) { Log(msg, COL_GREEN); }
static void LogERR(const std::string& msg) { Log(msg, COL_RED);   }

static void DrawRunning() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);

    DrawTextC("Tomodachi Life UGC Editor", SCREEN_W/2, 90, COL_TEXT, 3);

    // Big IP display
    FillRect(SCREEN_W/2-340, 160, 680, 90, COL_PANEL);
    DrawRect(SCREEN_W/2-340, 160, 680, 90, COL_ACCENT);
    DrawTextC("Open in your browser:", SCREEN_W/2, 185, COL_DIM, 2);
    DrawTextC("http://" + gIP + ":" + std::to_string(HTTP_PORT),
              SCREEN_W/2, 218, COL_GOLD, 3);

    // Status
    DrawTextC("HTTP server running  -  save mounted", SCREEN_W/2, 300, COL_GREEN, 2);
    DrawTextC("Textures path: save:/Ugc", SCREEN_W/2, 335, COL_DIM, 2);

    // Instructions
    DrawTextC("Connect your PC/phone to the same WiFi network",
              SCREEN_W/2, 420, COL_TEXT, 2);
    DrawTextC("Import PNG from your PC  |  Export texture as PNG  |  All changes auto-saved",
              SCREEN_W/2, 455, COL_DIM, 2);

    // Activity log (last 8 lines from HTTP server)
    {
        int logY = 490;
        for (auto& line : gLog) {
            DrawTextC(line.text, SCREEN_W/2, logY, line.col, 1);
            logY += 16;
        }
    }

    DrawTextC("(+) Quit", SCREEN_W/2, SCREEN_H-30, COL_DIM, 2);
    SDL_RenderPresent(gRen);
}

static void DrawError() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);
    DrawTextC("Error", SCREEN_W/2, SCREEN_H/2-50, COL_RED, 3);
    DrawTextC(gError, SCREEN_W/2, SCREEN_H/2+10, COL_TEXT, 2);
    DrawTextC("(+) Quit", SCREEN_W/2, SCREEN_H-30, COL_DIM, 2);
    SDL_RenderPresent(gRen);
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main(int, char**) {
    // Init network (must be before any nifm calls, including GetLocalIP)
    nifmInitialize(NifmServiceType_User);

    // Init BSD socket service (required on Switch before any socket() call)
    socketInitialize(socketGetDefaultInitConfig());

    // Init SDL
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK);
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);

    SDL_Window* win = SDL_CreateWindow("Tomodachi UGC Editor",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, SCREEN_W, SCREEN_H, 0);
    gRen = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_SetRenderDrawBlendMode(gRen, SDL_BLENDMODE_BLEND);

    // Init pad
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&gPad);

    // Ensure export dir exists
    mkdir("/switch/tomodachi-ugc", 0777);

    // Load users
    gUsers = SaveMount::GetUsers();
    if (gUsers.empty()) {
        gError  = "No user accounts found on this Switch.";
        gScreen = Screen::Error;
    }

    // Main loop
    while (appletMainLoop()) {
        padUpdate(&gPad);
        u64 kDown = padGetButtonsDown(&gPad);

        if (kDown & HidNpadButton_Plus) break;

        switch (gScreen) {

        case Screen::UserPick:
            if (kDown & HidNpadButton_Up) {
                if (gUserSel > 0) gUserSel--;
            }
            if (kDown & HidNpadButton_Down) {
                if (gUserSel+1 < (int)gUsers.size()) gUserSel++;
            }
            if (kDown & HidNpadButton_A) {
                gScreen = Screen::Mounting;
                DrawMounting();

                std::string err = SaveMount::Mount(gUsers[gUserSel].uid);
                if (!err.empty()) {
                    gError  = err;
                    gScreen = Screen::Error;
                } else {
                    gIP = SaveMount::GetLocalIP();
                    if (gIP.empty()) gIP = "?.?.?.?  (check WiFi)";
                    HttpServer::Start(HTTP_PORT, SAVE_UGC_PATH);
                    gScreen = Screen::Running;
                }
            }
            DrawUserPick();
            break;

        case Screen::Mounting:
            DrawMounting();
            break;

        case Screen::Running:
            // Pull activity from the HTTP server thread into the on-screen log
            {
                std::vector<HttpServer::LogEntry> httpLogs;
                HttpServer::DrainLog(httpLogs);
                for (auto& hl : httpLogs) {
                    SDL_Color col = hl.isError
                        ? SDL_Color{220, 70, 70, 255}
                        : SDL_Color{100, 180, 255, 255};
                    Log(hl.text, col);
                }
            }
            // Execute pending import on the main thread (IMG_Load is not thread-safe)
            if (HttpServer::HasPendingImport()) {
                auto job = HttpServer::TakePendingImport();
                std::string importErr = TextureProcessor::ImportPng(job.opts);
                remove(job.tmpPath.c_str());
                HttpServer::FinishImport(importErr);
                if (!importErr.empty()) LogERR("Import: " + importErr);
                else LogOK("Import OK");
            }
            // Check if HTTP server did an import and we need to commit
            if (HttpServer::HasPendingCommit()) {
                LogINF("Committing save...");
                HttpServer::ClearPendingCommit();
                std::string cerr = SaveMount::Commit();
                if (cerr.empty()) LogOK("Save committed");
                else              LogERR("Commit: " + cerr);
            }
            DrawRunning();
            break;

        case Screen::Error:
            DrawError();
            break;
        }
    }

    // Cleanup
    HttpServer::Stop();
    SaveMount::Unmount();
    nifmExit();
    socketExit();
    SDL_DestroyRenderer(gRen);
    SDL_DestroyWindow(win);
    IMG_Quit();
    SDL_Quit();
    return 0;
}
