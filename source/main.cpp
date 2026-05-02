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

enum class Screen { UpdateCheck, UpdateAvailable, Downloading, UserPick, BackupPrompt, BackingUp, Mounting, OnSwitch, Error };

static Screen      gScreen  = Screen::UpdateCheck;
static std::string gError;
static std::string gIP;
static std::vector<SaveMount::UserInfo> gUsers;
static int         gUserSel = 0;
static std::vector<SDL_Texture*> gAvatarTextures;

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

enum class OnSwitchMode { UGC, Mii, WebUI };
static OnSwitchMode gOnSwitchMode = OnSwitchMode::UGC;

static std::vector<UgcTextureEntry> gEntries;
static int          gEntrySel    = 0;
static int          gEntryScroll = 0;
static SDL_Texture* gPreviewTex  = nullptr;
static std::string  gPreviewStem;
static std::string  gOnSwitchMsg;
static SDL_Color    gOnSwitchMsgCol = COL_TEXT;

// File browser
struct BrowseEntry { std::string name; bool isDir; };
static bool gShowFileBrowser = false;
static bool gBrowseForMii    = false;
static std::string gBrowseCurPath;
static std::string gBrowsePngPath = "/switch/TomoToolNX";
static std::string gBrowseLtdPath = "/switch/TomoToolNX";
static bool        gBrowseForExportDir = false;
static std::string gExportPath = "/switch/TomoToolNX/Exports";
static std::vector<BrowseEntry> gBrowseEntries;
static int gBrowseSel    = 0;
static int gBrowseScroll = 0;

// Mii state
static std::vector<MiiManager::MiiSlot> gMiis;
static int gMiiSel    = 0;
static int gMiiScroll = 0;

// Windows-style accelerating key repeat for directional navigation.
// Returns kDown OR any held directional buttons that should fire this frame.
static int s_navHold[64] = {};
static u64 NavRepeat(u64 kDown, u64 kHeld) {
    u64 result = kDown;
    static const u64 MASK =
        HidNpadButton_Up    | HidNpadButton_Down  |
        HidNpadButton_Left  | HidNpadButton_Right |
        HidNpadButton_StickLUp   | HidNpadButton_StickLDown  |
        HidNpadButton_StickLLeft | HidNpadButton_StickLRight |
        HidNpadButton_StickRUp   | HidNpadButton_StickRDown  |
        HidNpadButton_StickRLeft | HidNpadButton_StickRRight;
    for (u64 m = MASK, bit; m; m &= ~bit) {
        bit = m & (u64)(-(s64)m);
        int idx = __builtin_ctzll(bit);
        if (kHeld & bit) {
            int f = ++s_navHold[idx];
            if (f > 15 && (f - 15) % 2 == 0) result |= bit;
        } else {
            s_navHold[idx] = 0;
        }
    }
    return result;
}

static void FreePreview() {
    if (gPreviewTex) { SDL_DestroyTexture(gPreviewTex); gPreviewTex=nullptr; }
    gPreviewStem = "";
}

static std::string FormatStem(const std::string& stem) {
    static const struct { const char* key; const char* label; } MAP[] = {
        {"ugcfood",       "Food"},
        {"ugccloth",      "Cloth"},
        {"ugcgoods",      "Goods"},
        {"ugcinterior",   "Interior"},
        {"ugcexterior",   "Exterior"},
        {"ugcmapobject",  "Map Object"},
        {"ugcmapfloor",   "Map Floor"},
        {"ugcfacepaint",  "Face Paint"},
    };
    std::string low = stem;
    for (auto& c : low) c = (char)tolower((unsigned char)c);
    for (auto& m : MAP) {
        size_t kl = strlen(m.key);
        if (low.size() > kl && low.compare(0, kl, m.key) == 0) {
            std::string num = stem.substr(kl);
            size_t nz = num.find_first_not_of('0');
            num = (nz == std::string::npos) ? "0" : num.substr(nz);
            return std::string(m.label) + " " + num;
        }
    }
    return stem;
}

static void LoadPreview(const UgcTextureEntry& e) {
    FreePreview();
    RgbaImage img;
    std::string err = TextureProcessor::DecodeFile(e.ugctexPath, img, false);
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

static void BrowseRefresh(const std::string& path) {
    gBrowseCurPath = path;
    if (gBrowseCurPath.size() > 1 && gBrowseCurPath.back() == '/')
        gBrowseCurPath.pop_back();
    gBrowseEntries.clear();
    gBrowseSel = 0;
    gBrowseScroll = 0;

    std::string ext = gBrowseForMii ? ".ltd" : ".png";

    DIR* d = opendir(gBrowseCurPath.c_str());
    if (!d) return;

    std::vector<std::string> dirs, files;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        std::string name = de->d_name;
        if (name == "." || name == "..") continue;
        std::string full = gBrowseCurPath + "/" + name;
        bool isDir = (de->d_type == DT_DIR);
        if (de->d_type == DT_UNKNOWN) {
            struct stat st;
            if (stat(full.c_str(), &st) == 0) isDir = S_ISDIR(st.st_mode);
        }
        if (isDir) {
            dirs.push_back(name);
        } else {
            std::string lower = name;
            for (auto& c : lower) c = (char)tolower((unsigned char)c);
            if (lower.size() >= ext.size() &&
                lower.compare(lower.size() - ext.size(), ext.size(), ext) == 0)
                files.push_back(name);
        }
    }
    closedir(d);

    std::sort(dirs.begin(), dirs.end());
    std::sort(files.begin(), files.end());

    if (gBrowseCurPath != "/") gBrowseEntries.push_back({"[..]", true});
    for (auto& dn : dirs)  gBrowseEntries.push_back({dn, true});
    for (auto& fn : files) gBrowseEntries.push_back({fn, false});
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
    MiiManager::LoadUgcNames();
    if (gEntrySel >= (int)gEntries.size()) gEntrySel=(int)gEntries.size()-1;
    if (!gEntries.empty()) LoadPreview(gEntries[gEntrySel]);
    gOnSwitchMsg="Imported successfully"; gOnSwitchMsgCol=COL_GREEN;
    LogOK("Import OK: "+e.stem);
}

// ─── Draw helpers ─────────────────────────────────────────────────────────────

static void DrawHeader(const std::string& title) {
    FillRect(0,0,SCREEN_W,28,COL_PANEL);
    DrawRect(0,27,SCREEN_W,1,COL_BORDER);
    TTF_Font* fsm=GetFont(Font::Sm);
    int tw=0,fh=0;
    if(fsm) TTF_SizeUTF8(fsm,"TomoToolNX",&tw,&fh);
    int y=(28-fh)/2;
    DrawText("TomoToolNX",10,y,COL_GOLD);
    if (!title.empty())
        DrawText(" / "+title, 10+tw, y, COL_DIM);
    int vw=0;
    if(fsm) TTF_SizeUTF8(fsm,"v" APP_VERSION,&vw,&fh);
    DrawText("v" APP_VERSION, SCREEN_W-vw-10, y, COL_DIM);
}

static void DrawFooter(const std::string& hints) {
    FillRect(0,SCREEN_H-30,SCREEN_W,30,COL_PANEL);
    DrawRect(0,SCREEN_H-30,SCREEN_W,1,COL_BORDER);
    DrawTextC(hints, SCREEN_W/2, SCREEN_H-15, COL_DIM);
}

