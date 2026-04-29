// main.cpp — TomoToolNX
// After user select: choose WebUI mode or On-Switch mode.
// WebUI: HTTP server, use browser on PC/phone.
// On-Switch: browse textures and import PNGs directly on console.

#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include "save_mount.h"
#include "http_server.h"
#include "texture_processor.h"
#include "ugc_scanner.h"
#include "backup.h"

#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <algorithm>

// ─── Constants ────────────────────────────────────────────────────────────────

static const int SCREEN_W = 1280;
static const int SCREEN_H = 720;
static const int HTTP_PORT = 8080;

static const SDL_Color COL_BG     = {10,  10,  10,  255};
static const SDL_Color COL_PANEL  = {22,  22,  22,  255};
static const SDL_Color COL_PANEL2 = {32,  32,  32,  255};
static const SDL_Color COL_SEL    = {50,  50,  50,  255};
static const SDL_Color COL_BORDER = {55,  55,  55,  255};
static const SDL_Color COL_TEXT   = {220, 220, 220, 255};
static const SDL_Color COL_DIM    = {100, 100, 100, 255};
static const SDL_Color COL_GOLD   = {200, 170, 100, 255};
static const SDL_Color COL_GREEN  = {100, 190, 120, 255};
static const SDL_Color COL_RED    = {190, 90,  90,  255};
static const SDL_Color COL_ACCENT = {120, 180, 200, 255};

static PadState      gPad;
static SDL_Renderer* gRen = nullptr;

// ─── Bitmap font ──────────────────────────────────────────────────────────────

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
    int cx=x;
    SDL_SetRenderDrawColor(gRen,col.r,col.g,col.b,col.a);
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
static void DrawTextC(const std::string& t,int cx,int cy,SDL_Color col,int scale=2){
    int tw=(int)t.size()*(5*scale+scale);
    DrawText(t,cx-tw/2,cy-7*scale/2,col,scale);
}
static void FillRect(int x,int y,int w,int h,SDL_Color c){
    SDL_SetRenderDrawColor(gRen,c.r,c.g,c.b,c.a);
    SDL_Rect r{x,y,w,h};SDL_RenderFillRect(gRen,&r);
}
static void DrawRect(int x,int y,int w,int h,SDL_Color c){
    SDL_SetRenderDrawColor(gRen,c.r,c.g,c.b,c.a);
    SDL_Rect r{x,y,w,h};SDL_RenderDrawRect(gRen,&r);
}

// ─── Screens ──────────────────────────────────────────────────────────────────

enum class Screen { UserPick, BackupPrompt, BackingUp, ModePick, Mounting, WebUI, OnSwitch, Error };

static Screen      gScreen  = Screen::UserPick;
static std::string gError;
static std::string gIP;
static std::vector<SaveMount::UserInfo> gUsers;
static int         gUserSel = 0;

// ─── Log (WebUI screen) ───────────────────────────────────────────────────────

struct LogLine { std::string text; SDL_Color col; };
static const int LOG_MAX = 10;
static std::vector<LogLine> gLog;
static void Log(const std::string& msg, SDL_Color col){
    gLog.push_back({msg,col});
    if((int)gLog.size()>LOG_MAX) gLog.erase(gLog.begin());
}
static void LogINF(const std::string& m){Log(m,COL_TEXT);}
static void LogOK (const std::string& m){Log(m,COL_GREEN);}
static void LogERR(const std::string& m){Log(m,COL_RED);}

// ─── On-Switch editor state ───────────────────────────────────────────────────

static std::vector<UgcTextureEntry> gEntries;
static int          gEntrySel    = 0;
static int          gEntryScroll = 0;
static SDL_Texture* gPreviewTex  = nullptr;
static std::string  gPreviewStem;
static std::string  gOnSwitchMsg;
static SDL_Color    gOnSwitchMsgCol = COL_TEXT;

// PNG files on SD available for import
static std::vector<std::string> gPngFiles;
static int gPngSel    = 0;
static int gPngScroll = 0;
static bool gShowPngPicker = false;

