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
#include "mii_manager.h"
#include "qrcode.hpp"
#include "updater.h"

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

// ─── SDL_ttf font renderer ────────────────────────────────────────────────────

#include <SDL2/SDL_ttf.h>
#include "font_data.h"

static TTF_Font* gFontSm  = nullptr; // 16px — body text, labels
static TTF_Font* gFontMd  = nullptr; // 22px — section titles, selected items
static TTF_Font* gFontLg  = nullptr; // 32px — screen titles, hero text

static void FontInit() {
    TTF_Init();
    // Load font from embedded byte array — no romfs required
    SDL_RWops* rw = SDL_RWFromConstMem(FONT_DATA, (int)FONT_DATA_SIZE);
    if (rw) {
        gFontSm = TTF_OpenFontRW(rw, 0, 16);
        SDL_RWseek(rw, 0, RW_SEEK_SET);
        gFontMd = TTF_OpenFontRW(rw, 0, 22);
        SDL_RWseek(rw, 0, RW_SEEK_SET);
        gFontLg = TTF_OpenFontRW(rw, 1, 32); // 1 = free rw when done
    }
}
static void FontQuit() {
    if(gFontSm) TTF_CloseFont(gFontSm);
    if(gFontMd) TTF_CloseFont(gFontMd);
    if(gFontLg) TTF_CloseFont(gFontLg);
    TTF_Quit();
}

enum class Font { Sm, Md, Lg };
static TTF_Font* GetFont(Font f) {
    switch(f){ case Font::Md: return gFontMd; case Font::Lg: return gFontLg; default: return gFontSm; }
}