// ─── Screen draws ─────────────────────────────────────────────────────────────

static void DrawUpdateCheck() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);
    DrawHeader("");
    DrawTextC("checking for updates...", SCREEN_W/2, SCREEN_H/2, COL_DIM, Font::Md);
    DrawFooter("B  skip");
    SDL_RenderPresent(gRen);
}

static void DrawUpdateAvailable() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);
    DrawHeader("");
    DrawTextC("update available!", SCREEN_W/2, SCREEN_H/2-40, COL_GREEN, Font::Lg);
    DrawTextC("v" APP_VERSION + std::string("  ->  v") + Updater::GetLatestVersion(),
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
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);
    DrawHeader("");
    DrawTextC("select user", SCREEN_W/2, 44, COL_DIM, Font::Md);

    int n = (int)gUsers.size();
    if (n == 0) { DrawFooter("+  quit"); SDL_RenderPresent(gRen); return; }

    const int GAP      = 24;
    const int MAX_VIS  = std::min(n, 5);
    const int CARD_W   = std::min(220, (SCREEN_W - 120 - (MAX_VIS-1)*GAP) / MAX_VIS);
    const int AV_SIZE  = CARD_W - 30;
    const int CARD_H   = AV_SIZE + 66;

    int scroll = 0;
    if (n > MAX_VIS) {
        scroll = gUserSel - MAX_VIS/2;
        scroll = std::max(0, std::min(scroll, n - MAX_VIS));
    }

    int totalW = MAX_VIS*CARD_W + (MAX_VIS-1)*GAP;
    int startX = (SCREEN_W - totalW) / 2;
    int startY = 68 + (SCREEN_H - 36 - 68 - CARD_H) / 2;

    for (int i = 0; i < MAX_VIS; i++) {
        int idx = scroll + i;
        if (idx >= n) break;
        bool sel = (idx == gUserSel);
        int cx = startX + i*(CARD_W + GAP);

        FillRect(cx, startY, CARD_W, CARD_H, sel ? COL_SEL : COL_PANEL);
        DrawRect(cx, startY, CARD_W, CARD_H, sel ? COL_GOLD : COL_BORDER);
        if (sel) DrawRect(cx+1, startY+1, CARD_W-2, CARD_H-2, COL_GOLD);

        int avX = cx + (CARD_W - AV_SIZE)/2;
        int avY = startY + 14;
        if (idx < (int)gAvatarTextures.size() && gAvatarTextures[idx]) {
            SDL_Rect dst{avX, avY, AV_SIZE, AV_SIZE};
            SDL_RenderCopy(gRen, gAvatarTextures[idx], nullptr, &dst);
        } else {
            FillRect(avX, avY, AV_SIZE, AV_SIZE, COL_PANEL2);
            DrawRect(avX, avY, AV_SIZE, AV_SIZE, COL_BORDER);
            DrawTextC("?", cx+CARD_W/2, avY+AV_SIZE/2, COL_DIM, Font::Lg);
        }

        DrawTextC(gUsers[idx].nickname, cx+CARD_W/2, avY+AV_SIZE+20,
                  sel ? COL_TEXT : COL_DIM, Font::Sm);
    }

    if (scroll > 0)
        DrawTextC("<", startX-18, startY+CARD_H/2, COL_DIM, Font::Md);
    if (scroll + MAX_VIS < n)
        DrawTextC(">", startX+totalW+18, startY+CARD_H/2, COL_DIM, Font::Md);

    DrawFooter("Left/Right  navigate    A  select    +  quit");
    SDL_RenderPresent(gRen);
}

static void DrawBackupPrompt() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);
    DrawHeader("");
    DrawTextC("previous backup found", SCREEN_W/2, 44, COL_DIM, Font::Md);

    const int cw  = 300;
    const int ch  = 148;
    const int gap = 20;
    const int cx  = SCREEN_W/2 - (3*cw + 2*gap)/2;
    const int cy  = SCREEN_H/2 - ch/2 + 10;
    int x1=cx, x2=cx+cw+gap, x3=cx+(cw+gap)*2;

    FillRect(x1,cy,cw,ch,COL_PANEL); DrawRect(x1,cy,cw,ch,COL_RED);
    DrawTextC("[A]",                x1+cw/2, cy+36,  COL_RED,    Font::Lg);
    DrawTextC("delete old backup",  x1+cw/2, cy+86,  COL_TEXT,   Font::Md);
    DrawTextC("then make a new one",x1+cw/2, cy+116, COL_DIM,    Font::Sm);

    FillRect(x2,cy,cw,ch,COL_PANEL); DrawRect(x2,cy,cw,ch,COL_ACCENT);
    DrawTextC("[B]",           x2+cw/2, cy+36,  COL_ACCENT, Font::Lg);
    DrawTextC("skip backup",   x2+cw/2, cy+86,  COL_TEXT,   Font::Md);
    DrawTextC("keep old as-is",x2+cw/2, cy+116, COL_DIM,    Font::Sm);

    FillRect(x3,cy,cw,ch,COL_PANEL); DrawRect(x3,cy,cw,ch,COL_GOLD);
    DrawTextC("[X]",                 x3+cw/2, cy+36,  COL_GOLD, Font::Lg);
    DrawTextC("keep old + make new", x3+cw/2, cy+86,  COL_TEXT, Font::Md);
    DrawTextC("saves both backups",  x3+cw/2, cy+116, COL_DIM,  Font::Sm);

    DrawFooter("A  delete + new    B  skip    X  keep both    +  quit");
    SDL_RenderPresent(gRen);
}

static void DrawBackingUp() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b, 255);
    SDL_RenderClear(gRen);
    DrawHeader("");
    DrawTextC("backing up save data...", SCREEN_W/2, SCREEN_H/2-30, COL_DIM, Font::Md);
    float prog = BackupService::BackupProgress();
    int bw=1000, bh=10, bx=SCREEN_W/2-bw/2, by=SCREEN_H/2;
    FillRect(bx,by,bw,bh,COL_PANEL);
    DrawRect(bx,by,bw,bh,COL_BORDER);
    FillRect(bx,by,(int)(bw*prog),bh,COL_ACCENT);
    char pct[16]; snprintf(pct,sizeof(pct),"%d%%",(int)(prog*100));
    DrawTextC(pct, SCREEN_W/2, by+bh+18, COL_DIM);
    DrawTextC("do not turn off the console", SCREEN_W/2, by+bh+42, COL_DIM);
    SDL_RenderPresent(gRen);
}

static void DrawMounting() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b, 255);
    SDL_RenderClear(gRen);
    DrawTextC("mounting save...", SCREEN_W/2, SCREEN_H/2, COL_DIM, Font::Md);
    SDL_RenderPresent(gRen);
}

static const int LIST_X=12, LIST_W=380, LIST_Y=64, LIST_H=SCREEN_H-96, ITEM_H=28;
static const int LIST_PAD_TOP=6; // padding between box top and first item
static const int PREVIEW_X=400, PREVIEW_Y=LIST_Y, PREVIEW_W=868, PREVIEW_H=LIST_H;
static const int VISIBLE = LIST_H/ITEM_H;

static const char* CONFIG_PATH = "/switch/TomoToolNX/config.ini";