static void FreePreview() {
    if (gPreviewTex) { SDL_DestroyTexture(gPreviewTex); gPreviewTex=nullptr; }
    gPreviewStem = "";
}

static void LoadPreview(const UgcTextureEntry& e) {
    FreePreview();
    RgbaImage img;
    std::string err = TextureProcessor::DecodeFile(e.ugctexPath, img);
    if (!err.empty()) { gOnSwitchMsg=err; gOnSwitchMsgCol=COL_RED; return; }
    // Upload to GPU texture
    SDL_Surface* surf = SDL_CreateRGBSurfaceFrom(
        img.pixels.data(), img.width, img.height, 32, img.width*4,
        0x000000FF,0x0000FF00,0x00FF0000,0xFF000000);
    if (!surf) return;
    gPreviewTex  = SDL_CreateTextureFromSurface(gRen, surf);
    SDL_FreeSurface(surf);
    gPreviewStem = e.stem;
    gOnSwitchMsg = "";
}

static void ScanPngs() {
    gPngFiles.clear();
    // Scan /switch/ for PNG files (one level deep)
    const char* roots[] = {"/switch/TomoToolNX", "/switch"};
    for (auto root : roots) {
        DIR* d = opendir(root);
        if (!d) continue;
        struct dirent* de;
        while ((de=readdir(d))!=nullptr) {
            std::string name = de->d_name;
            if (name.size()<4) continue;
            std::string ext = name.substr(name.size()-4);
            for (auto& c:ext) c=tolower(c);
            if (ext==".png") {
                gPngFiles.push_back(std::string(root)+"/"+name);
            }
        }
        closedir(d);
    }
    std::sort(gPngFiles.begin(), gPngFiles.end());
    gPngSel=0; gPngScroll=0;
}

static void DoOnSwitchImport(const std::string& pngPath) {
    if (gEntries.empty()) return;
    const auto& e = gEntries[gEntrySel];

    TextureProcessor::ImportOptions opts;
    opts.pngPath            = pngPath;
    opts.destStem           = e.directory()+"/"+e.stem;
    opts.writeCanvas        = e.hasCanvas();
    opts.writeThumb         = e.hasThumb();
    opts.noSrgb             = false;
    opts.originalUgctexPath = e.ugctexPath;

    std::string err = TextureProcessor::ImportPng(opts);
    if (!err.empty()) {
        gOnSwitchMsg=err; gOnSwitchMsgCol=COL_RED; return;
    }
    std::string cerr = SaveMount::Commit();
    if (!cerr.empty()) {
        gOnSwitchMsg="Commit: "+cerr; gOnSwitchMsgCol=COL_RED; return;
    }
    // Reload entry list and preview
    gEntries = UgcScanner::Scan(SAVE_UGC_PATH);
    if (gEntrySel >= (int)gEntries.size()) gEntrySel=(int)gEntries.size()-1;
    if (!gEntries.empty()) LoadPreview(gEntries[gEntrySel]);
    gOnSwitchMsg="Imported successfully"; gOnSwitchMsgCol=COL_GREEN;
}

// ─── Draw helpers ─────────────────────────────────────────────────────────────

static void DrawHeader(const std::string& title) {
    FillRect(0,0,SCREEN_W,44,COL_PANEL);
    DrawRect(0,0,SCREEN_W,44,COL_BORDER);
    DrawText("TomoToolNX",16,14,COL_GOLD,2);
    if (!title.empty()) {
        DrawText(" / "+title,16+11*12,14,COL_DIM,2);
    }
}

static void DrawFooter(const std::string& hints) {
    FillRect(0,SCREEN_H-38,SCREEN_W,38,COL_PANEL);
    DrawRect(0,SCREEN_H-38,SCREEN_W,1,COL_BORDER);
    DrawTextC(hints, SCREEN_W/2, SCREEN_H-19, COL_DIM, 1);
}