// Draw text at (x,y) top-left
static void DrawText(const std::string& text, int x, int y, SDL_Color col, Font f=Font::Sm) {
    if(text.empty()) return;
    TTF_Font* font = GetFont(f);
    SDL_Surface* surf = TTF_RenderUTF8_Blended(font, text.c_str(), col);
    if(!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(gRen, surf);
    SDL_FreeSurface(surf);
    if(!tex) return;
    int w,h; SDL_QueryTexture(tex,nullptr,nullptr,&w,&h);
    SDL_Rect dst{x,y,w,h};
    SDL_RenderCopy(gRen,tex,nullptr,&dst);
    SDL_DestroyTexture(tex);
}

// Draw text centered at (cx,cy)
static void DrawTextC(const std::string& text, int cx, int cy, SDL_Color col, Font f=Font::Sm) {
    if(text.empty()) return;
    TTF_Font* font = GetFont(f);
    int w,h; TTF_SizeUTF8(font, text.c_str(), &w, &h);
    DrawText(text, cx-w/2, cy-h/2, col, f);
}

// Draw text right-aligned at (rx,y)
static void DrawTextR(const std::string& text, int rx, int y, SDL_Color col, Font f=Font::Sm) {
    if(text.empty()) return;
    TTF_Font* font = GetFont(f);
    int w,h; TTF_SizeUTF8(font, text.c_str(), &w, &h);
    DrawText(text, rx-w, y-h/2, col, f);
}

static void FillRect(int x,int y,int w,int h,SDL_Color c){
    SDL_SetRenderDrawColor(gRen,c.r,c.g,c.b,c.a);
    SDL_Rect r{x,y,w,h};SDL_RenderFillRect(gRen,&r);
}
static void DrawRect(int x,int y,int w,int h,SDL_Color c){
    SDL_SetRenderDrawColor(gRen,c.r,c.g,c.b,c.a);
    SDL_Rect r{x,y,w,h};SDL_RenderDrawRect(gRen,&r);
}

// Draw a filled+outlined box sized to fit text with padding, then draw the text.
static int DrawTextBox(const std::string& text, int cx, int y, SDL_Color textCol,
                        SDL_Color boxCol, int padX=18, int padY=8, Font f=Font::Sm) {
    TTF_Font* font = GetFont(f);
    int tw=0, th=0;
    if (!text.empty() && font) TTF_SizeUTF8(font, text.c_str(), &tw, &th);
    int bw = tw + padX*2, bh = th + padY*2;
    int bx = cx - bw/2;
    FillRect(bx, y, bw, bh, COL_PANEL);
    DrawRect(bx, y, bw, bh, boxCol);
    DrawText(text, bx+padX, y+padY, textCol, f);
    return bh;
}




// ─── Screens ──────────────────────────────────────────────────────────────────

enum class Screen { UpdatePrompt, UpdateCheck, UpdateAvailable, Downloading, UserPick, BackupPrompt, BackingUp, ModePick, Mounting, WebUI, OnSwitch, Error };

static Screen      gScreen  = Screen::UpdatePrompt;
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

enum class OnSwitchMode { UGC, Mii };
static OnSwitchMode gOnSwitchMode = OnSwitchMode::UGC;

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

// Mii state
static std::vector<MiiManager::MiiSlot> gMiis;
static int gMiiSel    = 0;
static int gMiiScroll = 0;
static std::vector<std::string> gLtdFiles;
static int gLtdSel    = 0;
static int gLtdScroll = 0;
static bool gShowLtdPicker = false;

static void FreePreview() {
    if (gPreviewTex) { SDL_DestroyTexture(gPreviewTex); gPreviewTex=nullptr; }
    gPreviewStem = "";
}

static void LoadPreview(const UgcTextureEntry& e) {
    FreePreview();
    RgbaImage img;
    std::string err = TextureProcessor::DecodeFile(e.ugctexPath, img, true);
    if (!err.empty()) { gOnSwitchMsg=err; gOnSwitchMsgCol=COL_RED; return; }
    SDL_Surface* surf = SDL_CreateRGBSurfaceFrom(
        img.pixels.data(), img.width, img.height, 32, img.width*4,
        0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
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

static void ScanLtds() {
    gLtdFiles.clear();
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
            if (ext==".ltd")
                gLtdFiles.push_back(std::string(root)+"/"+name);
        }
        closedir(d);
    }
    std::sort(gLtdFiles.begin(), gLtdFiles.end());
    gLtdSel=0; gLtdScroll=0;
}

static void DoMiiImport(const std::string& ltdPath) {
    if (gMiis.empty()) return;
    int slot = gMiis[gMiiSel].slot;
    LogINF("Importing Mii slot "+std::to_string(slot)+"...");
    std::string err = MiiManager::ImportMii(slot, ltdPath);
    if (!err.empty()) { gOnSwitchMsg=err; gOnSwitchMsgCol=COL_RED; LogERR("Mii import failed: "+err); return; }
    std::string cerr = SaveMount::Commit();
    if (!cerr.empty()) { gOnSwitchMsg="Commit: "+cerr; gOnSwitchMsgCol=COL_RED; LogERR("Commit failed: "+cerr); return; }
    gMiis = MiiManager::ListMiis();
    if (gMiiSel >= (int)gMiis.size()) gMiiSel=(int)gMiis.size()-1;
    gOnSwitchMsg="Mii imported successfully"; gOnSwitchMsgCol=COL_GREEN;
    LogOK("Mii import OK: slot "+std::to_string(slot));
}

static void DoOnSwitchImport(const std::string& pngPath) {
    if (gEntries.empty()) return;
    const auto& e = gEntries[gEntrySel];

    LogINF("Importing "+e.stem+"...");
    TextureProcessor::ImportOptions opts;
    opts.pngPath            = pngPath;
    opts.destStem           = e.directory()+"/"+e.stem;
    opts.writeCanvas        = e.hasCanvas();
    opts.writeThumb         = false;
    opts.noSrgb             = false;
    opts.originalUgctexPath = e.ugctexPath;

    std::string err = TextureProcessor::ImportPng(opts);
    if (!err.empty()) {
        gOnSwitchMsg=err; gOnSwitchMsgCol=COL_RED;
        LogERR("Import failed: "+err); return;
    }
    std::string cerr = SaveMount::Commit();
    if (!cerr.empty()) {
        gOnSwitchMsg="Commit: "+cerr; gOnSwitchMsgCol=COL_RED;
        LogERR("Commit failed: "+cerr); return;
    }
    // Reload entry list and preview
    gEntries = UgcScanner::Scan(SAVE_UGC_PATH);
    if (gEntrySel >= (int)gEntries.size()) gEntrySel=(int)gEntries.size()-1;
    if (!gEntries.empty()) LoadPreview(gEntries[gEntrySel]);
    gOnSwitchMsg="Imported successfully"; gOnSwitchMsgCol=COL_GREEN;
    LogOK("Import OK: "+e.stem);
}

// ─── Draw helpers ─────────────────────────────────────────────────────────────

static void DrawHeader(const std::string& title) {
    FillRect(0,0,SCREEN_W,28,COL_PANEL);
    DrawRect(0,27,SCREEN_W,1,COL_BORDER);
    DrawText("TomoToolNX",10,6,COL_GOLD);
    if (!title.empty())
        DrawText("  /  "+title, 10+100, 6, COL_DIM);
}

static void DrawFooter(const std::string& hints) {
    FillRect(0,SCREEN_H-30,SCREEN_W,30,COL_PANEL);
    DrawRect(0,SCREEN_H-30,SCREEN_W,1,COL_BORDER);
    DrawTextC(hints, SCREEN_W/2, SCREEN_H-15, COL_DIM);
}

// ─── Screen draws ─────────────────────────────────────────────────────────────

static void DrawUpdatePrompt() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);
    DrawHeader("");
    DrawTextC("check for updates?", SCREEN_W/2, SCREEN_H/2-30, COL_TEXT, Font::Md);
    DrawTextC("current version: v" APP_VERSION, SCREEN_W/2, SCREEN_H/2+4, COL_DIM);
    DrawFooter("A  yes    B  skip    +  quit");
    SDL_RenderPresent(gRen);
}

static void DrawUpdateCheck() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);
    DrawHeader("");
    DrawTextC("checking for updates...", SCREEN_W/2, SCREEN_H/2, COL_DIM, Font::Md);
    SDL_RenderPresent(gRen);
}