static void LoadConfig() {
    FILE* f = fopen(CONFIG_PATH, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        auto eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = s.substr(0, eq);
        std::string val = s.substr(eq + 1);
        while (!val.empty() && (val.back()=='\n'||val.back()=='\r'||val.back()==' '))
            val.pop_back();
        if (key == "export_path") gExportPath = val;
    }
    fclose(f);
}

static void SaveConfig() {
    FILE* f = fopen(CONFIG_PATH, "w");
    if (!f) return;
    fprintf(f, "export_path=%s\n", gExportPath.c_str());
    fclose(f);
}

static void DrawFileBrowser() {
    FillRect(40,36,SCREEN_W-80,SCREEN_H-66,COL_PANEL);
    DrawRect(40,36,SCREEN_W-80,SCREEN_H-66,COL_BORDER);
    if (gBrowseForExportDir)
        DrawTextC("select export folder", SCREEN_W/2, 52, COL_GOLD, Font::Md);
    else
        DrawTextC(gBrowseForMii ? "select .ltd" : "select PNG", SCREEN_W/2, 52, COL_DIM, Font::Md);
    DrawTextC(gBrowseCurPath, SCREEN_W/2, 72, COL_DIM);
    if (gBrowseForExportDir)
        DrawTextC("current: "+gExportPath, SCREEN_W/2, 90, COL_DIM);
    if (gBrowseEntries.empty()) {
        DrawTextC("empty folder", SCREEN_W/2, SCREEN_H/2, COL_DIM);
    } else {
        int py = gBrowseForExportDir ? 106 : 88, ph=26;
        int pvis=(SCREEN_H-(gBrowseForExportDir?166:148))/ph;
        TTF_Font* f=GetFont(Font::Sm); int fh=0,fw=0;
        if(f) TTF_SizeUTF8(f,"A",&fw,&fh);
        for (int i=0;i<pvis;i++){
            int idx=gBrowseScroll+i;
            if (idx>=(int)gBrowseEntries.size()) break;
            bool isSel=(idx==gBrowseSel);
            auto& entry=gBrowseEntries[idx];
            FillRect(52,py+i*ph,SCREEN_W-104,ph-2, isSel?COL_SEL:COL_PANEL2);
            SDL_Color border = COL_GOLD;
            if (isSel) DrawRect(52,py+i*ph,SCREEN_W-104,ph-2,border);
            std::string label = (entry.isDir && entry.name!="[..]") ? "/"+entry.name : entry.name;
            DrawText(label, 60, py+i*ph+(ph-2-fh)/2, isSel?COL_TEXT:COL_DIM);
        }
    }
    if (gBrowseForExportDir)
        DrawFooter("Up/Down  navigate    A  open folder    Y  set as export folder    X  cancel    B  back");
    else
        DrawFooter("Up/Down  navigate    A  open / select    B  back    X  cancel");
}