// ─── Screen draws ─────────────────────────────────────────────────────────────

static void DrawUserPick() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);
    DrawHeader("");
    DrawTextC("select user", SCREEN_W/2, 90, COL_DIM, 1);

    int listTop=120, itemH=52;
    for (int i=0;i<(int)gUsers.size();i++){
        bool sel=(i==gUserSel);
        FillRect(SCREEN_W/2-220,listTop+i*itemH,440,itemH-2, sel?COL_SEL:COL_PANEL);
        if(sel) DrawRect(SCREEN_W/2-220,listTop+i*itemH,440,itemH-2,COL_ACCENT);
        DrawTextC(gUsers[i].nickname, SCREEN_W/2, listTop+i*itemH+(itemH-2)/2, sel?COL_TEXT:COL_DIM, 2);
    }
    DrawFooter("Up/Down  navigate    A  select    +  quit");
    SDL_RenderPresent(gRen);
}

static void DrawBackupPrompt() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);
    DrawHeader("");
    DrawTextC("previous backup found", SCREEN_W/2, 130, COL_DIM, 2);
    DrawTextC("what would you like to do?", SCREEN_W/2, 170, COL_TEXT, 2);

    int cy=220, cw=300, ch=160, gap=40;
    int x1=SCREEN_W/2-cw-gap/2, x2=SCREEN_W/2+gap/2;

    // A — delete old, make new
    FillRect(x1,cy,cw,ch,COL_PANEL); DrawRect(x1,cy,cw,ch,COL_RED);
    DrawTextC("[A]",               x1+cw/2, cy+30,  COL_RED,  3);
    DrawTextC("delete old backup", x1+cw/2, cy+80,  COL_TEXT, 2);
    DrawTextC("then make new one", x1+cw/2, cy+110, COL_DIM,  2);

    // B — skip, keep old
    FillRect(x2,cy,cw,ch,COL_PANEL); DrawRect(x2,cy,cw,ch,COL_ACCENT);
    DrawTextC("[B]",             x2+cw/2, cy+30,  COL_ACCENT, 3);
    DrawTextC("skip backup",     x2+cw/2, cy+80,  COL_TEXT,   2);
    DrawTextC("keep old as-is",  x2+cw/2, cy+110, COL_DIM,    2);

    // X — keep old, also make new
    int x3=SCREEN_W/2-cw/2, cy3=cy+ch+30;
    FillRect(x3,cy3,cw,ch,COL_PANEL); DrawRect(x3,cy3,cw,ch,COL_GOLD);
    DrawTextC("[X]",                    x3+cw/2, cy3+30,  COL_GOLD, 3);
    DrawTextC("keep old + make new",    x3+cw/2, cy3+80,  COL_TEXT, 2);
    DrawTextC("saves both backups",     x3+cw/2, cy3+110, COL_DIM,  2);

    DrawFooter("A  delete old + new    B  skip    X  keep old + new alongside    +  quit");
    SDL_RenderPresent(gRen);
}

static void DrawBackingUp() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);
    DrawHeader("");
    DrawTextC("backing up save data...", SCREEN_W/2, SCREEN_H/2-40, COL_DIM, 2);

    // Progress bar
    float prog = BackupService::BackupProgress();
    int bw=600, bh=16, bx=SCREEN_W/2-bw/2, by=SCREEN_H/2;
    FillRect(bx,by,bw,bh,COL_PANEL);
    DrawRect(bx,by,bw,bh,COL_BORDER);
    FillRect(bx,by,(int)(bw*prog),bh,COL_ACCENT);

    char pct[16]; snprintf(pct,sizeof(pct),"%d%%",(int)(prog*100));
    DrawTextC(pct, SCREEN_W/2, by+bh+20, COL_DIM, 1);
    DrawTextC("do not turn off the console", SCREEN_W/2, SCREEN_H/2+80, COL_DIM, 1);
    SDL_RenderPresent(gRen);
}