static void DrawUpdateAvailable() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);
    DrawHeader("");
    DrawTextC("update available!", SCREEN_W/2, SCREEN_H/2-40, COL_GREEN, Font::Lg);
    DrawTextC("v" APP_VERSION + std::string("  →  v") + Updater::GetLatestVersion(),
              SCREEN_W/2, SCREEN_H/2+2, COL_TEXT, Font::Md);
    DrawFooter("A  download and install    B  skip    +  quit");
    SDL_RenderPresent(gRen);
}

static void DrawDownloading() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);
    DrawHeader("");
    auto state = Updater::GetState();
    if (state == Updater::State::Done) {
        DrawTextC("update installed!", SCREEN_W/2, SCREEN_H/2-20, COL_GREEN, Font::Md);
        DrawTextC("restart the app to apply", SCREEN_W/2, SCREEN_H/2+14, COL_DIM);
        DrawFooter("B  continue without restarting    +  quit");
    } else if (state == Updater::State::Error) {
        DrawTextC("download failed", SCREEN_W/2, SCREEN_H/2-20, COL_RED, Font::Md);
        DrawTextC(Updater::GetError(), SCREEN_W/2, SCREEN_H/2+14, COL_DIM);
        DrawFooter("B  continue    +  quit");
    } else {
        DrawTextC("downloading update...", SCREEN_W/2, SCREEN_H/2-30, COL_DIM, Font::Md);
        float prog = Updater::GetProgress();
        int bw=700, bh=10, bx=SCREEN_W/2-bw/2, by=SCREEN_H/2;
        FillRect(bx,by,bw,bh,COL_PANEL); DrawRect(bx,by,bw,bh,COL_BORDER);
        FillRect(bx,by,(int)(bw*prog),bh,COL_ACCENT);
        char pct[16]; snprintf(pct,sizeof(pct),"%d%%",(int)(prog*100));
        DrawTextC(pct, SCREEN_W/2, by+bh+18, COL_DIM);
        DrawTextC("do not turn off the console", SCREEN_W/2, by+bh+42, COL_DIM);
    }
    SDL_RenderPresent(gRen);
}

static void DrawUserPick() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b, 255);
    SDL_RenderClear(gRen);
    DrawHeader("");
    DrawTextC("select user", SCREEN_W/2, 44, COL_DIM);
    {
    TTF_Font* fMd3=GetFont(Font::Md); int mh=0,tmp2=0;
    if(fMd3) TTF_SizeUTF8(fMd3,"A",&tmp2,&mh);
    int itemH=mh+16, listTop=66;
    for (int i=0;i<(int)gUsers.size();i++){
        bool sel=(i==gUserSel);
        FillRect(SCREEN_W/2-280,listTop+i*(itemH+4),560,itemH, sel?COL_SEL:COL_PANEL);
        if(sel) DrawRect(SCREEN_W/2-280,listTop+i*(itemH+4),560,itemH,COL_ACCENT);
        DrawTextC(gUsers[i].nickname, SCREEN_W/2, listTop+i*(itemH+4)+itemH/2, sel?COL_TEXT:COL_DIM, Font::Md);
    }
    }
    DrawFooter("Up/Down  navigate    A  select    +  quit");
    SDL_RenderPresent(gRen);
}

static void DrawBackupPrompt() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b, 255);
    SDL_RenderClear(gRen);
    DrawHeader("");
    DrawTextC("previous backup found", SCREEN_W/2, 58, COL_DIM);
    DrawTextC("what would you like to do?", SCREEN_W/2, 82, COL_TEXT, Font::Md);

    // Measure to size cards
    {
    TTF_Font* fMd2=GetFont(Font::Md), *fSm2=GetFont(Font::Sm);
    int px=20, py=10, gap2=20;
    int smh2=0, mdh2=0, tmp=0;
    if(fMd2) TTF_SizeUTF8(fMd2,"A",&tmp,&mdh2);
    if(fSm2) TTF_SizeUTF8(fSm2,"A",&tmp,&smh2);
    // widest text
    int maxw=0;
    const char* labels[]={"delete old backup","then make new one","skip backup","keep old as-is","keep old + make new","saves both backups"};
    for(auto l:labels){int w=0,h=0;if(fSm2)TTF_SizeUTF8(fSm2,l,&w,&h);if(w>maxw)maxw=w;}
    int cw2=maxw+px*2+40; // extra for key label
    int ch2=py+mdh2+8+smh2+6+smh2+py;
    int gap3=20, x1=SCREEN_W/2-cw2-gap3/2, x2=SCREEN_W/2+gap3/2;
    int cy2=100;

    FillRect(x1,cy2,cw2,ch2,COL_PANEL); DrawRect(x1,cy2,cw2,ch2,COL_RED);
    DrawTextC("[A]",               x1+cw2/2, cy2+py+mdh2/2,              COL_RED, Font::Md);
    DrawTextC("delete old backup", x1+cw2/2, cy2+py+mdh2+8+smh2/2,       COL_TEXT);
    DrawTextC("then make new one", x1+cw2/2, cy2+py+mdh2+8+smh2+6+smh2/2,COL_DIM);

    FillRect(x2,cy2,cw2,ch2,COL_PANEL); DrawRect(x2,cy2,cw2,ch2,COL_ACCENT);
    DrawTextC("[B]",             x2+cw2/2, cy2+py+mdh2/2,              COL_ACCENT, Font::Md);
    DrawTextC("skip backup",     x2+cw2/2, cy2+py+mdh2+8+smh2/2,       COL_TEXT);
    DrawTextC("keep old as-is",  x2+cw2/2, cy2+py+mdh2+8+smh2+6+smh2/2,COL_DIM);

    int x3=SCREEN_W/2-cw2/2, cy3=cy2+ch2+14;
    FillRect(x3,cy3,cw2,ch2,COL_PANEL); DrawRect(x3,cy3,cw2,ch2,COL_GOLD);
    DrawTextC("[X]",                 x3+cw2/2, cy3+py+mdh2/2,              COL_GOLD, Font::Md);
    DrawTextC("keep old + make new", x3+cw2/2, cy3+py+mdh2+8+smh2/2,       COL_TEXT);
    DrawTextC("saves both backups",  x3+cw2/2, cy3+py+mdh2+8+smh2+6+smh2/2,COL_DIM);
    }

    DrawFooter("A  delete old + new    B  skip    X  keep old + new alongside    +  quit");
    SDL_RenderPresent(gRen);
}