static void DrawOnSwitch() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);

    // Header with subtitle
    std::string subtitle = gOnSwitchMode==OnSwitchMode::UGC ? "textures" :
                           gOnSwitchMode==OnSwitchMode::Mii ? "miis" : "webui";
    DrawHeader(subtitle);

    // Tab bar (below header, above content)
    {
        int tw=180, th=22, gap=4;
        int totalW=tw*3+gap*2, tx=SCREEN_W/2-totalW/2, ty=28;
        struct { const char* label; OnSwitchMode mode; SDL_Color col; } tabs[]={
            {"textures", OnSwitchMode::UGC,   COL_GOLD},
            {"miis",     OnSwitchMode::Mii,   COL_GOLD},
            {"webui",    OnSwitchMode::WebUI, COL_GOLD},
        };
        for (auto& t : tabs) {
            bool sel = gOnSwitchMode==t.mode;
            FillRect(tx,ty,tw,th, sel?COL_SEL:COL_PANEL);
            DrawRect(tx,ty,tw,th, sel?t.col:COL_BORDER);
            DrawTextC(t.label, tx+tw/2, ty+th/2, sel?t.col:COL_DIM);
            tx+=tw+gap;
        }
    }

    // ── File browser (overlay) ───────────────────────────────────────────────
    if (gShowFileBrowser) {
        DrawFileBrowser();
        SDL_RenderPresent(gRen); return;
    }

    // ── UGC tab ──────────────────────────────────────────────────────────────
    if (gOnSwitchMode == OnSwitchMode::UGC) {
        FillRect(LIST_X-4,LIST_Y,LIST_W+8,LIST_H+4,COL_PANEL);
        DrawRect(LIST_X-4,LIST_Y,LIST_W+8,LIST_H+4,COL_BORDER);
        for (int i=0;i<VISIBLE;i++){
            int idx=gEntryScroll+i;
            if (idx>=(int)gEntries.size()) break;
            bool sel=(idx==gEntrySel);
            FillRect(LIST_X,LIST_Y+LIST_PAD_TOP+i*ITEM_H,LIST_W,ITEM_H-1, sel?COL_SEL:COL_BG);
            if(sel) DrawRect(LIST_X,LIST_Y+LIST_PAD_TOP+i*ITEM_H,LIST_W,ITEM_H-1,COL_GOLD);
            { std::string dn=MiiManager::GetUgcName(gEntries[idx].stem); if(dn.empty())dn=FormatStem(gEntries[idx].stem);
              TTF_Font* f=GetFont(Font::Sm); int fw=0,fh=0; if(f)TTF_SizeUTF8(f,dn.c_str(),&fw,&fh);
              DrawText(dn, LIST_X+6, LIST_Y+LIST_PAD_TOP+i*ITEM_H+(ITEM_H-fh)/2, sel?COL_TEXT:COL_DIM); }
        }
        if ((int)gEntries.size()>VISIBLE){
            int barH=LIST_H*VISIBLE/gEntries.size();
            int barY=LIST_Y+LIST_H*gEntryScroll/gEntries.size();
            FillRect(LIST_X+LIST_W+2,barY,4,barH,COL_BORDER);
        }
        FillRect(PREVIEW_X,PREVIEW_Y,PREVIEW_W,PREVIEW_H,{8,8,8,255});
        DrawRect(PREVIEW_X,PREVIEW_Y,PREVIEW_W,PREVIEW_H,COL_BORDER);
        // Header strip: show export/import message if active, else file stem
        if (!gOnSwitchMsg.empty())
            DrawTextC(gOnSwitchMsg, PREVIEW_X+PREVIEW_W/2, PREVIEW_Y+14, gOnSwitchMsgCol);
        else
            DrawTextC(gPreviewStem, PREVIEW_X+PREVIEW_W/2, PREVIEW_Y+14, COL_TEXT);
        {
            // Reserve space at the bottom for the import/export buttons
            const int kBtnH=46, kBtnGap=8, kBtnSlot=kBtnH+kBtnGap+4; // 58px total
            int imgY = PREVIEW_Y + 28;
            int imgH = PREVIEW_H - 28 - kBtnSlot;
            if (gPreviewTex) {
                int tw,th; SDL_QueryTexture(gPreviewTex,nullptr,nullptr,&tw,&th);
                float scale=std::min((float)PREVIEW_W/tw,(float)imgH/th);
                int dw=(int)(tw*scale), dh=(int)(th*scale);
                SDL_Rect dst{PREVIEW_X+(PREVIEW_W-dw)/2, imgY+(imgH-dh)/2, dw, dh};
                SDL_RenderCopy(gRen,gPreviewTex,nullptr,&dst);
                DrawTextC(std::to_string(tw)+"x"+std::to_string(th),
                          PREVIEW_X+PREVIEW_W/2, imgY+imgH-8, COL_DIM);
            } else {
                DrawTextC("no preview", PREVIEW_X+PREVIEW_W/2, imgY+imgH/2, COL_BORDER);
            }
            if (!gEntries.empty()) {
                int cx   = PREVIEW_X + PREVIEW_W/2;
                int btnY = imgY + imgH + kBtnGap;
                const int btnW=220, btnGap=24;
                FillRect(cx-btnW-btnGap/2, btnY, btnW, kBtnH, COL_SEL);
                DrawRect(cx-btnW-btnGap/2, btnY, btnW, kBtnH, COL_ACCENT);
                DrawTextC("A   import PNG", cx-btnW-btnGap/2+btnW/2, btnY+kBtnH/2, COL_ACCENT, Font::Md);
                FillRect(cx+btnGap/2, btnY, btnW, kBtnH, COL_PANEL);
                DrawRect(cx+btnGap/2, btnY, btnW, kBtnH, COL_GOLD);
                DrawTextC("Y   export PNG", cx+btnGap/2+btnW/2, btnY+kBtnH/2, COL_GOLD, Font::Md);
            }
        }
        DrawFooter("Up/Down  navigate    -  export folder    L/R  switch tab    B  back");
    }

    // ── Mii tab ───────────────────────────────────────────────────────────────
    else if (gOnSwitchMode == OnSwitchMode::Mii) {
        FillRect(LIST_X-4,LIST_Y,LIST_W+8,LIST_H+4,COL_PANEL);
        DrawRect(LIST_X-4,LIST_Y,LIST_W+8,LIST_H+4,COL_BORDER);
        for (int i=0;i<VISIBLE;i++){
            int idx=gMiiScroll+i;
            if (idx>=(int)gMiis.size()) break;
            bool sel=(idx==gMiiSel);
            FillRect(LIST_X,LIST_Y+LIST_PAD_TOP+i*ITEM_H,LIST_W,ITEM_H-1, sel?COL_SEL:COL_BG);
            if(sel) DrawRect(LIST_X,LIST_Y+LIST_PAD_TOP+i*ITEM_H,LIST_W,ITEM_H-1,COL_GOLD);
            std::string label=gMiis[idx].name;
            if(gMiis[idx].hasFacepaint) label+=" *";
            { TTF_Font* f=GetFont(Font::Sm); int fw=0,fh=0; if(f)TTF_SizeUTF8(f,label.c_str(),&fw,&fh);
                    DrawText(label, LIST_X+6, LIST_Y+LIST_PAD_TOP+i*ITEM_H+(ITEM_H-fh)/2, sel?COL_TEXT:COL_DIM); }
        }
        if ((int)gMiis.size()>VISIBLE){
            int barH=LIST_H*VISIBLE/gMiis.size();
            int barY=LIST_Y+LIST_H*gMiiScroll/gMiis.size();
            FillRect(LIST_X+LIST_W+2,barY,4,barH,COL_BORDER);
        }
        FillRect(PREVIEW_X,PREVIEW_Y,PREVIEW_W,PREVIEW_H,{8,8,8,255});
        DrawRect(PREVIEW_X,PREVIEW_Y,PREVIEW_W,PREVIEW_H,COL_BORDER);
        if (!gMiis.empty()) {
            const auto& m=gMiis[gMiiSel];
            int cx = PREVIEW_X+PREVIEW_W/2;
            int cy = PREVIEW_Y+PREVIEW_H/2;

            // Name — large, centered vertically in upper half
            DrawTextC(m.name, cx, PREVIEW_Y+PREVIEW_H/4, COL_TEXT, Font::Lg);

            // Slot + facepaint badge below name
            std::string meta = "slot "+std::to_string(m.slot);
            if (m.hasFacepaint) meta += "   ·   has facepaint";
            DrawTextC(meta, cx, PREVIEW_Y+PREVIEW_H/4+44, COL_DIM);

            // Divider
            FillRect(PREVIEW_X+40, cy-1, PREVIEW_W-80, 1, COL_BORDER);

            // Import / Export buttons — styled boxes in lower half
            int btnW=220, btnH=46, gap=24;
            int btn1X=cx-btnW-gap/2, btn2X=cx+gap/2, btnY=cy+30;

            FillRect(btn1X,btnY,btnW,btnH,COL_SEL);
            DrawRect(btn1X,btnY,btnW,btnH,COL_ACCENT);
            DrawTextC("A   import .ltd", btn1X+btnW/2, btnY+btnH/2, COL_ACCENT, Font::Md);

            FillRect(btn2X,btnY,btnW,btnH,COL_PANEL);
            DrawRect(btn2X,btnY,btnW,btnH,COL_GOLD);
            DrawTextC("Y   export .ltd", btn2X+btnW/2, btnY+btnH/2, COL_GOLD, Font::Md);
        }
        if (!gOnSwitchMsg.empty())
            DrawTextC(gOnSwitchMsg, PREVIEW_X+PREVIEW_W/2, PREVIEW_Y+PREVIEW_H/2+110, gOnSwitchMsgCol);
        DrawFooter("Up/Down  navigate    A  import .ltd    Y  export .ltd    -  export folder    L/R  switch tab    B  back");
    }

    // ── WebUI tab ─────────────────────────────────────────────────────────────
    else {
        bool wifiActive = !gIP.empty() && gIP!="?.?.?.?";
        std::string url = "http://"+gIP+":"+std::to_string(HTTP_PORT);

        // Layout: log box on left, wifi status + QR on right
        int margin=12;
        int contentY = LIST_Y;
        int contentH = SCREEN_H-contentY-32;

        // Log box — left half
        int logBoxW = SCREEN_W/2-margin-margin/2;
        int logBoxH = contentH;
        FillRect(margin,contentY,logBoxW,logBoxH,COL_PANEL);
        DrawRect(margin,contentY,logBoxW,logBoxH,COL_BORDER);
        DrawText("activity log", margin+8, contentY+6, COL_DIM);
        {
            int logY=contentY+26, logMaxY=contentY+logBoxH-10, lineH=20;
            int maxLines=(logMaxY-logY)/lineH;
            int startLine=(int)gLog.size()>maxLines?(int)gLog.size()-maxLines:0;
            for (int i=startLine;i<(int)gLog.size();i++) {
                if (logY+lineH>logMaxY) break;
                DrawText(gLog[i].text, margin+8, logY, gLog[i].col);
                logY+=lineH;
            }
            if (gLog.empty())
                DrawTextC("no activity yet", margin+logBoxW/2, contentY+logBoxH/2, COL_BORDER);
        }

        // Right half — URL top, WiFi status under it, QR code at bottom
        int rightX = SCREEN_W/2+margin/2;
        int rightW = SCREEN_W-rightX-margin;

        if (wifiActive) {
            TTF_Font* fsm=GetFont(Font::Sm); int lw=0,lh=0;
            if(fsm) TTF_SizeUTF8(fsm,"WiFi : ",&lw,&lh);

            // URL in styled box with gap from tab bar
            int urlY = contentY + 16;
            int urlBoxH = 0;
            {
                TTF_Font* fmd = GetFont(Font::Md);
                int urlTW=0, urlTH=0;
                if (fmd) TTF_SizeUTF8(fmd, url.c_str(), &urlTW, &urlTH);
                int bpx=18, bpy=8;
                int urlBoxW = urlTW + bpx*2;
                urlBoxH = urlTH + bpy*2;
                int urlBoxX = rightX + (rightW - urlBoxW) / 2;
                FillRect(urlBoxX, urlY, urlBoxW, urlBoxH, {18, 18, 38, 255});
                DrawRect(urlBoxX, urlY, urlBoxW, urlBoxH, COL_GOLD);
                DrawText(url, urlBoxX + bpx, urlY + bpy, COL_GOLD, Font::Md);
            }

            // WiFi status below URL box with comfortable gap
            int statusY = urlY + urlBoxH + 16;
            int iw=0;
            if(fsm) TTF_SizeUTF8(fsm,wifiActive?"Active":"Inactive",&iw,&lh);
            int totalStatusW=lw+iw;
            int sxBase=rightX+(rightW-totalStatusW)/2;
            DrawText("WiFi : ", sxBase, statusY, COL_DIM);
            DrawText("Active", sxBase+lw, statusY, COL_GREEN);

            // QR code below WiFi status
            QR::Mat mat;
            if (QR::build(url, mat)) {
                int qrTop = statusY + lh + 14;
                int qrAvailW = rightW;
                int qrAvailH = contentY + contentH - qrTop;
                int ms = std::min(qrAvailW, qrAvailH) / (mat.N+4);
                if (ms<3) ms=3;
                int border=2;
                int qrSize=mat.N*ms+border*2*ms;
                int qrX=rightX+(rightW-qrSize)/2;
                FillRect(qrX,qrTop,qrSize,qrSize,{255,255,255,255});
                SDL_SetRenderDrawColor(gRen,0,0,0,255);
                for (int y=0;y<mat.N;y++) for (int x=0;x<mat.N;x++) {
                    if (mat.get(x,y)) {
                        SDL_Rect r{qrX+(x+border)*ms, qrTop+(y+border)*ms, ms, ms};
                        SDL_RenderFillRect(gRen,&r);
                    }
                }
            }
        } else {
            // No wifi — just show status centered
            TTF_Font* fsm=GetFont(Font::Sm); int lw=0,lh=0;
            if(fsm) TTF_SizeUTF8(fsm,"WiFi : ",&lw,&lh);
            int cy2 = contentY+contentH/2-lh;
            DrawText("WiFi : ",  rightX+(rightW-lw)/2-20, cy2, COL_DIM);
            DrawText("Inactive", rightX+(rightW-lw)/2-20+lw, cy2, COL_RED);
        }

        DrawFooter("L/R  switch tab    B  back    +  quit");
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

// ─── Touch input ──────────────────────────────────────────────────────────────

static struct {
    bool         active   = false;
    bool         dragging = false;
    float        startX   = 0.f, startY = 0.f;
    float        accumX   = 0.f, accumY = 0.f;
    SDL_FingerID fid      = 0;
} s_touch;

static void ApplyTouchScroll(float ddx, float ddy) {
    if (gScreen == Screen::UserPick) {
        s_touch.accumX += ddx;
        while (s_touch.accumX >=  100.f && gUserSel > 0)
            { s_touch.accumX -= 100.f; gUserSel--; }
        while (s_touch.accumX <= -100.f && gUserSel < (int)gUsers.size()-1)
            { s_touch.accumX += 100.f; gUserSel++; }
        return;
    }
    if (gScreen != Screen::OnSwitch) return;

    if (gShowFileBrowser) {
        int itemTop = gBrowseForExportDir ? 106 : 88;
        if (s_touch.startY < itemTop || s_touch.startY >= SCREEN_H-30) return;
        const int ph   = 26;
        const int pvis = (SCREEN_H - (gBrowseForExportDir ? 166 : 148)) / ph;
        s_touch.accumY += ddy;
        while (s_touch.accumY >=  ph) { s_touch.accumY -= ph; gBrowseScroll = std::max(0, gBrowseScroll-1); }
        while (s_touch.accumY <= -ph) { s_touch.accumY += ph; gBrowseScroll = std::min(gBrowseScroll+1, std::max(0,(int)gBrowseEntries.size()-pvis)); }
        return;
    }

    if (gOnSwitchMode == OnSwitchMode::UGC && !gEntries.empty()) {
        if (s_touch.startX < LIST_X || s_touch.startX >= LIST_X+LIST_W) return;
        if (s_touch.startY < LIST_Y || s_touch.startY >= LIST_Y+LIST_H) return;
        s_touch.accumY += ddy;
        while (s_touch.accumY >=  ITEM_H) { s_touch.accumY -= ITEM_H; gEntryScroll = std::max(0, gEntryScroll-1); }
        while (s_touch.accumY <= -ITEM_H) { s_touch.accumY += ITEM_H; gEntryScroll = std::min(gEntryScroll+1, std::max(0,(int)gEntries.size()-VISIBLE)); }
    }
    else if (gOnSwitchMode == OnSwitchMode::Mii && !gMiis.empty()) {
        if (s_touch.startX < LIST_X || s_touch.startX >= LIST_X+LIST_W) return;
        if (s_touch.startY < LIST_Y || s_touch.startY >= LIST_Y+LIST_H) return;
        s_touch.accumY += ddy;
        while (s_touch.accumY >=  ITEM_H) { s_touch.accumY -= ITEM_H; gMiiScroll = std::max(0, gMiiScroll-1); }
        while (s_touch.accumY <= -ITEM_H) { s_touch.accumY += ITEM_H; gMiiScroll = std::min(gMiiScroll+1, std::max(0,(int)gMiis.size()-VISIBLE)); }
    }
}

static void ProcessTouch(int tx, int ty, u64& kDown) {
    auto hit = [&](int x, int y, int w, int h) -> bool {
        return tx>=x && tx<x+w && ty>=y && ty<y+h;
    };

    if (gScreen == Screen::UserPick) {
        int n = (int)gUsers.size();
        if (n == 0) return;
        const int GAP     = 24;
        const int MAX_VIS = std::min(n, 5);
        const int CARD_W  = std::min(220, (SCREEN_W - 120 - (MAX_VIS-1)*GAP) / MAX_VIS);
        const int CARD_H  = (CARD_W - 30) + 66;
        int scroll = 0;
        if (n > MAX_VIS) { scroll = gUserSel - MAX_VIS/2; scroll = std::max(0, std::min(scroll, n-MAX_VIS)); }
        int startX = (SCREEN_W - (MAX_VIS*CARD_W + (MAX_VIS-1)*GAP)) / 2;
        int startY = 68 + (SCREEN_H - 36 - 68 - CARD_H) / 2;
        for (int i = 0; i < MAX_VIS; i++) {
            int idx = scroll + i;
            if (idx >= n) break;
            if (hit(startX + i*(CARD_W+GAP), startY, CARD_W, CARD_H)) {
                gUserSel = idx;
                kDown |= HidNpadButton_A;
                break;
            }
        }
        return;
    }

    if (gScreen == Screen::BackupPrompt) {
        const int cw=300, ch=148, gap=20;
        const int bx = SCREEN_W/2 - (3*cw + 2*gap)/2;
        const int by = SCREEN_H/2 - ch/2 + 10;
        if      (hit(bx,            by, cw, ch)) kDown |= HidNpadButton_A;
        else if (hit(bx+cw+gap,     by, cw, ch)) kDown |= HidNpadButton_B;
        else if (hit(bx+(cw+gap)*2, by, cw, ch)) kDown |= HidNpadButton_X;
        return;
    }

    if (gScreen != Screen::OnSwitch) return;

    if (gShowFileBrowser) {
        int itemTop = gBrowseForExportDir ? 106 : 88;
        // Tap header area (above items) to cancel
        if (ty >= 36 && ty < itemTop) { kDown |= HidNpadButton_X; return; }
        // Tap footer to go up one directory level
        if (ty >= SCREEN_H-30) { kDown |= HidNpadButton_B; return; }
        // Tap an item to select and activate it
        int ph = 26;
        int pvis = (SCREEN_H - (gBrowseForExportDir ? 166 : 148)) / ph;
        for (int i = 0; i < pvis; i++) {
            int idx = gBrowseScroll + i;
            if (idx >= (int)gBrowseEntries.size()) break;
            if (hit(52, itemTop + i*ph, SCREEN_W-104, ph)) {
                gBrowseSel = idx;
                kDown |= HidNpadButton_A;
                break;
            }
        }
        return;
    }

    // Tab bar — jump directly to the tapped tab
    {
        const int tw=180, gap=4, tabY=14, tabH=42;
        const int tabX0 = SCREEN_W/2 - (tw*3+gap*2)/2;
        if (hit(tabX0,           tabY, tw, tabH)) {
            gOnSwitchMode = OnSwitchMode::UGC;   gOnSwitchMsg = "";
        } else if (hit(tabX0+tw+gap,     tabY, tw, tabH)) {
            if (gMiis.empty()) gMiis = MiiManager::ListMiis();
            gOnSwitchMode = OnSwitchMode::Mii;   gOnSwitchMsg = "";
        } else if (hit(tabX0+(tw+gap)*2, tabY, tw, tabH)) {
            gOnSwitchMode = OnSwitchMode::WebUI; gOnSwitchMsg = "";
        }
    }

    if (gOnSwitchMode == OnSwitchMode::UGC) {
        // List items — tap to select, loads preview
        for (int i = 0; i < VISIBLE; i++) {
            int idx = gEntryScroll + i;
            if (idx >= (int)gEntries.size()) break;
            if (hit(LIST_X, LIST_Y+LIST_PAD_TOP+i*ITEM_H, LIST_W, ITEM_H)) {
                if (gEntrySel != idx) { gEntrySel = idx; LoadPreview(gEntries[idx]); }
                break;
            }
        }
        // Import / Export buttons (coordinates must match the draw code)
        if (!gEntries.empty()) {
            const int kBtnH=46, kBtnGap=8, kBtnSlot=kBtnH+kBtnGap+4;
            int imgY = PREVIEW_Y + 28;
            int imgH = PREVIEW_H - 28 - kBtnSlot;
            int cx   = PREVIEW_X + PREVIEW_W/2;
            int btnY = imgY + imgH + kBtnGap;
            const int btnW=220, btnGap=24;
            if (hit(cx-btnW-btnGap/2, btnY, btnW, kBtnH)) kDown |= HidNpadButton_A;
            if (hit(cx+btnGap/2,      btnY, btnW, kBtnH)) kDown |= HidNpadButton_Y;
        }
    }
    else if (gOnSwitchMode == OnSwitchMode::Mii) {
        // List items — tap to select
        for (int i = 0; i < VISIBLE; i++) {
            int idx = gMiiScroll + i;
            if (idx >= (int)gMiis.size()) break;
            if (hit(LIST_X, LIST_Y+LIST_PAD_TOP+i*ITEM_H, LIST_W, ITEM_H)) {
                gMiiSel = idx;
                break;
            }
        }
        // Import / Export styled buttons (coordinates match the drawn buttons)
        if (!gMiis.empty()) {
            int cx = PREVIEW_X + PREVIEW_W/2;
            int cy = PREVIEW_Y + PREVIEW_H/2;
            const int btnW=220, btnH=46, btnGap=24;
            if (hit(cx-btnW-btnGap/2, cy+30, btnW, btnH)) kDown |= HidNpadButton_A;
            if (hit(cx+btnGap/2,      cy+30, btnW, btnH)) kDown |= HidNpadButton_Y;
        }
    }
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
    // Two-step rename so exFAT (case-insensitive) picks up the capital E
    rename("/switch/TomoToolNX/exports", "/switch/TomoToolNX/__exports_tmp__");
    rename("/switch/TomoToolNX/__exports_tmp__", "/switch/TomoToolNX/Exports");
    mkdir("/switch/TomoToolNX/Exports", 0777);
    LoadConfig();
    mkdir(gExportPath.c_str(), 0777);

    // Auto-check for updates only if WiFi is active — no prompt
    {
        std::string ip = SaveMount::GetLocalIP();
        if (!ip.empty()) {
            Updater::StartCheck();
            gScreen = Screen::UpdateCheck;
        } else {
            gScreen = Screen::UserPick;
        }
    }

    gUsers=SaveMount::GetUsers();
    if (gUsers.empty()){
        gError="No user accounts found.";
        gScreen=Screen::Error;
    }

    // Load avatar textures from JPEG data
    for (auto& u : gUsers) {
        SDL_Texture* tex = nullptr;
        if (!u.avatarJpeg.empty()) {
            SDL_RWops* rw = SDL_RWFromConstMem(u.avatarJpeg.data(), (int)u.avatarJpeg.size());
            if (rw) {
                SDL_Surface* surf = IMG_Load_RW(rw, 1);
                if (surf) { tex = SDL_CreateTextureFromSurface(gRen, surf); SDL_FreeSurface(surf); }
            }
        }
        gAvatarTextures.push_back(tex);
    }

    bool running = true; (void)running;

    while (appletMainLoop()) {
        padUpdate(&gPad);
        u64 kDown=padGetButtonsDown(&gPad);
        u64 kHeld=padGetButtons(&gPad);
        {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_FINGERDOWN && !s_touch.active) {
                    s_touch = { true, false,
                                ev.tfinger.x*SCREEN_W, ev.tfinger.y*SCREEN_H,
                                0.f, 0.f, ev.tfinger.fingerId };
                }
                else if (ev.type == SDL_FINGERMOTION && s_touch.active
                         && ev.tfinger.fingerId == s_touch.fid) {
                    float cx = ev.tfinger.x * SCREEN_W;
                    float cy = ev.tfinger.y * SCREEN_H;
                    float d2 = (cx-s_touch.startX)*(cx-s_touch.startX)
                             + (cy-s_touch.startY)*(cy-s_touch.startY);
                    if (!s_touch.dragging && d2 > 144.f) // 12 px radius
                        s_touch.dragging = true;
                    if (s_touch.dragging)
                        ApplyTouchScroll(ev.tfinger.dx * SCREEN_W,
                                         ev.tfinger.dy * SCREEN_H);
                }
                else if (ev.type == SDL_FINGERUP && s_touch.active
                         && ev.tfinger.fingerId == s_touch.fid) {
                    if (!s_touch.dragging)
                        ProcessTouch((int)s_touch.startX, (int)s_touch.startY, kDown);
                    s_touch.active = false;
                }
            }
        }
        u64 kNav =NavRepeat(kDown,kHeld);

        if (kDown&HidNpadButton_Plus) break;

        switch (gScreen) {

        case Screen::UpdateCheck:
            // Auto-check: started at launch only if WiFi is active
            {
                auto state = Updater::GetState();
                if (state==Updater::State::UpdateAvailable) gScreen=Screen::UpdateAvailable;
                else if (state==Updater::State::NoUpdate||state==Updater::State::Error||state==Updater::State::Idle)
                    gScreen=Screen::UserPick;
            }
            if (kDown&HidNpadButton_B) gScreen=Screen::UserPick;
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
            if (kNav&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft))   { gUserSel = (gUserSel>0) ? gUserSel-1 : (int)gUsers.size()-1; }
            if (kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight)) { gUserSel = (gUserSel+1<(int)gUsers.size()) ? gUserSel+1 : 0; }
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
                gScreen=Screen::OnSwitch;
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
                gScreen=Screen::OnSwitch;
            }
            DrawBackingUp();
            break;

        case Screen::Mounting:
            DrawMounting();
            // Init happens in mounting thread — once save is mounted we go to OnSwitch
            // and start both HTTP server and scan entries
            break;

        case Screen::OnSwitch:
            // On first entry: start HTTP server, scan UGC and Miis
            if (gEntries.empty() && gMiis.empty()) {
                gIP=SaveMount::GetLocalIP();
                if (gIP.empty()) gIP="?.?.?.?";
                HttpServer::Start(HTTP_PORT, SAVE_UGC_PATH);
                gEntries=UgcScanner::Scan(SAVE_UGC_PATH);
                MiiManager::LoadUgcNames();
                gMiis=MiiManager::ListMiis();
                gEntrySel=0; gEntryScroll=0;
                gOnSwitchMsg="";
                if (!gEntries.empty()) LoadPreview(gEntries[0]);
            }
            // HTTP server events — process regardless of active tab
            {
                std::vector<HttpServer::LogEntry> httpLogs;
                HttpServer::DrainLog(httpLogs);
                for (auto& hl:httpLogs) Log(hl.text, hl.isError?COL_RED:COL_GOLD);
            }
            if (HttpServer::HasPendingImport()) {
                auto job=HttpServer::TakePendingImport();
                std::string importErr=TextureProcessor::ImportPng(job.opts);
                remove(job.tmpPath.c_str());
                HttpServer::FinishImport(importErr);
                if (!importErr.empty()) LogERR("Import: "+importErr);
                else { LogOK("Import OK"); gEntries=UgcScanner::Scan(SAVE_UGC_PATH);
                       MiiManager::LoadUgcNames();
                       if (!gEntries.empty()) LoadPreview(gEntries[gEntrySel < (int)gEntries.size() ? gEntrySel : 0]); }
            }
            if (HttpServer::HasPendingCommit()) {
                LogINF("Committing save...");
                HttpServer::ClearPendingCommit();
                std::string cerr=SaveMount::Commit();
                if (cerr.empty()) LogOK("Saved"); else LogERR("Commit: "+cerr);
            }
            // File browser takes priority
            if (gShowFileBrowser) {
                int pvis=(SCREEN_H-148)/26;
                if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp)){
                    if(gBrowseSel>0){gBrowseSel--;if(gBrowseSel<gBrowseScroll)gBrowseScroll=gBrowseSel;}
                }
                if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown)){
                    if(gBrowseSel+1<(int)gBrowseEntries.size()){
                        gBrowseSel++;
                        if(gBrowseSel>=gBrowseScroll+pvis)gBrowseScroll=gBrowseSel-pvis+1;
                    }
                }
                if (kDown&HidNpadButton_A && !gBrowseEntries.empty()){
                    auto& entry=gBrowseEntries[gBrowseSel];
                    if (entry.isDir){
                        std::string newPath;
                        if (entry.name=="[..]"){
                            size_t pos=gBrowseCurPath.rfind('/');
                            newPath=(pos==0)?"/":gBrowseCurPath.substr(0,pos);
                        } else {
                            newPath=gBrowseCurPath+"/"+entry.name;
                        }
                        BrowseRefresh(newPath);
                    } else if (!gBrowseForExportDir) {
                        std::string fullPath=gBrowseCurPath+"/"+entry.name;
                        if (gBrowseForMii) gBrowseLtdPath=gBrowseCurPath;
                        else gBrowsePngPath=gBrowseCurPath;
                        gShowFileBrowser=false;
                        if (gBrowseForMii) DoMiiImport(fullPath);
                        else DoOnSwitchImport(fullPath);
                    }
                }
                if (kDown&HidNpadButton_B && gBrowseCurPath!="/"){
                    size_t pos=gBrowseCurPath.rfind('/');
                    BrowseRefresh((pos==0)?"/":gBrowseCurPath.substr(0,pos));
                }
                if (kDown&HidNpadButton_Y && gBrowseForExportDir) {
                    gExportPath = gBrowseCurPath;
                    mkdir(gExportPath.c_str(), 0777);
                    SaveConfig();
                    gShowFileBrowser = false;
                    gOnSwitchMsg = "Export folder set to: "+gExportPath;
                    gOnSwitchMsgCol = COL_GREEN;
                }
                if (kDown&HidNpadButton_X) {
                    if (gBrowseForExportDir) {
                        gShowFileBrowser = false;
                    } else if (gBrowseForMii) {
                        gBrowseLtdPath=gBrowseCurPath;
                        gShowFileBrowser=false;
                    } else {
                        gBrowsePngPath=gBrowseCurPath;
                        gShowFileBrowser=false;
                    }
                }
            } else if (gOnSwitchMode == OnSwitchMode::UGC) {
                // Tab switch
                if (kDown&(HidNpadButton_R|HidNpadButton_ZR)){
                    gOnSwitchMode=OnSwitchMode::Mii;
                    if(gMiis.empty()) gMiis=MiiManager::ListMiis();
                    gOnSwitchMsg=""; break;
                }
                if (kDown&(HidNpadButton_L|HidNpadButton_ZL)){
                    gOnSwitchMode=OnSwitchMode::WebUI; gOnSwitchMsg=""; break;
                }
                if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp)){
                    if (!gEntries.empty()) {
                        gEntrySel = (gEntrySel>0) ? gEntrySel-1 : (int)gEntries.size()-1;
                        if(gEntrySel<gEntryScroll) gEntryScroll=gEntrySel;
                        if(gEntrySel>=(int)gEntries.size()-1) gEntryScroll=std::max(0,(int)gEntries.size()-VISIBLE);
                        LoadPreview(gEntries[gEntrySel]);
                    }
                }
                if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown)){
                    if (!gEntries.empty()) {
                        gEntrySel = (gEntrySel+1<(int)gEntries.size()) ? gEntrySel+1 : 0;
                        if(gEntrySel>=gEntryScroll+VISIBLE) gEntryScroll=gEntrySel-VISIBLE+1;
                        if(gEntrySel==0) gEntryScroll=0;
                        LoadPreview(gEntries[gEntrySel]);
                    }
                }
                if (kDown&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft) && !gEntries.empty()){
                    gEntrySel=0; gEntryScroll=0; LoadPreview(gEntries[0]);
                }
                if (kDown&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight) && !gEntries.empty()){
                    gEntrySel=(int)gEntries.size()-1;
                    gEntryScroll=std::max(0,(int)gEntries.size()-VISIBLE);
                    LoadPreview(gEntries[gEntrySel]);
                }
                if (kDown&HidNpadButton_Minus){
                    gBrowseForExportDir=true; gBrowseForMii=false;
                    struct stat _st; std::string _sp=gExportPath;
                    if (stat(_sp.c_str(),&_st)!=0||!S_ISDIR(_st.st_mode)){
                        size_t _p=_sp.rfind('/'); _sp=(_p&&_p!=std::string::npos)?_sp.substr(0,_p):"/";
                    }
                    BrowseRefresh(_sp); gShowFileBrowser=true;
                }
                if (kDown&HidNpadButton_A){
                    gBrowseForMii=false;
                    BrowseRefresh(gBrowsePngPath);
                    gShowFileBrowser=true;
                }
                if (kDown&HidNpadButton_Y && !gEntries.empty()){
                    const auto& e=gEntries[gEntrySel];
                    LogINF("Exporting "+e.stem+"...");
                    RgbaImage img;
                    std::string err=TextureProcessor::DecodeFile(e.ugctexPath,img,true);
                    if (!err.empty()){gOnSwitchMsg=err;gOnSwitchMsgCol=COL_RED;LogERR("Export failed: "+err);}
                    else {
                        std::string outPath=gExportPath+"/"+e.stem+".png";
                        SDL_Surface* surf=SDL_CreateRGBSurfaceWithFormatFrom(
                            img.pixels.data(),img.width,img.height,32,img.width*4,
                            SDL_PIXELFORMAT_RGBA32);
                        if(surf){IMG_SavePNG(surf,outPath.c_str());SDL_FreeSurface(surf);
                            gOnSwitchMsg="Exported to: "+outPath;gOnSwitchMsgCol=COL_GREEN;
                            LogOK("Exported to: "+outPath);}
                    }
                }
                if (kDown&HidNpadButton_B){
                    FreePreview(); HttpServer::Stop(); gLog.clear();
                    gEntries.clear(); gMiis.clear();
                    gScreen=Screen::UserPick; SaveMount::Unmount();
                }
            } else if (gOnSwitchMode == OnSwitchMode::Mii) {
                // Tab switch
                if (kDown&(HidNpadButton_L|HidNpadButton_ZL)){
                    gOnSwitchMode=OnSwitchMode::UGC; gOnSwitchMsg=""; break;
                }
                if (kDown&(HidNpadButton_R|HidNpadButton_ZR)){
                    gOnSwitchMode=OnSwitchMode::WebUI; gOnSwitchMsg=""; break;
                }
                if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp)){
                    if (!gMiis.empty()) {
                        gMiiSel = (gMiiSel>0) ? gMiiSel-1 : (int)gMiis.size()-1;
                        if(gMiiSel<gMiiScroll) gMiiScroll=gMiiSel;
                        if(gMiiSel>=(int)gMiis.size()-1) gMiiScroll=std::max(0,(int)gMiis.size()-VISIBLE);
                    }
                }
                if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown)){
                    if (!gMiis.empty()) {
                        gMiiSel = (gMiiSel+1<(int)gMiis.size()) ? gMiiSel+1 : 0;
                        if(gMiiSel>=gMiiScroll+VISIBLE) gMiiScroll=gMiiSel-VISIBLE+1;
                        if(gMiiSel==0) gMiiScroll=0;
                    }
                }
                if (kDown&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft) && !gMiis.empty()){
                    gMiiSel=0; gMiiScroll=0;
                }
                if (kDown&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight) && !gMiis.empty()){
                    gMiiSel=(int)gMiis.size()-1;
                    gMiiScroll=std::max(0,(int)gMiis.size()-VISIBLE);
                }
                if (kDown&HidNpadButton_Minus){
                    gBrowseForExportDir=true; gBrowseForMii=false;
                    struct stat _st; std::string _sp=gExportPath;
                    if (stat(_sp.c_str(),&_st)!=0||!S_ISDIR(_st.st_mode)){
                        size_t _p=_sp.rfind('/'); _sp=(_p&&_p!=std::string::npos)?_sp.substr(0,_p):"/";
                    }
                    BrowseRefresh(_sp); gShowFileBrowser=true;
                }
                if (kDown&HidNpadButton_A && !gMiis.empty()){
                    gBrowseForMii=true;
                    BrowseRefresh(gBrowseLtdPath);
                    gShowFileBrowser=true;
                }
                if (kDown&HidNpadButton_Y && !gMiis.empty()){
                    int slot=gMiis[gMiiSel].slot;
                    std::string fname=gMiis[gMiiSel].name+"_slot"+std::to_string(slot)+".ltd";
                    for(auto& c:fname){
                        if(c==' '||c=='/'||c=='\\'||c==':'||c=='*'||c=='?'||
                           c=='"'||c=='<'||c=='>'||c=='|'||(uint8_t)c>127)
                            c='_';
                    }
                    std::string outPath=gExportPath+"/"+fname;
                    LogINF("Exporting Mii slot "+std::to_string(slot)+"...");
                    std::string err=MiiManager::ExportMii(slot,outPath);
                    if(!err.empty()){gOnSwitchMsg=err;gOnSwitchMsgCol=COL_RED;LogERR("Mii export failed: "+err);}
                    else{gOnSwitchMsg="Exported to: "+outPath;gOnSwitchMsgCol=COL_GREEN;LogOK("Exported to: "+outPath);}
                }
                if (kDown&HidNpadButton_B){
                    FreePreview(); HttpServer::Stop(); gLog.clear();
                    gEntries.clear(); gMiis.clear();
                    gScreen=Screen::UserPick; SaveMount::Unmount();
                }
            } else { // WebUI tab
                // Tab switch
                if (kDown&(HidNpadButton_L|HidNpadButton_ZL)){
                    gOnSwitchMode=OnSwitchMode::Mii; gOnSwitchMsg=""; break;
                }
                if (kDown&(HidNpadButton_R|HidNpadButton_ZR)){
                    gOnSwitchMode=OnSwitchMode::UGC; gOnSwitchMsg=""; break;
                }
                if (kDown&HidNpadButton_B){
                    FreePreview(); HttpServer::Stop(); gLog.clear();
                    gEntries.clear(); gMiis.clear();
                    gScreen=Screen::UserPick; SaveMount::Unmount();
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
    BackupService::Cleanup();
    SaveMount::Unmount();
    Updater::Cleanup();
    for (auto tex : gAvatarTextures) if (tex) SDL_DestroyTexture(tex);
    gAvatarTextures.clear();
    FontQuit();
    nifmExit();
    socketExit();
    SDL_DestroyRenderer(gRen);
    SDL_DestroyWindow(win);
    IMG_Quit();
    SDL_Quit();
    return 0;
}