static void DrawModePick() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);
    DrawHeader("");
    DrawTextC("choose mode", SCREEN_W/2, 110, COL_DIM, 1);

    // Left card — WebUI
    int cx1=SCREEN_W/2-240, cx2=SCREEN_W/2+40;
    int cy=240, cw=200, ch=180;
    FillRect(cx1,cy,cw,ch,COL_PANEL);
    DrawRect(cx1,cy,cw,ch,COL_ACCENT);
    DrawTextC("WebUI",    cx1+cw/2, cy+50,  COL_TEXT,  2);
    DrawTextC("browser on", cx1+cw/2, cy+90,  COL_DIM, 1);
    DrawTextC("PC or phone", cx1+cw/2, cy+106, COL_DIM, 1);
    DrawTextC("[L]", cx1+cw/2, cy+140, COL_ACCENT, 2);

    // Right card — On-Switch
    FillRect(cx2,cy,cw,ch,COL_PANEL);
    DrawRect(cx2,cy,cw,ch,COL_GOLD);
    DrawTextC("On-Switch", cx2+cw/2, cy+50,  COL_TEXT, 2);
    DrawTextC("controller",  cx2+cw/2, cy+90,  COL_DIM, 1);
    DrawTextC("no network",  cx2+cw/2, cy+106, COL_DIM, 1);
    DrawTextC("[R]", cx2+cw/2, cy+140, COL_GOLD, 2);

    DrawFooter("L  WebUI mode    R  On-Switch mode    B  back    +  quit");
    SDL_RenderPresent(gRen);
}

static void DrawMounting() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);
    DrawTextC("mounting save...", SCREEN_W/2, SCREEN_H/2, COL_DIM, 2);
    SDL_RenderPresent(gRen);
}

static void DrawWebUI() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);
    DrawHeader("webui");

    // URL box
    FillRect(SCREEN_W/2-320,80,640,70,COL_PANEL);
    DrawRect(SCREEN_W/2-320,80,640,70,COL_BORDER);
    DrawTextC("open in browser", SCREEN_W/2, 100, COL_DIM, 1);
    DrawTextC("http://"+gIP+":"+std::to_string(HTTP_PORT), SCREEN_W/2, 124, COL_GOLD, 2);

    // Log
    int logY=170;
    for (auto& line : gLog) {
        DrawText(line.text, 40, logY, line.col, 1);
        logY+=14;
    }
    DrawFooter("B  back to mode select    +  quit");
    SDL_RenderPresent(gRen);
}

static const int LIST_X=40, LIST_W=420, LIST_Y=50, LIST_H=SCREEN_H-120, ITEM_H=36;
static const int PREVIEW_X=490, PREVIEW_Y=50, PREVIEW_W=750, PREVIEW_H=480;
static const int VISIBLE = LIST_H/ITEM_H;