static void DrawBackingUp() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b, 255);
    SDL_RenderClear(gRen);
    DrawHeader("");
    DrawTextC("backing up save data...", SCREEN_W/2, SCREEN_H/2-30, COL_DIM, Font::Md);
    float prog = BackupService::BackupProgress();
    int bw=700, bh=10, bx=SCREEN_W/2-bw/2, by=SCREEN_H/2;
    FillRect(bx,by,bw,bh,COL_PANEL);
    DrawRect(bx,by,bw,bh,COL_BORDER);
    FillRect(bx,by,(int)(bw*prog),bh,COL_ACCENT);
    char pct[16]; snprintf(pct,sizeof(pct),"%d%%",(int)(prog*100));
    DrawTextC(pct, SCREEN_W/2, by+bh+18, COL_DIM);
    DrawTextC("do not turn off the console", SCREEN_W/2, by+bh+42, COL_DIM);
    SDL_RenderPresent(gRen);
}

static void DrawModePick() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b, 255);
    SDL_RenderClear(gRen);
    DrawHeader("");
    DrawTextC("choose mode", SCREEN_W/2, 62, COL_DIM);

    // Measure text to size cards dynamically
    TTF_Font* fLg=GetFont(Font::Lg), *fSm=GetFont(Font::Sm), *fMd=GetFont(Font::Md);
    int gap=24, padX=28, padY=14;
    auto cardW = [&](const std::string& main, const std::string& sub) {
        int w1=0,w2=0,h=0;
        if(fLg) TTF_SizeUTF8(fLg,main.c_str(),&w1,&h);
        if(fSm) TTF_SizeUTF8(fSm,sub.c_str(),&w2,&h);
        return std::max(w1,w2)+padX*2;
    };
    int cw1=cardW("WebUI","browser on PC or phone");
    int cw2=cardW("On-Switch","controller, no network");
    int cw=std::max(cw1,cw2);

    // Card height: Lg title + gap + Sm sub + gap + Md button
    int lgh=0,smh=0,mdh=0;
    if(fLg) TTF_SizeUTF8(fLg,"A",&lgh,&lgh);
    if(fSm) TTF_SizeUTF8(fSm,"A",&smh,&smh);
    if(fMd) TTF_SizeUTF8(fMd,"A",&mdh,&mdh);
    int ch = padY + lgh + 10 + smh + 10 + mdh + padY;

    int cy=82, cx1=SCREEN_W/2-cw-gap/2, cx2=SCREEN_W/2+gap/2;

    // WebUI card
    FillRect(cx1,cy,cw,ch,COL_PANEL); DrawRect(cx1,cy,cw,ch,COL_ACCENT);
    DrawTextC("WebUI",                 cx1+cw/2, cy+padY+lgh/2,                COL_TEXT, Font::Lg);
    DrawTextC("browser on PC or phone",cx1+cw/2, cy+padY+lgh+10+smh/2,         COL_DIM);
    DrawTextC("[L]",                   cx1+cw/2, cy+padY+lgh+10+smh+10+mdh/2,  COL_ACCENT, Font::Md);

    // On-Switch card
    FillRect(cx2,cy,cw,ch,COL_PANEL); DrawRect(cx2,cy,cw,ch,COL_GOLD);
    DrawTextC("On-Switch",             cx2+cw/2, cy+padY+lgh/2,                COL_TEXT, Font::Lg);
    DrawTextC("controller, no network",cx2+cw/2, cy+padY+lgh+10+smh/2,         COL_DIM);
    DrawTextC("[R]",                   cx2+cw/2, cy+padY+lgh+10+smh+10+mdh/2,  COL_GOLD, Font::Md);

    DrawFooter("L  WebUI mode    R  On-Switch mode    B  back    +  quit");
    SDL_RenderPresent(gRen);
}

static void DrawMounting() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b, 255);
    SDL_RenderClear(gRen);
    DrawTextC("mounting save...", SCREEN_W/2, SCREEN_H/2, COL_DIM, Font::Md);
    SDL_RenderPresent(gRen);
}

static void DrawWebUI() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b, 255);
    SDL_RenderClear(gRen);
    DrawHeader("webui");

    bool wifiActive = !gIP.empty() && gIP != "?.?.?.?";
    std::string url = "http://"+gIP+":"+std::to_string(HTTP_PORT);

    // WiFi status
    int sy=36;
    DrawText("WiFi : ", 16, sy, COL_DIM);
    TTF_Font* fsm=GetFont(Font::Sm); int lw=0,lh=0;
    if(fsm) TTF_SizeUTF8(fsm,"WiFi : ",&lw,&lh);
    DrawText(wifiActive?"Active":"Inactive", 16+lw, sy,
             wifiActive?COL_GREEN:COL_RED);

    // URL
    DrawTextC(url, SCREEN_W/2, sy+lh+12, COL_GOLD, Font::Md);

    // QR code rendered with SDL (so user can scan with phone)
    if (wifiActive) {
        QR::Mat mat;
        if (QR::build(url, mat)) {
            int ms=5, border=2;
            int qrSize=mat.N*ms+border*2*ms;
            int qx=SCREEN_W-qrSize-20, qy=30;
            // White background
            FillRect(qx,qy,qrSize,qrSize,{255,255,255,255});
            // Dark modules
            SDL_SetRenderDrawColor(gRen,0,0,0,255);
            for (int y=0;y<mat.N;y++) for (int x=0;x<mat.N;x++) {
                if (mat.get(x,y)) {
                    SDL_Rect r{qx+(x+border)*ms, qy+(y+border)*ms, ms, ms};
                    SDL_RenderFillRect(gRen,&r);
                }
            }
            DrawTextC("scan to open", qx+qrSize/2, qy+qrSize+12, COL_DIM);
        }
    }

    // Log
    int logY=36+lh+lh+28;
    for (auto& line : gLog) { DrawText(line.text, 16, logY, line.col); logY+=20; }

    DrawFooter("B  back    +  quit");
    SDL_RenderPresent(gRen);
}

static const int LIST_X=12, LIST_W=380, LIST_Y=30, LIST_H=SCREEN_H-62, ITEM_H=28;
static const int PREVIEW_X=400, PREVIEW_Y=30, PREVIEW_W=868, PREVIEW_H=498;
static const int VISIBLE = LIST_H/ITEM_H;

static void DrawFilePicker(const std::vector<std::string>& files, int sel, int scroll,
                           const std::string& title, const std::string& ext) {
    FillRect(40,36,SCREEN_W-80,SCREEN_H-66,COL_PANEL);
    DrawRect(40,36,SCREEN_W-80,SCREEN_H-66,COL_BORDER);
    DrawTextC(title, SCREEN_W/2, 56, COL_DIM, Font::Md);
    if (files.empty()) {
        DrawTextC("no "+ext+" files found in /switch/TomoToolNX/ or /switch/",
                  SCREEN_W/2, SCREEN_H/2, COL_DIM);
    } else {
        int py=76, ph=26;
        int pvis=(SCREEN_H-136)/ph;
        for (int i=0;i<pvis;i++){
            int idx=scroll+i;
            if (idx>=(int)files.size()) break;
            bool isSel=(idx==sel);
            FillRect(52,py+i*ph,SCREEN_W-104,ph-2, isSel?COL_SEL:COL_PANEL2);
            if(isSel) DrawRect(52,py+i*ph,SCREEN_W-104,ph-2,COL_ACCENT);
            std::string name=files[idx];
            size_t sl=name.rfind('/');
            if(sl!=std::string::npos) name=name.substr(sl+1);
            DrawText(name,60,py+i*ph+5,isSel?COL_TEXT:COL_DIM);
        }
    }
    DrawFooter("Up/Down  navigate    A  select    B  cancel");
}