static void DrawOnSwitch() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);
    DrawHeader("on-switch");

    if (gShowPngPicker) {
        // PNG picker overlay
        FillRect(100,60,SCREEN_W-200,SCREEN_H-120,COL_PANEL);
        DrawRect(100,60,SCREEN_W-200,SCREEN_H-120,COL_BORDER);
        DrawTextC("select PNG to import", SCREEN_W/2, 90, COL_DIM, 1);

        int py=120, ph=32;
        int pvis=(SCREEN_H-200)/ph;
        for (int i=0;i<pvis;i++){
            int idx=gPngScroll+i;
            if (idx>=(int)gPngFiles.size()) break;
            bool sel=(idx==gPngSel);
            FillRect(120,py+i*ph,SCREEN_W-240,ph-2, sel?COL_SEL:COL_PANEL2);
            if(sel) DrawRect(120,py+i*ph,SCREEN_W-240,ph-2,COL_ACCENT);
            // Show just filename
            std::string name=gPngFiles[idx];
            size_t sl=name.rfind('/');
            if(sl!=std::string::npos) name=name.substr(sl+1);
            DrawText(name,134,py+i*ph+8,sel?COL_TEXT:COL_DIM,1);
        }
        DrawFooter("Up/Down  navigate    A  import    B  cancel");
        SDL_RenderPresent(gRen);
        return;
    }

    // Texture list (left panel)
    FillRect(LIST_X-4,LIST_Y-4,LIST_W+8,LIST_H+8,COL_PANEL);
    DrawRect(LIST_X-4,LIST_Y-4,LIST_W+8,LIST_H+8,COL_BORDER);
    for (int i=0;i<VISIBLE;i++){
        int idx=gEntryScroll+i;
        if (idx>=(int)gEntries.size()) break;
        bool sel=(idx==gEntrySel);
        FillRect(LIST_X,LIST_Y+i*ITEM_H,LIST_W,ITEM_H-1, sel?COL_SEL:COL_BG);
        if(sel) DrawRect(LIST_X,LIST_Y+i*ITEM_H,LIST_W,ITEM_H-1,COL_ACCENT);
        std::string label=gEntries[idx].stem;
        DrawText(label, LIST_X+8, LIST_Y+i*ITEM_H+10, sel?COL_TEXT:COL_DIM, 1);
    }

    // Scroll indicator
    if ((int)gEntries.size()>VISIBLE){
        int barH=LIST_H*VISIBLE/gEntries.size();
        int barY=LIST_Y+LIST_H*gEntryScroll/gEntries.size();
        FillRect(LIST_X+LIST_W+2,barY,4,barH,COL_BORDER);
    }

    // Preview (right panel)
    FillRect(PREVIEW_X,PREVIEW_Y,PREVIEW_W,PREVIEW_H,{8,8,8,255});
    DrawRect(PREVIEW_X,PREVIEW_Y,PREVIEW_W,PREVIEW_H,COL_BORDER);
    if (gPreviewTex) {
        int tw,th; SDL_QueryTexture(gPreviewTex,nullptr,nullptr,&tw,&th);
        // Fit within preview area keeping aspect ratio
        float scale=std::min((float)PREVIEW_W/tw,(float)PREVIEW_H/th);
        int dw=(int)(tw*scale), dh=(int)(th*scale);
        int dx=PREVIEW_X+(PREVIEW_W-dw)/2, dy=PREVIEW_Y+(PREVIEW_H-dh)/2;
        SDL_Rect dst{dx,dy,dw,dh};
        SDL_RenderCopy(gRen,gPreviewTex,nullptr,&dst);
        DrawTextC(gPreviewStem+" ("+std::to_string(tw)+"x"+std::to_string(th)+")",
                  PREVIEW_X+PREVIEW_W/2, PREVIEW_Y+PREVIEW_H+16, COL_DIM, 1);
    } else {
        DrawTextC("no preview", PREVIEW_X+PREVIEW_W/2, PREVIEW_Y+PREVIEW_H/2, COL_BORDER, 1);
    }

    // Message
    if (!gOnSwitchMsg.empty())
        DrawTextC(gOnSwitchMsg, PREVIEW_X+PREVIEW_W/2, PREVIEW_Y+PREVIEW_H+34, gOnSwitchMsgCol, 1);

    DrawFooter("Up/Down  select    A  import PNG    Y  export PNG    B  back    +  quit");
    SDL_RenderPresent(gRen);
}