static void DrawOnSwitch() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b, 255);
    SDL_RenderClear(gRen);
    DrawHeader("on-switch");

    // Mode tabs
    {
        int tw=180, th=24, tx=SCREEN_W/2-tw-2, ty=8;
        FillRect(tx,ty,tw,th, gOnSwitchMode==OnSwitchMode::UGC?COL_SEL:COL_PANEL);
        DrawRect(tx,ty,tw,th, gOnSwitchMode==OnSwitchMode::UGC?COL_ACCENT:COL_BORDER);
        DrawTextC("textures [L]", tx+tw/2, ty+th/2, gOnSwitchMode==OnSwitchMode::UGC?COL_TEXT:COL_DIM);
        tx=SCREEN_W/2+2;
        FillRect(tx,ty,tw,th, gOnSwitchMode==OnSwitchMode::Mii?COL_SEL:COL_PANEL);
        DrawRect(tx,ty,tw,th, gOnSwitchMode==OnSwitchMode::Mii?COL_GOLD:COL_BORDER);
        DrawTextC("miis [R]", tx+tw/2, ty+th/2, gOnSwitchMode==OnSwitchMode::Mii?COL_TEXT:COL_DIM);
    }

    // ── File pickers (overlay) ───────────────────────────────────────────────
    if (gShowPngPicker) {
        DrawFilePicker(gPngFiles, gPngSel, gPngScroll, "select PNG to import", "PNG");
        SDL_RenderPresent(gRen); return;
    }
    if (gShowLtdPicker) {
        DrawFilePicker(gLtdFiles, gLtdSel, gLtdScroll, "select .ltd Mii file to import", "ltd");
        SDL_RenderPresent(gRen); return;
    }

    // ── UGC mode ─────────────────────────────────────────────────────────────
    if (gOnSwitchMode == OnSwitchMode::UGC) {
        FillRect(LIST_X-4,LIST_Y-4,LIST_W+8,LIST_H+8,COL_PANEL);
        DrawRect(LIST_X-4,LIST_Y-4,LIST_W+8,LIST_H+8,COL_BORDER);
        for (int i=0;i<VISIBLE;i++){
            int idx=gEntryScroll+i;
            if (idx>=(int)gEntries.size()) break;
            bool sel=(idx==gEntrySel);
            FillRect(LIST_X,LIST_Y+i*ITEM_H,LIST_W,ITEM_H-1, sel?COL_SEL:COL_BG);
            if(sel) DrawRect(LIST_X,LIST_Y+i*ITEM_H,LIST_W,ITEM_H-1,COL_ACCENT);
            DrawText(gEntries[idx].stem, LIST_X+6, LIST_Y+i*ITEM_H+6, sel?COL_TEXT:COL_DIM);
        }
        if ((int)gEntries.size()>VISIBLE){
            int barH=LIST_H*VISIBLE/gEntries.size();
            int barY=LIST_Y+LIST_H*gEntryScroll/gEntries.size();
            FillRect(LIST_X+LIST_W+2,barY,4,barH,COL_BORDER);
        }
        FillRect(PREVIEW_X,PREVIEW_Y,PREVIEW_W,PREVIEW_H,{8,8,8,255});
        DrawRect(PREVIEW_X,PREVIEW_Y,PREVIEW_W,PREVIEW_H,COL_BORDER);
        if (gPreviewTex) {
            int tw,th; SDL_QueryTexture(gPreviewTex,nullptr,nullptr,&tw,&th);
            float scale=std::min((float)PREVIEW_W/tw,(float)PREVIEW_H/th);
            int dw=(int)(tw*scale), dh=(int)(th*scale);
            SDL_Rect dst{PREVIEW_X+(PREVIEW_W-dw)/2, PREVIEW_Y+(PREVIEW_H-dh)/2, dw, dh};
            SDL_RenderCopy(gRen,gPreviewTex,nullptr,&dst);
            DrawTextC(gPreviewStem+" ("+std::to_string(tw)+"x"+std::to_string(th)+")",
                      PREVIEW_X+PREVIEW_W/2, PREVIEW_Y+PREVIEW_H+16, COL_DIM);
        } else {
            DrawTextC("no preview", PREVIEW_X+PREVIEW_W/2, PREVIEW_Y+PREVIEW_H/2, COL_BORDER);
        }
        if (!gOnSwitchMsg.empty())
            DrawTextC(gOnSwitchMsg, PREVIEW_X+PREVIEW_W/2, PREVIEW_Y+PREVIEW_H+10, gOnSwitchMsgCol);
        int logY = PREVIEW_Y+PREVIEW_H+30;
        for (auto& line : gLog) { DrawText(line.text, PREVIEW_X, logY, line.col); logY+=20; }
        DrawFooter("Up/Down  select    A  import PNG    Y  export PNG    R  miis tab    B  back    +  quit");
    }

    // ── Mii mode ─────────────────────────────────────────────────────────────
    else {
        FillRect(LIST_X-4,LIST_Y-4,LIST_W+8,LIST_H+8,COL_PANEL);
        DrawRect(LIST_X-4,LIST_Y-4,LIST_W+8,LIST_H+8,COL_BORDER);
        for (int i=0;i<VISIBLE;i++){
            int idx=gMiiScroll+i;
            if (idx>=(int)gMiis.size()) break;
            bool sel=(idx==gMiiSel);
            FillRect(LIST_X,LIST_Y+i*ITEM_H,LIST_W,ITEM_H-1, sel?COL_SEL:COL_BG);
            if(sel) DrawRect(LIST_X,LIST_Y+i*ITEM_H,LIST_W,ITEM_H-1,COL_GOLD);
            std::string label=gMiis[idx].name;
            if(gMiis[idx].hasFacepaint) label+=" *";
            DrawText(label, LIST_X+6, LIST_Y+i*ITEM_H+6, sel?COL_TEXT:COL_DIM);
        }
        if ((int)gMiis.size()>VISIBLE){
            int barH=LIST_H*VISIBLE/gMiis.size();
            int barY=LIST_Y+LIST_H*gMiiScroll/gMiis.size();
            FillRect(LIST_X+LIST_W+2,barY,4,barH,COL_BORDER);
        }

        // Detail panel
        FillRect(PREVIEW_X,PREVIEW_Y,PREVIEW_W,PREVIEW_H,{8,8,8,255});
        DrawRect(PREVIEW_X,PREVIEW_Y,PREVIEW_W,PREVIEW_H,COL_BORDER);
        if (!gMiis.empty()) {
            const auto& m=gMiis[gMiiSel];
            DrawTextC(m.name, PREVIEW_X+PREVIEW_W/2, PREVIEW_Y+30, COL_TEXT, Font::Lg);
            DrawTextC("slot "+std::to_string(m.slot)+(m.hasFacepaint?"   has facepaint":""),
                      PREVIEW_X+PREVIEW_W/2, PREVIEW_Y+72, COL_DIM);
            DrawTextC("A  import .ltd", PREVIEW_X+PREVIEW_W/2, PREVIEW_Y+110, COL_ACCENT);
            DrawTextC("Y  export .ltd", PREVIEW_X+PREVIEW_W/2, PREVIEW_Y+138, COL_GOLD);
        }
        if (!gOnSwitchMsg.empty())
            DrawTextC(gOnSwitchMsg, PREVIEW_X+PREVIEW_W/2, PREVIEW_Y+PREVIEW_H+20, gOnSwitchMsgCol);
        // Activity log
        int mlogY = PREVIEW_Y+PREVIEW_H+38;
        for (auto& line : gLog) { DrawText(line.text, PREVIEW_X, mlogY, line.col); mlogY+=20; }
        DrawFooter("Up/Down  select    A  import .ltd    Y  export .ltd    L  textures tab    B  back    +  quit");
    }

    SDL_RenderPresent(gRen);
}