static void DrawError() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);
    DrawHeader("");
    DrawTextC("error", SCREEN_W/2, SCREEN_H/2-30, COL_RED, 2);
    DrawTextC(gError,  SCREEN_W/2, SCREEN_H/2+10, COL_DIM, 1);
    DrawFooter("+  quit");
    SDL_RenderPresent(gRen);
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main(int,char**) {
    nifmInitialize(NifmServiceType_User);
    socketInitialize(socketGetDefaultInitConfig());
    SDL_Init(SDL_INIT_VIDEO|SDL_INIT_JOYSTICK);
    IMG_Init(IMG_INIT_PNG|IMG_INIT_JPG);

    SDL_Window* win=SDL_CreateWindow("TomoToolNX",
        SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,SCREEN_W,SCREEN_H,0);
    gRen=SDL_CreateRenderer(win,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
    SDL_SetRenderDrawBlendMode(gRen,SDL_BLENDMODE_BLEND);

    padConfigureInput(1,HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&gPad);
    mkdir("/switch/TomoToolNX",0777);

    gUsers=SaveMount::GetUsers();
    if (gUsers.empty()){
        gError="No user accounts found.";
        gScreen=Screen::Error;
    }

    bool running = true; (void)running;

    while (appletMainLoop()) {
        padUpdate(&gPad);
        u64 kDown=padGetButtonsDown(&gPad);

        if (kDown&HidNpadButton_Plus) break;

        switch (gScreen) {

        case Screen::UserPick:
            if (kDown&HidNpadButton_Up)   { if(gUserSel>0) gUserSel--; }
            if (kDown&HidNpadButton_Down) { if(gUserSel+1<(int)gUsers.size()) gUserSel++; }
            if (kDown&HidNpadButton_A) {
                // Mount save first so we can check backup state
                gScreen=Screen::Mounting;
                DrawMounting();
                {
                    std::string err=SaveMount::Mount(gUsers[gUserSel].uid);
                    if (!err.empty()){gError=err;gScreen=Screen::Error;break;}
                }
                // Now decide: backup prompt or straight to mode picker
                if (BackupService::HasExistingBackup()) {
                    gScreen=Screen::BackupPrompt;
                } else {
                    // No existing backup — start one immediately
                    BackupService::StartFullBackup("tomodata:/");
                    gScreen=Screen::BackingUp;
                }
            }
            DrawUserPick();
            break;

        case Screen::BackupPrompt:
            if (kDown&HidNpadButton_A) {
                // Delete old, make new
                BackupService::DeleteBackup();
                BackupService::StartFullBackup("tomodata:/");
                gScreen=Screen::BackingUp;
            }
            if (kDown&HidNpadButton_B) {
                // Keep old, skip backup — go straight to mode picker
                gScreen=Screen::ModePick;
            }
            if (kDown&HidNpadButton_X) {
                // Keep old (rename save/ to save_old/), then make new save/
                rename(BACKUP_SAVE, BACKUP_ROOT "/save_old");
                BackupService::StartFullBackup("tomodata:/");
                gScreen=Screen::BackingUp;
            }
            DrawBackupPrompt();
            break;

        case Screen::BackingUp:
            if (BackupService::BackupDone()) {
                gScreen=Screen::ModePick;
            }
            DrawBackingUp();
            break;

        case Screen::ModePick:
            if (kDown&HidNpadButton_B) {
                // Back to user pick — unmount save
                SaveMount::Unmount();
                gScreen=Screen::UserPick;
            }
            if (kDown&HidNpadButton_L || kDown&HidNpadButton_ZL) {
                gIP=SaveMount::GetLocalIP();
                if (gIP.empty()) gIP="?.?.?.?";
                HttpServer::Start(HTTP_PORT, SAVE_UGC_PATH);
                gScreen=Screen::WebUI;
            }
            if (kDown&HidNpadButton_R || kDown&HidNpadButton_ZR) {
                gEntries=UgcScanner::Scan(SAVE_UGC_PATH);
                gEntrySel=0; gEntryScroll=0;
                FreePreview();
                gOnSwitchMsg="";
                if (!gEntries.empty()) LoadPreview(gEntries[0]);
                gScreen=Screen::OnSwitch;
            }
            DrawModePick();
            break;

        case Screen::Mounting:
            DrawMounting();
            break;

        case Screen::WebUI:
            if (kDown&HidNpadButton_B){
                HttpServer::Stop();
                gLog.clear();
                gScreen=Screen::ModePick;
                break;
            }
            {
                std::vector<HttpServer::LogEntry> httpLogs;
                HttpServer::DrainLog(httpLogs);
                for (auto& hl:httpLogs) Log(hl.text, hl.isError?COL_RED:COL_ACCENT);
            }
            if (HttpServer::HasPendingImport()) {
                auto job=HttpServer::TakePendingImport();
                std::string importErr=TextureProcessor::ImportPng(job.opts);
                remove(job.tmpPath.c_str());
                HttpServer::FinishImport(importErr);
                if (!importErr.empty()) LogERR("Import: "+importErr);
                else LogOK("Import OK");
            }
            if (HttpServer::HasPendingCommit()) {
                LogINF("Committing save...");
                HttpServer::ClearPendingCommit();
                std::string cerr=SaveMount::Commit();
                if (cerr.empty()) LogOK("Saved");
                else LogERR("Commit: "+cerr);
            }
            DrawWebUI();
            break;

        case Screen::OnSwitch:
            if (gShowPngPicker) {
                if (kDown&HidNpadButton_Up){
                    if(gPngSel>0){gPngSel--;if(gPngSel<gPngScroll)gPngScroll=gPngSel;}
                }
                if (kDown&HidNpadButton_Down){
                    if(gPngSel+1<(int)gPngFiles.size()){
                        gPngSel++;
                        int pvis=(SCREEN_H-200)/32;
                        if(gPngSel>=gPngScroll+pvis)gPngScroll=gPngSel-pvis+1;
                    }
                }
                if (kDown&HidNpadButton_A && !gPngFiles.empty()){
                    gShowPngPicker=false;
                    DoOnSwitchImport(gPngFiles[gPngSel]);
                }
                if (kDown&HidNpadButton_B){ gShowPngPicker=false; }
            } else {
                if (kDown&HidNpadButton_Up){
                    if(gEntrySel>0){
                        gEntrySel--;
                        if(gEntrySel<gEntryScroll)gEntryScroll=gEntrySel;
                        if(!gEntries.empty())LoadPreview(gEntries[gEntrySel]);
                    }
                }
                if (kDown&HidNpadButton_Down){
                    if(gEntrySel+1<(int)gEntries.size()){
                        gEntrySel++;
                        if(gEntrySel>=gEntryScroll+VISIBLE)gEntryScroll=gEntrySel-VISIBLE+1;
                        LoadPreview(gEntries[gEntrySel]);
                    }
                }
                if (kDown&HidNpadButton_A){
                    // Import: show PNG picker
                    ScanPngs();
                    if (gPngFiles.empty()){
                        gOnSwitchMsg="No PNG files found in /switch/TomoToolNX/ or /switch/";
                        gOnSwitchMsgCol=COL_RED;
                    } else {
                        gShowPngPicker=true;
                    }
                }
                if (kDown&HidNpadButton_Y && !gEntries.empty()){
                    // Export
                    const auto& e=gEntries[gEntrySel];
                    RgbaImage img;
                    std::string err=TextureProcessor::DecodeFile(e.ugctexPath,img);
                    if (!err.empty()){gOnSwitchMsg=err;gOnSwitchMsgCol=COL_RED;}
                    else {
                        // Save as PNG via SDL_image
                        std::string outPath="/switch/TomoToolNX/"+e.stem+".png";
                        SDL_Surface* surf=SDL_CreateRGBSurfaceFrom(
                            img.pixels.data(),img.width,img.height,32,img.width*4,
                            0x000000FF,0x0000FF00,0x00FF0000,0xFF000000);
                        if (surf){
                            IMG_SavePNG(surf,outPath.c_str());
                            SDL_FreeSurface(surf);
                            gOnSwitchMsg="Exported to "+outPath;
                            gOnSwitchMsgCol=COL_GREEN;
                        }
                    }
                }
                if (kDown&HidNpadButton_B){
                    // Back to mode pick — keep save mounted
                    FreePreview();
                    HttpServer::Stop();
                    gLog.clear();
                    gScreen=Screen::ModePick;
                }
            }
            DrawOnSwitch();
            break;

        case Screen::Error:
            DrawError();
            break;
        }
    }

    FreePreview();
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