static void DrawError() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b, 255);
    SDL_RenderClear(gRen);
    DrawHeader("");
    DrawTextC("error", SCREEN_W/2, SCREEN_H/2-20, COL_RED, Font::Lg);
    DrawTextC(gError,  SCREEN_W/2, SCREEN_H/2+16, COL_DIM);
    DrawFooter("+  quit");
    SDL_RenderPresent(gRen);
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main(int,char**) {
    nifmInitialize(NifmServiceType_User);
    socketInitialize(socketGetDefaultInitConfig());
    SDL_Init(SDL_INIT_VIDEO|SDL_INIT_JOYSTICK);
    IMG_Init(IMG_INIT_PNG|IMG_INIT_JPG);
    TTF_Init();

    SDL_Window* win=SDL_CreateWindow("TomoToolNX",
        SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,SCREEN_W,SCREEN_H,0);
    gRen=SDL_CreateRenderer(win,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
    SDL_SetRenderDrawBlendMode(gRen,SDL_BLENDMODE_BLEND);
    FontInit();

    padConfigureInput(1,HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&gPad);
    mkdir("/switch/TomoToolNX", 0777);

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

        case Screen::UpdatePrompt:
            if (kDown&HidNpadButton_A) {
                Updater::StartCheck();
                gScreen=Screen::UpdateCheck;
            }
            if (kDown&HidNpadButton_B) { gScreen=Screen::UserPick; }
            DrawUpdatePrompt();
            break;

        case Screen::UpdateCheck:
            {
                auto state = Updater::GetState();
                if (state==Updater::State::UpdateAvailable) gScreen=Screen::UpdateAvailable;
                else if (state==Updater::State::NoUpdate||state==Updater::State::Error)
                    gScreen=Screen::UserPick;
            }
            DrawUpdateCheck();
            break;

        case Screen::UpdateAvailable:
            if (kDown&HidNpadButton_A) {
                Updater::StartDownload();
                gScreen=Screen::Downloading;
            }
            if (kDown&HidNpadButton_B) { gScreen=Screen::UserPick; }
            DrawUpdateAvailable();
            break;

        case Screen::Downloading:
            {
                auto state = Updater::GetState();
                if ((state==Updater::State::Done||state==Updater::State::Error)
                    && (kDown&HidNpadButton_B)) {
                    gScreen=Screen::UserPick;
                }
            }
            DrawDownloading();
            break;

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
                gMiis=MiiManager::ListMiis();
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
            // File pickers take priority
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
                if (kDown&HidNpadButton_B) gShowPngPicker=false;
            } else if (gShowLtdPicker) {
                if (kDown&HidNpadButton_Up){
                    if(gLtdSel>0){gLtdSel--;if(gLtdSel<gLtdScroll)gLtdScroll=gLtdSel;}
                }
                if (kDown&HidNpadButton_Down){
                    if(gLtdSel+1<(int)gLtdFiles.size()){
                        gLtdSel++;
                        int pvis=(SCREEN_H-200)/32;
                        if(gLtdSel>=gLtdScroll+pvis)gLtdScroll=gLtdSel-pvis+1;
                    }
                }
                if (kDown&HidNpadButton_A && !gLtdFiles.empty()){
                    gShowLtdPicker=false;
                    DoMiiImport(gLtdFiles[gLtdSel]);
                }
                if (kDown&HidNpadButton_B) gShowLtdPicker=false;
            } else if (gOnSwitchMode == OnSwitchMode::UGC) {
                // Tab switch
                if (kDown&HidNpadButton_R||kDown&HidNpadButton_ZR){
                    gOnSwitchMode=OnSwitchMode::Mii;
                    if(gMiis.empty()) gMiis=MiiManager::ListMiis();
                    gOnSwitchMsg=""; break;
                }
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
                    ScanPngs();
                    if(gPngFiles.empty()){gOnSwitchMsg="No PNG files found";gOnSwitchMsgCol=COL_RED;}
                    else gShowPngPicker=true;
                }
                if (kDown&HidNpadButton_Y && !gEntries.empty()){
                    const auto& e=gEntries[gEntrySel];
                    LogINF("Exporting "+e.stem+"...");
                    RgbaImage img;
                    std::string err=TextureProcessor::DecodeFile(e.ugctexPath,img,true);
                    if (!err.empty()){gOnSwitchMsg=err;gOnSwitchMsgCol=COL_RED;LogERR("Export failed: "+err);}
                    else {
                        std::string outPath="/switch/TomoToolNX/"+e.stem+".png";
                        SDL_Surface* surf=SDL_CreateRGBSurfaceWithFormatFrom(
                            img.pixels.data(),img.width,img.height,32,img.width*4,
                            SDL_PIXELFORMAT_RGBA32);
                        if(surf){IMG_SavePNG(surf,outPath.c_str());SDL_FreeSurface(surf);
                            gOnSwitchMsg="Exported to "+outPath;gOnSwitchMsgCol=COL_GREEN;
                            LogOK("Exported: "+e.stem+".png");}
                    }
                }
                if (kDown&HidNpadButton_B){
                    FreePreview(); HttpServer::Stop(); gLog.clear();
                    gScreen=Screen::ModePick;
                }
            } else { // Mii mode
                // Tab switch
                if (kDown&HidNpadButton_L||kDown&HidNpadButton_ZL){
                    gOnSwitchMode=OnSwitchMode::UGC; gOnSwitchMsg=""; break;
                }
                if (kDown&HidNpadButton_Up){
                    if(gMiiSel>0){gMiiSel--;if(gMiiSel<gMiiScroll)gMiiScroll=gMiiSel;}
                }
                if (kDown&HidNpadButton_Down){
                    if(gMiiSel+1<(int)gMiis.size()){
                        gMiiSel++;
                        if(gMiiSel>=gMiiScroll+VISIBLE)gMiiScroll=gMiiSel-VISIBLE+1;
                    }
                }
                if (kDown&HidNpadButton_A && !gMiis.empty()){
                    ScanLtds();
                    if(gLtdFiles.empty()){gOnSwitchMsg="No .ltd files found in /switch/TomoToolNX/";gOnSwitchMsgCol=COL_RED;}
                    else gShowLtdPicker=true;
                }
                if (kDown&HidNpadButton_Y && !gMiis.empty()){
                    int slot=gMiis[gMiiSel].slot;
                    std::string outPath="/switch/TomoToolNX/"+gMiis[gMiiSel].name+"_slot"+std::to_string(slot)+".ltd";
                    for(auto& c:outPath){
                        if(c==' '||c=='/'||c=='\\'||c==':'||c=='*'||c=='?'||
                           c=='"'||c=='<'||c=='>'||c=='|'||(uint8_t)c>127)
                            c='_';
                    }
                    LogINF("Exporting Mii slot "+std::to_string(slot)+"...");
                    std::string err=MiiManager::ExportMii(slot,outPath);
                    if(!err.empty()){gOnSwitchMsg=err;gOnSwitchMsgCol=COL_RED;LogERR("Mii export failed: "+err);}
                    else{gOnSwitchMsg="Exported to /switch/TomoToolNX/";gOnSwitchMsgCol=COL_GREEN;LogOK("Mii exported: slot "+std::to_string(slot));}
                }
                if (kDown&HidNpadButton_B){
                    FreePreview(); HttpServer::Stop(); gLog.clear();
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
    Updater::Cleanup();
    FontQuit();
    nifmExit();
    socketExit();
    SDL_DestroyRenderer(gRen);
    SDL_DestroyWindow(win);
    IMG_Quit();
    SDL_Quit();
    return 0;
}
