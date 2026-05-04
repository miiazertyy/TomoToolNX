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

#include "save_editor.h"
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

enum class Screen { UpdateCheck, UpdateAvailable, Downloading, UserPick, BackupPrompt, BackingUp, Mounting, OnSwitch, SaveFeedback, Error };

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

enum class OnSwitchMode { UGC, Mii, Player, MiiStats, WebUI };
static OnSwitchMode gOnSwitchMode = OnSwitchMode::WebUI;

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

// ─── Touch / hitbox system ────────────────────────────────────────────────────
// Function pointers avoid std::function overhead under -fno-exceptions/-fno-rtti.
typedef void (*TouchFn)(int);
struct TouchHit { int x,y,w,h; TouchFn fn; int param; };
static TouchHit gHits[160];
static int      gHitCount = 0;
static u64      gSimKDown = 0;
static int      gTouchScale = 1; // multiplier for nudge actions

static void HitClear() { gHitCount = 0; }
static void HitAdd(int x,int y,int w,int h, TouchFn fn, int param=0) {
    if (gHitCount < 160) gHits[gHitCount++] = {x,y,w,h,fn,param};
}
static void HitFire(int tx, int ty) {
    for (int i = 0; i < gHitCount; i++) {
        auto& h = gHits[i];
        if (tx>=h.x && tx<h.x+h.w && ty>=h.y && ty<h.y+h.h) { h.fn(h.param); break; }
    }
}

// Touch handler helpers
static void TSimBtn (int btn)   { gSimKDown |= (u64)(uint32_t)btn; }
static void TSetMode(int mode)  { gOnSwitchMode = (OnSwitchMode)mode; gOnSwitchMsg = ""; }
// Defined after their target globals are declared (forward refs below via lambdas are fine as fn ptrs)
static void TSelUser (int idx);
static void TSelUgc  (int idx);
static void TSelMii  (int idx);
static void TSelBrowse(int idx);
static void TAdjust  (int delta); // encoded: negative=left, positive=right; |val| = scale
static void TSelPlayerField(int idx);
static void TSetSkinTone(int idx);
static void TSetIslandSize(int val);
static void TSelMiiStatsMii(int idx);
static void TSelMiiStatsField(int idx);
static void TSelMiiWordsSlot(int idx);
static void TSelMiiRelation(int idx);
static void TRelMeterAdj(int delta);
static void TSetSocialView(int v);
static void TAckSaveWarning(int);
static void TPlayerKbd(int);
static void TMiiStatsKbd(int);
static void DrawSaveWarning();
static void TUndoPlayer(int);
static void TUndoMii(int);
static void DrawBackPrompt();
static void TAckBackYes(int);
static void TAckBackNo(int);
static void TDismissThumbTip(int);
static void DrawThumbTip();
static void SaveConfig();

// ─── Save editor state ────────────────────────────────────────────────────────
static SaveEditor::SavFile gPlayerSav, gMiiSav;
static bool   gPlayerSavDirty   = false, gMiiSavDirty = false;
static int    gPlayerFieldSel   = 0;
static int    gPlayerScroll     = 0;
static int    gMiiStatsMiiSel   = 0; // index into gMiis
static int    gMiiStatsFieldSel = 0;
static int    gMiiStatsScroll   = 0;
static std::string gPlayerMsg, gMiiStatsMsg;
static SDL_Color   gPlayerMsgCol = COL_TEXT, gMiiStatsMsgCol = COL_TEXT;

// Undo state (single-step undo per field)
static int32_t gPlayerUndoVal[16]   = {};
static bool    gPlayerUndoValid[16] = {};
static int32_t gMiiUndoVal[16]     = {};
static bool    gMiiUndoValid[16]   = {};

// Button focus (joystick navigation of +/- buttons)
static int  gPlayerBtnSel   = 3;     // 0-7 for 8 Player numeric buttons
static int  gMiiBtnSel      = 2;     // 0-5 for 6 MiiStats numeric buttons
static bool gBtnTouchActive = false; // set by TAdjust (touch), cleared after use

// Back-prompt state
static bool gShowBackPrompt  = false;
static bool gBackPromptIsMii = false;
static bool gQuitApp         = false;

// Save-feedback state (brief "Saved!" screen after B or + save)
static int  gSaveFeedbackFrames = 0;
static bool gSaveFeedbackQuit   = false;

// Thumb-tip modal (shown once after first successful UGC import)
static bool gShowThumbTip      = false;
static int  gThumbTipCountdown = 0;  // frames remaining before button enables
static bool gThumbTipSeen      = false;

// ─── Player field table ───────────────────────────────────────────────────────
struct SaveFieldDef {
    const char* label;
    const char* fieldName;  // "" when rawHash is used
    uint32_t    rawHash;    // 0 = hash fieldName; nonzero = use directly
    bool isStr;      // WStr32 keyboard edit
    bool isUInt;     // UInt nudge (GetUInt/SetUInt)
    bool isEnum;     // Currency enum cycle
    bool isInt;      // Signed int nudge (GetAnyScalar/SetAnyScalar)
    bool isSkinTone; // UInt 0-5 swatch buttons
    bool isIslandSz; // Any-scalar, 4 safe preset buttons (values 1-4)
    bool isRegion;   // Named region enum cycle (7 values)
    bool isRawEnum;  // Raw unsigned nudge (GetAnyScalar/SetAnyScalar)
    int32_t minVal, maxVal; // -1 = unclamped
    const char* desc;
};
static uint32_t PFHash(const SaveFieldDef& fd) {
    return fd.rawHash ? fd.rawHash : SaveEditor::Hash(fd.fieldName);
}
static const struct { const char* name; const char* label; } REGION_OPTIONS[] = {
    {"NorthAmerica",  "North America"},
    {"SouthAmericaN", "S.America (N)"},
    {"SouthAmericaS", "S.America (S)"},
    {"Australia",     "Australia/NZ"},
    {"Asia",          "HK/TW/Korea"},
    {"OthersN",       "Other (N)"},
    {"OthersS",       "Other (S)"},
};
static const int REGION_OPTION_COUNT = 7;
//                    label           fieldName                              rawHash      Str    UInt   Enum   Int    Skin   Island Region RawEn  min  max  desc
static const SaveFieldDef PLAYER_FIELDS[] = {
    {"Name",          "Player.Name",                           0,           true,  false, false, false, false, false, false, false, 0,   0,   "Your player's display name."},
    {"Island",        "Player.IslandName",                     0,           true,  false, false, false, false, false, false, false, 0,   0,   "The name of your island."},
    {"Money",         "Player.Money",                          0,           false, true,  false, false, false, false, false, false, 0,   -1,  "Current coin balance."},
    {"Currency",      "Player.Currency",                       0,           false, false, true,  false, false, false, false, false, 0,   -1,  "Regional currency symbol shown in-game."},
    {"Boot Count",    "Player.BootNum",                        0,           false, true,  false, false, false, false, false, false, 0,   -1,  "Number of times the game has been launched."},
    {"Skin Tone",     "Player.SkinColorIndex",                 0,           false, false, false, false, true,  false, false, false, 0,   5,   "Player hand/skin color (0-5)."},
    {"Bday Day",      "",                                      0xdb7786bbu, false, false, false, false, false, false, false, true,  1,   31,  "Birthday day of the month (1-31)."},
    {"Bday Month",    "",                                      0xc754bef3u, false, false, false, false, false, false, false, true,  1,   12,  "Birthday month (1-12)."},
    {"Bday Year",     "",                                      0x11996629u, false, false, false, true,  false, false, false, false, -1,  -1,  "Birthday year."},
    {"Island Size",   "",                                      0x870a807cu, false, false, false, false, false, true,  false, false, 1,   4,   "Island size preset. Other values corrupt the save!"},
    {"Fountain Lv",   "Liberation.FountainLevel",              0,           false, false, false, false, false, false, false, true,  0,   -1,  "Wishing fountain upgrade level."},
    {"Wishes",        "",                                      0xa32f7e47u, false, false, false, false, false, false, false, true,  0,   -1,  "Number of wishes made."},
    {"Region",        "Player.Region",                         0,           false, false, false, false, false, false, true,  false, 0,   -1,  "Player region (affects seasons and weather)."},
    {"Region Code",   "Player.RegionCode",                     0,           false, false, false, false, false, false, false, true,  0,   -1,  "Numeric region code."},
    {"Name Lang",     "Player.NameRegionLanguageID",           0,           false, false, false, false, false, false, false, true,  0,   -1,  "Language ID for player name pronunciation."},
    {"Island Lang",   "Player.IslandNameRegionLanguageID",     0,           false, false, false, false, false, false, false, true,  0,   -1,  "Language ID for island name pronunciation."},
};
static const int PLAYER_FIELD_COUNT = 16;

// ─── Mii stats field table ─────────────────────────────────────────────────────
struct MiiStatsDef {
    const char* label;
    const char* fieldName;
    bool isStr;   // WStr32Array
    bool isUInt;  // UIntArray; otherwise IntArray or EnumArray
    bool isEnum;  // EnumArray
    int dispOffset; // add when displaying (level is stored 0-based, shown +1)
    int32_t minVal, maxVal;
    const char* desc;
};
static const MiiStatsDef MII_STATS_FIELDS[] = {
    {"Name",        "Mii.Name.Name",                       true, false,false, 0,  0,  0, "The Mii's in-game display name."},
    {"Level",       "Mii.MiiMisc.SatisfyInfo.Level",      false,false,false, 1,  0, -1, "Friendship level with the player."},
    {"Lv Meter",    "Mii.MiiMisc.SatisfyInfo.Meter",      false,false,false, 0,  0,100, "Progress bar toward the next level (0-100)."},
    {"Money",       "Mii.Belongings.Money",                false,true, false, 0,  0, -1, "Coins currently held by this Mii."},
    {"Bday Day",    "Mii.MiiMisc.BirthdayInfo.Day",       false,false,false, 0,  1, 31, "Birthday day of the month (1-31)."},
    {"Bday Month",  "Mii.MiiMisc.BirthdayInfo.Month",     false,false,false, 0,  1, 12, "Birthday month (1-12)."},
    {"Birth Year",  "Mii.MiiMisc.BirthdayInfo.Year",      false,false,false, 0, -1, -1, "Year of birth."},
    {"Direct Age",  "Mii.MiiMisc.BirthdayInfo.DirectAge", false,false,false, 0, -1, -1, "Age override. -1 means auto (from birthday)."},
    {"Activeness",  "Mii.CharacterParam.Activeness",       false,false,false, 0,  1, 10, "How active and energetic the Mii is (1-10)."},
    {"Audacity",    "Mii.CharacterParam.Audaciousness",    false,false,false, 0,  1, 10, "How bold and daring the Mii is (1-10)."},
    {"Commonsense", "Mii.CharacterParam.Commonsense",      false,false,false, 0,  1, 10, "How wise and sensible the Mii is (1-10)."},
    {"Gaiety",      "Mii.CharacterParam.Gaiety",           false,false,false, 0,  1, 10, "How cheerful and upbeat the Mii is (1-10)."},
    {"Sociability", "Mii.CharacterParam.Sociability",      false,false,false, 0,  1, 10, "How sociable and outgoing the Mii is (1-10)."},
    {"Bond Meter",  "Mii.MiiMisc.BondInfo.Meter",         false,false,false, 0,  0,100, "Strength of the bond with the player (0-100)."},
    {"Mood",        "Mii.Feeling.Type",                    false,false,true,  0,  0, -1, "Current emotional state."},
    {"Fullness",    "Mii.MiiMisc.EatInfo.EatFullness",     false,false,false, 0,  0, -1, "How well-fed the Mii is. Can exceed 100 with certain items."},
};
static const int MII_STATS_FIELD_COUNT = 16;

// Feeling.Type enum hash → display label (from Mii.Feeling.Type EnumArray)
static const struct { uint32_t hash; const char* label; } FEELING_LABELS[] = {
    { 0xb6eede09u, "None"     },
    { 0x390b012du, "Normal"   },
    { 0x62189afbu, "Good"     },
    { 0xab185a53u, "Irritate" },
    { 0xabc36d9fu, "Deject"   },
    { 0x86e2036fu, "Careless" },
    { 0xf3046534u, "Angry"    },
    { 0xf54cf52eu, "Depress"  },
    { 0x651b7eddu, "Worry"    },
};
static const int FEELING_LABEL_COUNT = 9;
static const char* FeelingName(uint32_t hash) {
    for (int i = 0; i < FEELING_LABEL_COUNT; i++)
        if (FEELING_LABELS[i].hash == hash) return FEELING_LABELS[i].label;
    return nullptr;
}

// Word kind options (Mii.MiiMisc.WordInfo.WordArray.WordKind)
static const struct { const char* name; const char* label; } WORD_KIND_LIST[] = {
    { "Invalid",     "(empty)"     },
    { "TalkStart",   "Talk Start"  },
    { "TalkEnd",     "Talk End"    },
    { "Phrase",      "Phrase"      },
    { "Happy",       "Happy"       },
    { "Sad",         "Sad"         },
    { "Angry",       "Angry"       },
    { "Greeting",    "Greeting"    },
    { "TalkInSleep", "Sleep Talk"  },
    { "ShoutToSea",  "Shout Sea"   },
    { "BeforeEat",   "Before Eat"  },
};
static const int WORD_KIND_COUNT = 11;
static int WordKindIndex(uint32_t hash) {
    for (int i = 0; i < WORD_KIND_COUNT; i++)
        if (SaveEditor::Hash(WORD_KIND_LIST[i].name) == hash) return i;
    return -1;
}
static const char* WordKindLabel(uint32_t hash) {
    int i = WordKindIndex(hash);
    return i >= 0 ? WORD_KIND_LIST[i].label : nullptr;
}

// Player.Currency enum hash → symbol / name  (hash of 'Player.Currency' = 0xdc88f139)
static const struct { uint32_t hash; const char* symbol; const char* name; } CURRENCY_LABELS[] = {
    { 0x7e3d1e46u, "---",  "Invalid"    },
    { 0xce8d18e1u, "\xc2\xa5", "Yen"   }, // ¥
    { 0x2bef288au, "$",    "Dollar"     },
    { 0x038c20a9u, "\xe2\x82\xac", "Euro" }, // €
    { 0x2f084465u, "\xc2\xa3", "Pound"  }, // £
    { 0x78915cbeu, "HK$",  "AsiaDollar" },
    { 0xd73beb47u, "KRW",  "Won"        },
    { 0xaca685c9u, "\xc2\xa5", "Yuan"   }, // ¥
    { 0x4616c424u, "RUB",  "Rouble"     },
    { 0xa60f72e0u, "PHP",  "Peso"       },
    { 0x6362048bu, "G$",   "GeneralUse" },
};
static const int CURRENCY_LABEL_COUNT = 11;
static int CurrencyIndex(uint32_t hash) {
    for (int i = 0; i < CURRENCY_LABEL_COUNT; i++)
        if (CURRENCY_LABELS[i].hash == hash) return i;
    return -1;
}
static const char* CurrencySymbol(uint32_t hash) {
    int i = CurrencyIndex(hash); return i >= 0 ? CURRENCY_LABELS[i].symbol : "?";
}
static const char* CurrencyName(uint32_t hash) {
    int i = CurrencyIndex(hash); return i >= 0 ? CURRENCY_LABELS[i].name : "?";
}

// Relation.Info.DirectionalInfo.BaseRelationType enum hash → display name
static const struct { uint32_t hash; const char* name; } RELATION_TYPE_NAMES[] = {
    { 0x0784a8dcu, "Other"       }, { 0x7e3d1e46u, "Invalid"     },
    { 0x354a0515u, "Know"        }, { 0xba939a42u, "Friend"      },
    { 0xc2d067a7u, "Couple"      }, { 0xb7ce0c18u, "Lover"       },
    { 0x7783d4c3u, "Ex-Lover"    }, { 0xfe59f825u, "Divorce"     },
    { 0xdcfc7603u, "Parent"      }, { 0xe193c5a2u, "Child"       },
    { 0x1918f808u, "SiblingOld"  }, { 0x3b1d200au, "SiblingYng"  },
    { 0x2fd9785bu, "Grandparent" }, { 0x7e3cd550u, "Grandchild"  },
    { 0x804172f3u, "Relative"    },
};
static const int RELATION_TYPE_COUNT = 15;
static const char* RelTypeName(uint32_t hash) {
    for (int i = 0; i < RELATION_TYPE_COUNT; i++)
        if (RELATION_TYPE_NAMES[i].hash == hash) return RELATION_TYPE_NAMES[i].name;
    return "Unknown";
}
static SDL_Color RelTypeColor(uint32_t hash) {
    if (hash==0xba939a42u) return {230,210, 60,255}; // Friend   → yellow
    if (hash==0xc2d067a7u) return {255,150,180,255}; // Couple
    if (hash==0xb7ce0c18u) return {255,100,100,255}; // Lover
    if (hash==0x354a0515u) return {100,210,120,255}; // Know     → green
    if (hash==0x7783d4c3u) return {160, 80,160,255}; // Ex-Lover
    if (hash==0xfe59f825u) return {160, 60, 60,255}; // Divorce
    if (hash==0xdcfc7603u||hash==0xe193c5a2u) return {220,150, 80,255}; // Parent/Child
    if (hash==0x1918f808u||hash==0x3b1d200au) return {220,200, 80,255}; // Sibling
    if (hash==0x2fd9785bu||hash==0x7e3cd550u) return {200,170,120,255}; // Grandparent/child
    if (hash==0x804172f3u) return {160,160,160,255}; // Relative
    return {100,100,100,255};
}
static uint32_t RelTypeCounterpart(uint32_t hash) {
    if (hash==0xdcfc7603u) return 0xe193c5a2u; // Parent → Child
    if (hash==0xe193c5a2u) return 0xdcfc7603u; // Child → Parent
    if (hash==0x1918f808u) return 0x3b1d200au; // SiblingOld → SiblingYng
    if (hash==0x3b1d200au) return 0x1918f808u; // SiblingYng → SiblingOld
    if (hash==0x2fd9785bu) return 0x7e3cd550u; // Grandparent → Grandchild
    if (hash==0x7e3cd550u) return 0x2fd9785bu; // Grandchild → Grandparent
    return hash; // all other types are symmetric
}
static bool RelMeterFixed(uint32_t hash) {
    return hash==0x0784a8dcu || hash==0x7e3d1e46u; // Other or Invalid
}

// Fit a UTF-8 string to maxW pixels wide using font f, appending "~" if truncated.
static std::string FitNodeName(const std::string& s, TTF_Font* f, int maxW) {
    const std::string src = s.empty() ? "?" : s;
    if (!f) return src;
    int w = 0, h = 0;
    TTF_SizeUTF8(f, src.c_str(), &w, &h);
    if (w <= maxW) return src;
    std::string t = src;
    while (!t.empty()) {
        // strip trailing bytes until we're at a UTF-8 codepoint boundary
        do { t.pop_back(); } while (!t.empty() && (uint8_t(t.back()) & 0xC0) == 0x80);
        std::string cand = t + "~";
        TTF_SizeUTF8(f, cand.c_str(), &w, &h);
        if (w <= maxW) return cand;
    }
    return "~";
}

// MiiStats sub-tab state
enum class MiiStatsSubTab { Stats=0, Words=1, Relations=2, Social=3 };
static MiiStatsSubTab gMiiStatsSubTab  = MiiStatsSubTab::Stats;
static bool           gSocialExpanded  = false;
static int            gSocialScroll    = 0;
static int            gMiiWordsSlotSel = 0;   // 0-11: selected word slot
static int            gMiiRelSel       = 0;   // selected row in filtered relations list
static int            gMiiRelPairIdx   = -1;  // actual pair array index (set during draw)
static bool           gMiiRelSelfA     = true; // selected Mii is 'A' in the pair

// WebUI standby prevention
static bool            gStandbyOff      = false;
static OnSwitchMode    gPrevOnSwitchMode = OnSwitchMode::UGC;

// Save editor first-launch warning
static bool         gSaveWarningAcked    = false;
static bool         gShowSaveWarning     = false;
static int          gSaveWarningCountdown = 0;
static OnSwitchMode gSaveWarningTarget   = OnSwitchMode::Player;

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

// ─── Touch handler implementations (need globals defined above) ───────────────
static void TSelUser(int idx) {
    if (idx>=0 && idx<(int)gUsers.size()) { gUserSel=idx; gSimKDown|=HidNpadButton_A; }
}
// TSelUgc / TSelMii defined after VISIBLE and LoadPreview are declared (below)
static void TSelBrowse(int idx) {
    if (idx>=0 && idx<(int)gBrowseEntries.size()) { gBrowseSel=idx; gSimKDown|=HidNpadButton_A; }
}
static void TAdjust(int delta) {
    // |delta| is the scale; sign is direction
    if (delta<0) { gTouchScale=-delta; gSimKDown|=HidNpadButton_Left; }
    else         { gTouchScale= delta; gSimKDown|=HidNpadButton_Right; }
    gBtnTouchActive = true;
}
static void TSelPlayerField(int idx) { gPlayerFieldSel=idx; }
static void TSetSkinTone(int idx) {
    if (!gPlayerSav.loaded) return;
    uint32_t h = PFHash(PLAYER_FIELDS[gPlayerFieldSel]);
    uint32_t cur = SaveEditor::GetUInt(gPlayerSav, h);
    if (!gPlayerUndoValid[gPlayerFieldSel]) { gPlayerUndoVal[gPlayerFieldSel]=(int32_t)cur; gPlayerUndoValid[gPlayerFieldSel]=true; }
    SaveEditor::SetUInt(gPlayerSav, h, (uint32_t)idx);
    gPlayerSavDirty = true;
}
static void TSetIslandSize(int val) {
    if (!gPlayerSav.loaded) return;
    uint32_t h = PFHash(PLAYER_FIELDS[gPlayerFieldSel]);
    uint32_t cur = SaveEditor::GetAnyScalar(gPlayerSav, h);
    if (!gPlayerUndoValid[gPlayerFieldSel]) { gPlayerUndoVal[gPlayerFieldSel]=(int32_t)cur; gPlayerUndoValid[gPlayerFieldSel]=true; }
    SaveEditor::SetAnyScalar(gPlayerSav, h, (uint32_t)val);
    gPlayerSavDirty = true;
}
static void TSelMiiStatsMii(int idx) {
    if (idx>=0 && idx<(int)gMiis.size()) gMiiStatsMiiSel=idx;
}
static void TSelMiiStatsField(int idx) { gMiiStatsFieldSel=idx; }
static void TSelMiiWordsSlot(int idx)  { gMiiWordsSlotSel=idx; }
static void TSelMiiRelation(int idx)   { gMiiRelSel=idx; }
static void TRelMeterAdj(int delta) {
    if (gMiiRelPairIdx < 0 || !gMiiSav.loaded) return;
    static const uint32_t H_BASE_RA  = 0x8b41897eu;
    static const uint32_t H_METER_RA = 0x42c2fc2fu;
    int myDir = gMiiRelSelfA ? gMiiRelPairIdx*2   : gMiiRelPairIdx*2+1;
    int otDir = gMiiRelSelfA ? gMiiRelPairIdx*2+1 : gMiiRelPairIdx*2;
    uint32_t myType = SaveEditor::GetEnumAt(gMiiSav, H_BASE_RA, myDir, 0x0784a8dcu);
    if (RelMeterFixed(myType)) return;
    auto adjM = [&](int dir) {
        int32_t m = SaveEditor::GetIntAt(gMiiSav, H_METER_RA, dir);
        m = std::max(0, std::min(100, m + delta));
        SaveEditor::SetIntAt(gMiiSav, H_METER_RA, dir, m);
    };
    adjM(myDir);
    adjM(otDir);
    gMiiSavDirty = true;
}
static void TSetSocialView(int v) {
    gMiiStatsSubTab = (MiiStatsSubTab)v;
    gSocialScroll = 0;
    if (gMiiStatsSubTab != MiiStatsSubTab::Social) gSocialExpanded = false;
}
static void TAckSaveWarning(int) {
    gSaveWarningAcked = true; gShowSaveWarning = false; SaveConfig();
    gOnSwitchMode = gSaveWarningTarget;
    if (gSaveWarningTarget == OnSwitchMode::Player && !gPlayerSav.loaded) {
        std::string err;
        if (!SaveEditor::Load(SAVE_PLAYER_SAV, gPlayerSav, err)) gPlayerMsg = "Load error: " + err;
    }
    if (gSaveWarningTarget == OnSwitchMode::MiiStats && !gMiiSav.loaded) {
        std::string err;
        if (!SaveEditor::Load(SAVE_MII_SAV, gMiiSav, err)) gMiiStatsMsg = "Load error: " + err;
    }
}
static void TPlayerKbd(int) { gSimKDown|=HidNpadButton_A; }
static void TMiiStatsKbd(int) { gSimKDown|=HidNpadButton_A; }

// ─── On-screen keyboard ───────────────────────────────────────────────────────
static std::string ShowKeyboard(const std::string& guide, const std::string& initial, int maxLen=32) {
    SwkbdConfig kbd;
    // swkbdCreate allocates the applet's internal storage — without it swkbdShow crashes.
    if (R_FAILED(swkbdCreate(&kbd, 0))) return initial;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, guide.c_str());
    swkbdConfigSetStringLenMin(&kbd, 0);
    swkbdConfigSetStringLenMax(&kbd, (u32)maxLen);
    swkbdConfigSetInitialText(&kbd, initial.c_str());
    char buf[256] = {};
    Result rc = swkbdShow(&kbd, buf, sizeof(buf));
    swkbdClose(&kbd); // must be paired with every successful swkbdCreate
    return R_SUCCEEDED(rc) ? std::string(buf) : initial;
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
    if (!gThumbTipSeen) { gShowThumbTip=true; gThumbTipCountdown=120; }
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
    HitClear();
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

        HitAdd(cx, startY, CARD_W, CARD_H, TSelUser, idx);

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
    HitClear();
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

    HitAdd(x1,cy,cw,ch, TSimBtn, (int)HidNpadButton_A);
    HitAdd(x2,cy,cw,ch, TSimBtn, (int)HidNpadButton_B);
    HitAdd(x3,cy,cw,ch, TSimBtn, (int)HidNpadButton_X);


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

// Implementations deferred to here because they reference VISIBLE and LoadPreview
static void TSelUgc(int idx) {
    if (idx>=0 && idx<(int)gEntries.size()) {
        gEntrySel=idx;
        if (gEntrySel>=gEntryScroll+VISIBLE) gEntryScroll=gEntrySel-VISIBLE+1;
        if (gEntrySel<gEntryScroll) gEntryScroll=gEntrySel;
        LoadPreview(gEntries[gEntrySel]);
    }
}
static void TSelMii(int idx) {
    if (idx>=0 && idx<(int)gMiis.size()) {
        gMiiSel=idx;
        if (gMiiSel>=gMiiScroll+VISIBLE) gMiiScroll=gMiiSel-VISIBLE+1;
        if (gMiiSel<gMiiScroll) gMiiScroll=gMiiSel;
    }
}

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
        if (key == "export_path")      gExportPath      = val;
        if (key == "thumb_tip_seen")   gThumbTipSeen    = (val == "1");
        if (key == "save_warn_acked")  gSaveWarningAcked = (val == "1");
    }
    fclose(f);
}

static void SaveConfig() {
    FILE* f = fopen(CONFIG_PATH, "w");
    if (!f) return;
    fprintf(f, "export_path=%s\n",     gExportPath.c_str());
    fprintf(f, "thumb_tip_seen=%d\n",  gThumbTipSeen     ? 1 : 0);
    fprintf(f, "save_warn_acked=%d\n", gSaveWarningAcked ? 1 : 0);
    fclose(f);
}

static void DrawFileBrowser() {
    HitClear();
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
            HitAdd(52,py+i*ph,SCREEN_W-104,ph-2, TSelBrowse, idx);
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
    HitClear();
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);

    // Header with subtitle
    std::string subtitle;
    switch(gOnSwitchMode){
        case OnSwitchMode::UGC:      subtitle="textures"; break;
        case OnSwitchMode::Mii:      subtitle="miis"; break;
        case OnSwitchMode::Player:   subtitle="player"; break;
        case OnSwitchMode::MiiStats: subtitle="mii stats"; break;
        default:                     subtitle="webui"; break;
    }
    DrawHeader(subtitle);

    // Tab bar — 5 tabs at 140px each
    {
        int tw=140, th=22, gap=4;
        int totalW=tw*5+gap*4, tx=SCREEN_W/2-totalW/2, ty=28;
        struct { const char* label; OnSwitchMode mode; } tabs[]={
            {"webui",    OnSwitchMode::WebUI},
            {"textures", OnSwitchMode::UGC},
            {"miis",     OnSwitchMode::Mii},
            {"player",   OnSwitchMode::Player},
            {"mii stats",OnSwitchMode::MiiStats},
        };
        for (auto& t : tabs) {
            bool sel = gOnSwitchMode==t.mode;
            HitAdd(tx,ty,tw,th, TSetMode, (int)t.mode);
            FillRect(tx,ty,tw,th, sel?COL_SEL:COL_PANEL);
            DrawRect(tx,ty,tw,th, sel?COL_GOLD:COL_BORDER);
            DrawTextC(t.label, tx+tw/2, ty+th/2, sel?COL_GOLD:COL_DIM);
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
            HitAdd(LIST_X,LIST_Y+LIST_PAD_TOP+i*ITEM_H,LIST_W,ITEM_H-1, TSelUgc, idx);
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
            HitAdd(LIST_X,LIST_Y+LIST_PAD_TOP+i*ITEM_H,LIST_W,ITEM_H-1, TSelMii, idx);
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

        DrawFooter("L/R  switch tab    X  restart server    B  back    +  quit");
    }

    SDL_RenderPresent(gRen);
}

// ─── Player save editor ───────────────────────────────────────────────────────

static const int SE_LIST_X   = 12;
static const int SE_LIST_W   = 320;
static const int SE_DETAIL_X = SE_LIST_X + SE_LIST_W + 8;
static const int SE_DETAIL_W = SCREEN_W - SE_DETAIL_X - 8;
static const int SE_TOP_Y    = LIST_Y;
static const int SE_ROW_H    = 52;

static void DrawPlayer() {
    HitClear();
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);
    DrawHeader("player");

    // Tab bar (same as DrawOnSwitch)
    {
        int tw=140, th=22, gap=4;
        int totalW=tw*5+gap*4, tx=SCREEN_W/2-totalW/2, ty=28;
        struct { const char* label; OnSwitchMode mode; } tabs[]={
            {"webui",OnSwitchMode::WebUI},{"textures",OnSwitchMode::UGC},
            {"miis",OnSwitchMode::Mii},{"player",OnSwitchMode::Player},{"mii stats",OnSwitchMode::MiiStats}};
        for (auto& t : tabs) {
            bool sel=gOnSwitchMode==t.mode;
            HitAdd(tx,ty,tw,th,TSetMode,(int)t.mode);
            FillRect(tx,ty,tw,th,sel?COL_SEL:COL_PANEL);
            DrawRect(tx,ty,tw,th,sel?COL_GOLD:COL_BORDER);
            DrawTextC(t.label,tx+tw/2,ty+th/2,sel?COL_GOLD:COL_DIM);
            tx+=tw+gap;
        }
    }

    if (!gPlayerSav.loaded) {
        DrawTextC("loading current player save...", SCREEN_W/2, SCREEN_H/2, COL_DIM, Font::Md);
        DrawFooter("B  back");
        SDL_RenderPresent(gRen);
        return;
    }

    // Left panel: field list (compact rows)
    FillRect(SE_LIST_X-4, SE_TOP_Y, SE_LIST_W+8, SCREEN_H-SE_TOP_Y-32, COL_PANEL);
    DrawRect(SE_LIST_X-4, SE_TOP_Y, SE_LIST_W+8, SCREEN_H-SE_TOP_Y-32, COL_BORDER);

    static const int PL_ROW_H = 33;
    int plListH = SCREEN_H - SE_TOP_Y - 32;
    int plVis   = plListH / PL_ROW_H;
    if (gPlayerFieldSel < gPlayerScroll) gPlayerScroll = gPlayerFieldSel;
    if (gPlayerFieldSel >= gPlayerScroll + plVis) gPlayerScroll = gPlayerFieldSel - plVis + 1;
    if (gPlayerScroll < 0) gPlayerScroll = 0;

    TTF_Font* fsm_pl = GetFont(Font::Sm);
    for (int i = gPlayerScroll; i < PLAYER_FIELD_COUNT && i < gPlayerScroll + plVis; i++) {
        bool sel = (i == gPlayerFieldSel);
        int ry = SE_TOP_Y + (i - gPlayerScroll) * PL_ROW_H;
        HitAdd(SE_LIST_X, ry, SE_LIST_W, PL_ROW_H-1, TSelPlayerField, i);
        FillRect(SE_LIST_X, ry, SE_LIST_W, PL_ROW_H-1, sel?COL_SEL:COL_BG);
        if (sel) DrawRect(SE_LIST_X, ry, SE_LIST_W, PL_ROW_H-1, COL_GOLD);

        const auto& fd = PLAYER_FIELDS[i];
        uint32_t fh = PFHash(fd);
        int lw=0,lh=0;
        if(fsm_pl) TTF_SizeUTF8(fsm_pl, fd.label, &lw, &lh);
        DrawText(fd.label, SE_LIST_X+8, ry+(PL_ROW_H-lh)/2, sel?COL_TEXT:COL_DIM);

        std::string val;
        if (fd.isStr) {
            val = SaveEditor::GetWStr32(gPlayerSav, fh);
            if (val.size() > 16) val = val.substr(0,14) + "..";
        } else if (fd.isEnum) {
            uint32_t ev = SaveEditor::GetEnum(gPlayerSav, fh);
            val = std::string(CurrencySymbol(ev));
        } else if (fd.isSkinTone) {
            val = std::to_string(SaveEditor::GetUInt(gPlayerSav, fh)+1);
        } else if (fd.isIslandSz) {
            static const char* SL[] = {"50x36","70x50","90x64","120x80"};
            uint32_t sv = SaveEditor::GetAnyScalar(gPlayerSav, fh);
            val = (sv>=1&&sv<=4) ? SL[sv-1] : "?";
        } else if (fd.isRegion) {
            uint32_t ev = SaveEditor::GetEnum(gPlayerSav, fh);
            val = "?";
            for (int ri=0; ri<REGION_OPTION_COUNT; ri++)
                if (SaveEditor::Hash(REGION_OPTIONS[ri].name)==ev) { val=REGION_OPTIONS[ri].label; break; }
            if (val.size()>12) val=val.substr(0,10)+"..";
        } else if (fd.isInt) {
            val = std::to_string((int32_t)SaveEditor::GetAnyScalar(gPlayerSav, fh));
        } else if (fd.isUInt) {
            val = std::to_string(SaveEditor::GetUInt(gPlayerSav, fh));
        } else {
            val = std::to_string(SaveEditor::GetAnyScalar(gPlayerSav, fh));
        }
        int vw=0,vh=0;
        if(fsm_pl) TTF_SizeUTF8(fsm_pl, val.c_str(), &vw, &vh);
        DrawText(val, SE_LIST_X+SE_LIST_W-vw-8, ry+(PL_ROW_H-vh)/2, sel?COL_ACCENT:COL_DIM);
    }

    // Right panel: edit controls for selected field
    FillRect(SE_DETAIL_X, SE_TOP_Y, SE_DETAIL_W, SCREEN_H-SE_TOP_Y-32, {8,8,8,255});
    DrawRect(SE_DETAIL_X, SE_TOP_Y, SE_DETAIL_W, SCREEN_H-SE_TOP_Y-32, COL_BORDER);

    const auto& fd = PLAYER_FIELDS[gPlayerFieldSel];
    uint32_t ph = PFHash(fd);
    int cx = SE_DETAIL_X + SE_DETAIL_W/2;
    int midY = SE_TOP_Y + (SCREEN_H-SE_TOP_Y-32)/2;

    DrawTextC(fd.label, cx, midY-102, COL_DIM, Font::Md);
    DrawTextC(fd.desc,  cx, midY-76,  COL_DIM, Font::Sm);

    if (fd.isStr) {
        std::string val = SaveEditor::GetWStr32(gPlayerSav, ph);
        DrawTextC(val.empty() ? "(empty)" : val, cx, midY-40, COL_TEXT, Font::Lg);
        int bw=260, bh=40, bx=cx-bw/2, by=midY+10;
        HitAdd(bx,by,bw,bh, TPlayerKbd, 0);
        FillRect(bx,by,bw,bh,COL_SEL); DrawRect(bx,by,bw,bh,COL_ACCENT);
        DrawTextC("A  edit with keyboard", cx, by+bh/2, COL_ACCENT, Font::Md);
    } else if (fd.isEnum) {
        uint32_t ev = SaveEditor::GetEnum(gPlayerSav, ph);
        DrawTextC(CurrencySymbol(ev), cx, midY-36, COL_TEXT, Font::Lg);
        DrawTextC(CurrencyName(ev), cx, midY-4, COL_DIM, Font::Md);
        const int btnW=140, btnH=36, btnGap=24;
        int bxL=cx-btnGap/2-btnW, bxR=cx+btnGap/2, btnY=midY+22;
        HitAdd(bxL,btnY,btnW,btnH, TSimBtn, (int)HidNpadButton_Left);
        FillRect(bxL,btnY,btnW,btnH,COL_PANEL); DrawRect(bxL,btnY,btnW,btnH,COL_BORDER);
        DrawTextC("< Prev", bxL+btnW/2, btnY+btnH/2, COL_DIM);
        HitAdd(bxR,btnY,btnW,btnH, TSimBtn, (int)HidNpadButton_Right);
        FillRect(bxR,btnY,btnW,btnH,COL_PANEL); DrawRect(bxR,btnY,btnW,btnH,COL_BORDER);
        DrawTextC("Next >", bxR+btnW/2, btnY+btnH/2, COL_DIM);
        DrawTextC("Left/Right  cycle currency", cx, btnY+btnH+14, COL_DIM);
    } else if (fd.isSkinTone) {
        static const SDL_Color SKIN_COLS[] = {
            {246,217,189,255},{236,193,157,255},{210,160,122,255},
            {176,122,84,255},{130,85,51,255},{90,57,28,255}
        };
        uint32_t cur = SaveEditor::GetUInt(gPlayerSav, ph);
        int sw=66, sh=66, sgap=12;
        int totalSW = 6*(sw+sgap)-sgap;
        int sbx = cx - totalSW/2, sby = midY-30;
        for (int si=0; si<6; si++) {
            bool ssel = ((uint32_t)si == cur);
            HitAdd(sbx, sby, sw, sh, TSetSkinTone, si);
            FillRect(sbx, sby, sw, sh, SKIN_COLS[si]);
            DrawRect(sbx, sby, sw, sh, ssel ? COL_GOLD : SDL_Color{80,80,80,255});
            if (ssel) DrawRect(sbx+2, sby+2, sw-4, sh-4, COL_GOLD);
            sbx += sw+sgap;
        }
        DrawTextC("Left/Right  cycle tone", cx, sby+sh+14, COL_DIM, Font::Sm);
    } else if (fd.isIslandSz) {
        static const char* SZ_LABELS[] = {"50 x 36","70 x 50","90 x 64","120 x 80"};
        uint32_t cur = SaveEditor::GetAnyScalar(gPlayerSav, ph, 0);
        DrawTextC("Other sizes will corrupt the save file!", cx, midY-46, COL_RED, Font::Sm);
        int bw=140, bh=44, bgap=12;
        int totalBW = 4*(bw+bgap)-bgap;
        int bbx = cx - totalBW/2, bby = midY-16;
        for (int pi=0; pi<4; pi++) {
            bool psel = (cur == (uint32_t)(pi+1));
            HitAdd(bbx, bby, bw, bh, TSetIslandSize, pi+1);
            FillRect(bbx, bby, bw, bh, psel?COL_SEL:COL_PANEL);
            DrawRect(bbx, bby, bw, bh, psel?COL_GOLD:COL_BORDER);
            DrawTextC(SZ_LABELS[pi], bbx+bw/2, bby+bh/2, psel?COL_GOLD:COL_DIM);
            bbx += bw+bgap;
        }
        DrawTextC("Left/Right  cycle size", cx, bby+bh+14, COL_DIM, Font::Sm);
    } else if (fd.isRegion) {
        uint32_t ev = SaveEditor::GetEnum(gPlayerSav, ph);
        const char* rlabel = "Unknown";
        for (int ri=0; ri<REGION_OPTION_COUNT; ri++) {
            if (SaveEditor::Hash(REGION_OPTIONS[ri].name)==ev) { rlabel=REGION_OPTIONS[ri].label; break; }
        }
        DrawTextC(rlabel, cx, midY-36, COL_TEXT, Font::Lg);
        const int btnW=140, btnH=36, btnGap=24;
        int bxL=cx-btnGap/2-btnW, bxR=cx+btnGap/2, btnY=midY+22;
        HitAdd(bxL,btnY,btnW,btnH, TSimBtn, (int)HidNpadButton_Left);
        FillRect(bxL,btnY,btnW,btnH,COL_PANEL); DrawRect(bxL,btnY,btnW,btnH,COL_BORDER);
        DrawTextC("< Prev", bxL+btnW/2, btnY+btnH/2, COL_DIM);
        HitAdd(bxR,btnY,btnW,btnH, TSimBtn, (int)HidNpadButton_Right);
        FillRect(bxR,btnY,btnW,btnH,COL_PANEL); DrawRect(bxR,btnY,btnW,btnH,COL_BORDER);
        DrawTextC("Next >", bxR+btnW/2, btnY+btnH/2, COL_DIM);
        DrawTextC("Left/Right  cycle region", cx, btnY+btnH+14, COL_DIM);
    } else {
        // Generic numeric: UInt, Int, RawEnum
        uint32_t rawV = fd.isUInt ? SaveEditor::GetUInt(gPlayerSav, ph) : SaveEditor::GetAnyScalar(gPlayerSav, ph);
        std::string dispV = fd.isInt ? std::to_string((int32_t)rawV) : std::to_string(rawV);
        DrawTextC(dispV, cx, midY-40, COL_TEXT, Font::Lg);

        static const int STEPS[]={1000,100,10,1};
        int bw=52, bh=52, gap=6;
        int totalBW = 4*(bw+gap)*2 + 20;
        int bx = cx - totalBW/2;
        int by = midY+10;
        int btnIdx = 0;
        for (int s : STEPS) {
            bool focused = (gPlayerBtnSel == btnIdx);
            HitAdd(bx,by,bw,bh, TAdjust, -s);
            FillRect(bx,by,bw,bh, focused?COL_SEL:COL_PANEL);
            DrawRect(bx,by,bw,bh, focused?COL_GOLD:COL_RED);
            DrawTextC("-"+std::to_string(s), bx+bw/2, by+bh/2, focused?COL_GOLD:COL_RED, Font::Md);
            bx += bw+gap; btnIdx++;
        }
        bx += 20;
        for (int j = 3; j >= 0; j--) {
            bool focused = (gPlayerBtnSel == btnIdx);
            int s = STEPS[j];
            HitAdd(bx,by,bw,bh, TAdjust, s);
            FillRect(bx,by,bw,bh, focused?COL_SEL:COL_PANEL);
            DrawRect(bx,by,bw,bh, focused?COL_GOLD:COL_GREEN);
            DrawTextC("+"+std::to_string(s), bx+bw/2, by+bh/2, focused?COL_GOLD:COL_GREEN, Font::Md);
            bx += bw+gap; btnIdx++;
        }
    }
    // Undo button for all non-string fields
    if (!fd.isStr) {
        bool canUndo = gPlayerUndoValid[gPlayerFieldSel];
        int ubW=100, ubH=44, ubX=cx-ubW/2, ubY=midY+130;
        HitAdd(ubX, ubY, ubW, ubH, TUndoPlayer, 0);
        FillRect(ubX, ubY, ubW, ubH, COL_PANEL);
        DrawRect(ubX, ubY, ubW, ubH, canUndo ? COL_GOLD : COL_BORDER);
        DrawTextC("Undo", ubX+ubW/2, ubY+ubH/2, canUndo ? COL_GOLD : COL_DIM, Font::Md);
    }

    if (!gPlayerMsg.empty())
        DrawTextC(gPlayerMsg, cx, SE_TOP_Y+16, gPlayerMsgCol);

    DrawFooter("Up/Down  select field    Left/Right  adjust    A  confirm    X  undo    B  save & exit    +  discard");
    SDL_RenderPresent(gRen);
}

// ─── Mii stats editor ─────────────────────────────────────────────────────────
static const int MSE_LIST_W = 240; // mii selector on left

static void DrawMiiStats() {
    HitClear();
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);
    DrawHeader("mii stats");

    // Tab bar
    {
        int tw=140, th=22, gap=4;
        int totalW=tw*5+gap*4, tx=SCREEN_W/2-totalW/2, ty=28;
        struct { const char* label; OnSwitchMode mode; } tabs[]={
            {"webui",OnSwitchMode::WebUI},{"textures",OnSwitchMode::UGC},
            {"miis",OnSwitchMode::Mii},{"player",OnSwitchMode::Player},{"mii stats",OnSwitchMode::MiiStats}};
        for (auto& t : tabs) {
            bool sel=gOnSwitchMode==t.mode;
            HitAdd(tx,ty,tw,th,TSetMode,(int)t.mode);
            FillRect(tx,ty,tw,th,sel?COL_SEL:COL_PANEL);
            DrawRect(tx,ty,tw,th,sel?COL_GOLD:COL_BORDER);
            DrawTextC(t.label,tx+tw/2,ty+th/2,sel?COL_GOLD:COL_DIM);
            tx+=tw+gap;
        }
    }

    if (!gMiiSav.loaded) {
        DrawTextC("loading current mii save...", SCREEN_W/2, SCREEN_H/2, COL_DIM, Font::Md);
        DrawFooter("B  back");
        SDL_RenderPresent(gRen);
        return;
    }

    // Left: mii selector list (with scroll)
    int mseListH = SCREEN_H - SE_TOP_Y - 32;
    int mseVis   = mseListH / ITEM_H;
    FillRect(SE_LIST_X-4, SE_TOP_Y, MSE_LIST_W+8, mseListH, COL_PANEL);
    DrawRect(SE_LIST_X-4, SE_TOP_Y, MSE_LIST_W+8, mseListH, COL_BORDER);
    for (int i = 0; i < mseVis; i++) {
        int idx = gMiiStatsScroll + i;
        if (idx >= (int)gMiis.size()) break;
        bool sel = (idx == gMiiStatsMiiSel);
        int ry = SE_TOP_Y + LIST_PAD_TOP + i * ITEM_H;
        HitAdd(SE_LIST_X, ry, MSE_LIST_W, ITEM_H-1, TSelMiiStatsMii, idx);
        FillRect(SE_LIST_X, ry, MSE_LIST_W, ITEM_H-1, sel?COL_SEL:COL_BG);
        if (sel) DrawRect(SE_LIST_X, ry, MSE_LIST_W, ITEM_H-1, COL_GOLD);
        TTF_Font* fsm=GetFont(Font::Sm); int fw=0,fh=0;
        if(fsm) TTF_SizeUTF8(fsm,gMiis[idx].name.c_str(),&fw,&fh);
        DrawText(gMiis[idx].name, SE_LIST_X+6, ry+(ITEM_H-fh)/2, sel?COL_TEXT:COL_DIM);
    }
    if ((int)gMiis.size() > mseVis) {
        int barH = mseListH * mseVis / (int)gMiis.size();
        int barY = SE_TOP_Y + mseListH * gMiiStatsScroll / (int)gMiis.size();
        FillRect(SE_LIST_X+MSE_LIST_W+2, barY, 4, barH, COL_BORDER);
    }
    if (gMiis.empty()) {
        DrawTextC("no miis", SE_LIST_X+MSE_LIST_W/2, SE_TOP_Y+80, COL_DIM);
    }

    // Middle: field list for selected mii (hidden in Social tab — graph expands into its space)
    int midX = SE_LIST_X + MSE_LIST_W + 12;
    int midW = 340;
    if (gMiiStatsSubTab != MiiStatsSubTab::Social) {
        FillRect(midX, SE_TOP_Y, midW, SCREEN_H-SE_TOP_Y-32, {8,8,8,255});
        DrawRect(midX, SE_TOP_Y, midW, SCREEN_H-SE_TOP_Y-32, COL_BORDER);
    }

    if (!gMiis.empty() && gMiiStatsMiiSel < (int)gMiis.size()) {
        int miiSlotIdx = gMiis[gMiiStatsMiiSel].slot - 1; // 0-based array index

        if (gMiiStatsSubTab == MiiStatsSubTab::Words) {
            // Word slots list (12 items)
            static const uint32_t H_WKIND_L = SaveEditor::Hash("Mii.MiiMisc.WordInfo.WordArray.WordKind");
            static const uint32_t H_WTXT_L  = SaveEditor::Hash("Mii.MiiMisc.WordInfo.WordArray.WordText");
            static const uint32_t H_WINV_L  = SaveEditor::Hash("Invalid");
            TTF_Font* fsm = GetFont(Font::Sm);
            for (int i = 0; i < 12; i++) {
                bool sel = (i == gMiiWordsSlotSel);
                int ry = SE_TOP_Y + 2 + i * 34;
                if (ry + 34 > SCREEN_H - 32) break;
                HitAdd(midX+1, ry, midW-2, 33, TSelMiiWordsSlot, i);
                FillRect(midX+1, ry, midW-2, 33, sel?COL_SEL:COL_BG);
                if (sel) DrawRect(midX+1, ry, midW-2, 33, COL_GOLD);
                int wIdx = miiSlotIdx * 12 + i;
                uint32_t kh = SaveEditor::GetAnyEnumAt(gMiiSav, H_WKIND_L, wIdx, H_WINV_L);
                bool filled = (kh != H_WINV_L);
                const char* kLbl = filled ? WordKindLabel(kh) : nullptr;
                // Kind label right-aligned
                int kw=0,kh2=0;
                if (fsm && kLbl) TTF_SizeUTF8(fsm, kLbl, &kw, &kh2);
                if (kLbl) DrawText(kLbl, midX+midW-kw-8, ry+(33-kh2)/2, sel?COL_ACCENT:COL_DIM);
                // Slot number
                std::string numS = std::to_string(i+1)+".";
                int nw=0,nh=0;
                if (fsm) TTF_SizeUTF8(fsm, numS.c_str(), &nw, &nh);
                DrawText(numS, midX+8, ry+(33-nh)/2, sel?COL_TEXT:COL_DIM);
                // Word text or status
                int availW = midW - kw - (kw?8:0) - 8 - nw - 8;
                std::string wt = filled ? SaveEditor::GetWStr64At(gMiiSav, H_WTXT_L, wIdx) : "";
                std::string disp = filled ? (wt.empty()?"—":FitNodeName(wt,fsm,availW)) : "(empty)";
                DrawText(disp, midX+8+nw+4, ry+(33-nh)/2, sel?(filled?COL_TEXT:COL_DIM):(filled?COL_DIM:COL_DIM));
            }
        } else if (gMiiStatsSubTab == MiiStatsSubTab::Relations) {
            // Relations list: pairs involving this Mii
            static const uint32_t H_IDA_ML   = 0xf7420afbu;
            static const uint32_t H_IDB_ML   = 0x4071f71cu;
            static const uint32_t H_BASE_ML  = 0x8b41897eu;
            static const uint32_t H_MNAME_ML = 0x2499bfdau;
            int pairCount = SaveEditor::ArraySize(gMiiSav, H_IDA_ML);
            TTF_Font* fsm2 = GetFont(Font::Sm);
            int row = 0, filtRow = 0;
            gMiiRelPairIdx = -1;
            for (int pi = 0; pi < pairCount; pi++) {
                int a = SaveEditor::GetIntAt(gMiiSav, H_IDA_ML, pi);
                int b = SaveEditor::GetIntAt(gMiiSav, H_IDB_ML, pi);
                if (a < 0 || b < 0) continue;
                if (a != miiSlotIdx && b != miiSlotIdx) continue;
                bool selfA = (a == miiSlotIdx);
                int other  = selfA ? b : a;
                int myDir  = selfA ? pi*2 : pi*2+1;
                uint32_t myType = SaveEditor::GetEnumAt(gMiiSav, H_BASE_ML, myDir, 0x0784a8dcu);
                SDL_Color tc = RelTypeColor(myType);
                bool sel = (filtRow == gMiiRelSel);
                if (sel) { gMiiRelPairIdx = pi; gMiiRelSelfA = selfA; }
                int ry = SE_TOP_Y + 2 + row * 34;
                if (ry + 34 > SCREEN_H - 32) break;
                HitAdd(midX+1, ry, midW-2, 33, TSelMiiRelation, filtRow);
                FillRect(midX+1, ry, midW-2, 33, sel?COL_SEL:COL_BG);
                if (sel) DrawRect(midX+1, ry, midW-2, 33, COL_GOLD);
                // Type label right-aligned (colored)
                const char* tName = RelTypeName(myType);
                int tw=0,th=0;
                if (fsm2 && tName) TTF_SizeUTF8(fsm2, tName, &tw, &th);
                DrawText(tName?tName:"?", midX+midW-tw-8, ry+(33-th)/2, tc);
                // Other Mii name left
                std::string oName = SaveEditor::GetWStr32At(gMiiSav, H_MNAME_ML, other);
                int availRL = midW - tw - (tw?8:0) - 16;
                std::string oFit = FitNodeName(oName.empty()?"?":oName, fsm2, availRL);
                int rnw=0,rnh=0;
                if (fsm2) TTF_SizeUTF8(fsm2, oFit.c_str(), &rnw, &rnh);
                DrawText(oFit, midX+8, ry+(33-rnh)/2, sel?COL_TEXT:COL_DIM);
                filtRow++; row++;
            }
            if (filtRow > 0 && gMiiRelSel >= filtRow) gMiiRelSel = filtRow-1;
            if (row == 0)
                DrawTextC("no relations", midX+midW/2, SE_TOP_Y+80, COL_DIM);
        } else {
            // Stats fields list
            for (int i = 0; i < MII_STATS_FIELD_COUNT; i++) {
                bool sel = (i == gMiiStatsFieldSel);
                int ry = SE_TOP_Y + 2 + i * 34;
                if (ry + 34 > SCREEN_H - 32) break;
                HitAdd(midX+1, ry, midW-2, 33, TSelMiiStatsField, i);
                FillRect(midX+1, ry, midW-2, 33, sel?COL_SEL:COL_BG);
                if (sel) DrawRect(midX+1, ry, midW-2, 33, COL_GOLD);
                const auto& fd = MII_STATS_FIELDS[i];
                std::string val;
                if (fd.isStr) {
                    val = SaveEditor::GetWStr32At(gMiiSav, SaveEditor::Hash(fd.fieldName), miiSlotIdx);
                } else if (fd.isUInt) {
                    val = std::to_string(SaveEditor::GetUIntAt(gMiiSav, SaveEditor::Hash(fd.fieldName), miiSlotIdx));
                } else if (fd.isEnum) {
                    uint32_t ev = SaveEditor::GetEnumAt(gMiiSav, SaveEditor::Hash(fd.fieldName), miiSlotIdx);
                    const char* lbl = FeelingName(ev);
                    val = lbl ? lbl : ("?" + std::to_string(ev));
                } else {
                    int32_t v = SaveEditor::GetIntAt(gMiiSav, SaveEditor::Hash(fd.fieldName), miiSlotIdx);
                    val = std::to_string(v + fd.dispOffset);
                }
                TTF_Font* fsm=GetFont(Font::Sm); int lw=0,lh=0,vw=0,vh=0;
                if(fsm) TTF_SizeUTF8(fsm,fd.label,&lw,&lh);
                if(fsm) TTF_SizeUTF8(fsm,val.c_str(),&vw,&vh);
                DrawText(fd.label, midX+8, ry+(33-lh)/2, sel?COL_TEXT:COL_DIM);
                DrawText(val, midX+midW-vw-8, ry+(33-vh)/2, sel?COL_ACCENT:COL_DIM);
            }
        }
    }

    // Right: edit controls / social view
    // In Social mode the panel extends left to cover the hidden middle panel
    int editX = midX + midW + 8;
    int editW = SCREEN_W - editX - 8;
    int panelX = (gMiiStatsSubTab == MiiStatsSubTab::Social) ? midX   : editX;
    int panelW = (gMiiStatsSubTab == MiiStatsSubTab::Social) ? (SCREEN_W - midX - 8) : editW;
    FillRect(panelX, SE_TOP_Y, panelW, SCREEN_H-SE_TOP_Y-32, {6,6,6,255});
    DrawRect(panelX, SE_TOP_Y, panelW, SCREEN_H-SE_TOP_Y-32, COL_BORDER);

    // Sub-tab bar: [Stats] [Words] [Relations] [Social]
    {
        int stW=100, stH=22, stGap=4, stX=editX+8, stY=SE_TOP_Y+6;
        const char* stLabels[] = {"Stats","Words","Relations","Social"};
        for (int ti=0; ti<4; ti++) {
            bool sSel = ((int)gMiiStatsSubTab == ti);
            int sx = stX + ti*(stW+stGap);
            HitAdd(sx, stY, stW, stH, TSetSocialView, ti);
            FillRect(sx, stY, stW, stH, sSel?COL_SEL:COL_PANEL);
            DrawRect(sx, stY, stW, stH, sSel?COL_GOLD:COL_BORDER);
            DrawTextC(stLabels[ti], sx+stW/2, stY+stH/2, sSel?COL_GOLD:COL_DIM);
        }
    }

    if (!gMiis.empty() && gMiiStatsMiiSel < (int)gMiis.size()) {
        int miiSlotIdx = gMiis[gMiiStatsMiiSel].slot - 1;
        int panelTop = SE_TOP_Y + 32;
        int panelH   = SCREEN_H - panelTop - 32;

        if (gMiiStatsSubTab == MiiStatsSubTab::Social) {
            static const uint32_t H_ID_A  = 0xf7420afbu;
            static const uint32_t H_ID_B  = 0x4071f71cu;
            static const uint32_t H_BASE  = 0x8b41897eu;
            static const uint32_t H_METER = 0x42c2fc2fu;
            static const uint32_t H_NAME  = 0x2499bfdau;
            int pairCount = SaveEditor::ArraySize(gMiiSav, H_ID_A);

            if (gSocialExpanded) {
                // ── Global graph ─────────────────────────────────────────────────
                int gcx = panelX + panelW/2;
                int gcy = panelTop + panelH/2;
                int N   = (int)gMiis.size();
                if (N == 0) {
                    DrawTextC("no miis", gcx, gcy, COL_DIM, Font::Md);
                } else {
                    // Node size scales with count
                    int nW = N <= 8 ? 80 : N <= 14 ? 68 : 56;
                    int nH = N <= 8 ? 30 : N <= 14 ? 26 : 22;
                    // Radius: large enough that adjacent nodes don't overlap
                    int defR = std::min(panelW/2, panelH/2) - nH/2 - 10;
                    int R = defR;
                    if (N > 1) {
                        int minR = (int)((float)(nW + 6) / (2.0f * sinf((float)M_PI / N))) + 2;
                        if (minR > R) R = minR;
                    }
                    // Clamp so nodes stay inside the panel
                    int maxR = std::min(panelW/2 - nW/2 - 4, panelH/2 - nH/2 - 4);
                    if (R > maxR) R = maxR;
                    if (R < 40)  R = 40;

                    // Pre-compute node positions
                    std::vector<int> nx_(N), ny_(N);
                    for (int i = 0; i < N; i++) {
                        float a = -(float)M_PI/2.f + (2.f*(float)M_PI*i)/N;
                        nx_[i] = gcx + (int)((float)R * cosf(a));
                        ny_[i] = gcy + (int)((float)R * sinf(a));
                    }
                    // Edges first — colored, semi-transparent, no labels
                    for (int i = 0; i < pairCount; i++) {
                        int sa = SaveEditor::GetIntAt(gMiiSav, H_ID_A, i);
                        int sb = SaveEditor::GetIntAt(gMiiSav, H_ID_B, i);
                        if (sa < 0 || sb < 0) continue;
                        int ai = -1, bi = -1;
                        for (int j = 0; j < N; j++) {
                            if (gMiis[j].slot-1 == sa) ai = j;
                            if (gMiis[j].slot-1 == sb) bi = j;
                        }
                        if (ai < 0 || bi < 0) continue;
                        SDL_Color lc = RelTypeColor(SaveEditor::GetEnumAt(gMiiSav, H_BASE, i*2));
                        SDL_SetRenderDrawColor(gRen, lc.r, lc.g, lc.b, 110);
                        SDL_RenderDrawLine(gRen, nx_[ai], ny_[ai], nx_[bi], ny_[bi]);
                    }
                    // Nodes on top of edges
                    TTF_Font* fsm = GetFont(Font::Sm);
                    for (int i = 0; i < N; i++) {
                        bool isSel = (i == gMiiStatsMiiSel);
                        std::string raw = SaveEditor::GetWStr32At(gMiiSav, H_NAME, gMiis[i].slot-1);
                        std::string name = FitNodeName(raw, fsm, nW - 10);
                        SDL_Color fill   = isSel ? COL_ACCENT : COL_PANEL2;
                        SDL_Color border = isSel ? COL_GOLD   : COL_BORDER;
                        SDL_Color text   = isSel ? SDL_Color{10,10,10,255} : COL_TEXT;
                        // Shadow
                        FillRect(nx_[i]-nW/2+2, ny_[i]-nH/2+2, nW, nH, {0,0,0,110});
                        HitAdd(nx_[i]-nW/2, ny_[i]-nH/2, nW, nH, TSelMiiStatsMii, i);
                        FillRect(nx_[i]-nW/2, ny_[i]-nH/2, nW, nH, fill);
                        // Gold top strip for selected
                        if (isSel) FillRect(nx_[i]-nW/2, ny_[i]-nH/2, nW, 2, COL_GOLD);
                        DrawRect(nx_[i]-nW/2, ny_[i]-nH/2, nW, nH, border);
                        DrawTextC(name, nx_[i], ny_[i], text, Font::Sm);
                    }
                }
            } else {
                // ── Focus graph: selected mii at center, connections as satellites ─
                struct SRow { int other; uint32_t outT; int32_t outM; };
                std::vector<SRow> rows;
                for (int i = 0; i < pairCount; i++) {
                    int a = SaveEditor::GetIntAt(gMiiSav, H_ID_A, i);
                    int b = SaveEditor::GetIntAt(gMiiSav, H_ID_B, i);
                    if (a < 0 || b < 0) continue;
                    if (a != miiSlotIdx && b != miiSlotIdx) continue;
                    bool selfA = (a == miiSlotIdx);
                    SRow r;
                    r.other = selfA ? b : a;
                    r.outT  = SaveEditor::GetEnumAt(gMiiSav, H_BASE, selfA ? i*2 : i*2+1);
                    r.outM  = SaveEditor::GetIntAt(gMiiSav, H_METER, selfA ? i*2 : i*2+1);
                    rows.push_back(r);
                }
                int gcx = panelX + panelW/2;
                int gcy = panelTop + panelH/2;

                if (rows.empty()) {
                    DrawTextC("no relationships", gcx, gcy, COL_DIM, Font::Md);
                } else {
                    int n   = (int)rows.size();
                    int nW  = n > 10 ? 80 : 96, nH = 44;
                    int cnW = 120, cnH = 44;
                    int R   = std::min(panelW/2, panelH/2) - nH/2 - 16;
                    if (R < 80) R = 80;

                    TTF_Font* fsm = GetFont(Font::Sm);
                    std::string cName = SaveEditor::GetWStr32At(gMiiSav, H_NAME, miiSlotIdx);
                    std::string cFit  = FitNodeName(cName.empty()?"?":cName, fsm, cnW - 14);

                    // Pre-compute satellite positions
                    std::vector<int> snx(n), sny(n);
                    for (int i = 0; i < n; i++) {
                        float angle = -(float)M_PI/2.f + (2.f*(float)M_PI*i)/n;
                        snx[i] = gcx + (int)((float)R * cosf(angle));
                        sny[i] = gcy + (int)((float)R * sinf(angle));
                    }

                    // 1. Edges (drawn under everything)
                    for (int i = 0; i < n; i++) {
                        SDL_Color lc = RelTypeColor(rows[i].outT);
                        SDL_SetRenderDrawColor(gRen, lc.r, lc.g, lc.b, 180);
                        SDL_RenderDrawLine(gRen, gcx, gcy, snx[i], sny[i]);
                    }

                    // 2. Center node (drawn before satellites so satellites overlap it)
                    FillRect(gcx-cnW/2+2, gcy-cnH/2+2, cnW, cnH, {0,0,0,110});
                    FillRect(gcx-cnW/2, gcy-cnH/2, cnW, cnH, COL_ACCENT);
                    DrawRect(gcx-cnW/2, gcy-cnH/2, cnW, cnH, COL_TEXT);
                    DrawTextC(cFit, gcx, gcy, {10,10,10,255}, Font::Md);

                    // 3. Satellite nodes
                    for (int i = 0; i < n; i++) {
                        SDL_Color lc = RelTypeColor(rows[i].outT);
                        const char* relLbl = RelTypeName(rows[i].outT);
                        std::string nn    = SaveEditor::GetWStr32At(gMiiSav, H_NAME, rows[i].other);
                        std::string nFit  = FitNodeName(nn.empty()?"?":nn, fsm, nW - 16);
                        // Shadow
                        FillRect(snx[i]-nW/2+2, sny[i]-nH/2+2, nW, nH, {0,0,0,110});
                        FillRect(snx[i]-nW/2, sny[i]-nH/2, nW, nH, COL_PANEL2);
                        // Left accent bar (relationship color)
                        FillRect(snx[i]-nW/2, sny[i]-nH/2, 3, nH, lc);
                        DrawRect(snx[i]-nW/2, sny[i]-nH/2, nW, nH, lc);
                        // Name (upper row)
                        DrawTextC(nFit, snx[i]+2, sny[i]-11, COL_TEXT, Font::Sm);
                        // Rel type (lower row, colored)
                        DrawTextC(relLbl ? relLbl : "?", snx[i]+2, sny[i]+11, lc, Font::Sm);
                    }
                }
            }
        } else if (gMiiStatsSubTab == MiiStatsSubTab::Words) {
            // ── Words editor ──────────────────────────────────────────────────────
            static const uint32_t H_WKIND_R  = SaveEditor::Hash("Mii.MiiMisc.WordInfo.WordArray.WordKind");
            static const uint32_t H_WTXT_R   = SaveEditor::Hash("Mii.MiiMisc.WordInfo.WordArray.WordText");
            static const uint32_t H_WHOW_R   = SaveEditor::Hash("Mii.MiiMisc.WordInfo.WordArray.WordHowToCall");
            static const uint32_t H_WINV_R   = SaveEditor::Hash("Invalid");
            int wIdx = miiSlotIdx * 12 + gMiiWordsSlotSel;
            uint32_t curKind  = SaveEditor::GetAnyEnumAt(gMiiSav, H_WKIND_R, wIdx, H_WINV_R);
            std::string curText = SaveEditor::GetWStr64At(gMiiSav, H_WTXT_R, wIdx);
            std::string curHow  = SaveEditor::GetWStr64At(gMiiSav, H_WHOW_R, wIdx);
            bool isFilled = (curKind != H_WINV_R);
            const char* kindLbl = WordKindLabel(curKind);
            int cx2 = editX + editW/2;
            // Slot header
            DrawTextC("Slot " + std::to_string(gMiiWordsSlotSel+1) + " / 12",
                      cx2, panelTop+14, COL_DIM, Font::Sm);
            // Kind section
            SDL_SetRenderDrawColor(gRen,COL_BORDER.r,COL_BORDER.g,COL_BORDER.b,80);
            SDL_RenderDrawLine(gRen, editX+12, panelTop+30, editX+editW-12, panelTop+30);
            DrawTextC("Kind", cx2, panelTop+46, COL_DIM, Font::Sm);
            DrawTextC(isFilled?(kindLbl?kindLbl:"?"):"(empty)",
                      cx2, panelTop+76, isFilled?COL_TEXT:COL_DIM, Font::Lg);
            {
                int bW=108,bH=28,bGap=14;
                int bxL=cx2-bGap/2-bW, bxR=cx2+bGap/2, bY=panelTop+112;
                HitAdd(bxL,bY,bW,bH,TSimBtn,(int)HidNpadButton_Left);
                FillRect(bxL,bY,bW,bH,COL_PANEL); DrawRect(bxL,bY,bW,bH,COL_BORDER);
                DrawTextC("< Kind",bxL+bW/2,bY+bH/2,COL_DIM);
                HitAdd(bxR,bY,bW,bH,TSimBtn,(int)HidNpadButton_Right);
                FillRect(bxR,bY,bW,bH,COL_PANEL); DrawRect(bxR,bY,bW,bH,COL_BORDER);
                DrawTextC("Kind >",bxR+bW/2,bY+bH/2,COL_DIM);
            }
            // Text section
            SDL_SetRenderDrawColor(gRen,COL_BORDER.r,COL_BORDER.g,COL_BORDER.b,80);
            SDL_RenderDrawLine(gRen, editX+12, panelTop+154, editX+editW-12, panelTop+154);
            DrawTextC("Word Text", cx2, panelTop+166, COL_DIM, Font::Sm);
            std::string dispTxt = curText.empty() ? "(none)" : curText;
            DrawTextC(dispTxt, cx2, panelTop+192, (isFilled&&!curText.empty())?COL_TEXT:COL_DIM, Font::Md);
            {
                int bW=240,bH=34,bX=cx2-bW/2,bY=panelTop+218;
                HitAdd(bX,bY,bW,bH,TSimBtn,(int)HidNpadButton_A);
                FillRect(bX,bY,bW,bH,COL_SEL); DrawRect(bX,bY,bW,bH,COL_ACCENT);
                DrawTextC("A  Edit Text",cx2,bY+bH/2,COL_ACCENT,Font::Sm);
            }
            // Pronunciation section
            SDL_SetRenderDrawColor(gRen,COL_BORDER.r,COL_BORDER.g,COL_BORDER.b,80);
            SDL_RenderDrawLine(gRen, editX+12, panelTop+266, editX+editW-12, panelTop+266);
            DrawTextC("Pronunciation  (optional)", cx2, panelTop+280, COL_DIM, Font::Sm);
            std::string dispHow = curHow.empty() ? "(same as text)" : curHow;
            DrawTextC(dispHow, cx2, panelTop+302, !curHow.empty()?COL_TEXT:COL_DIM, Font::Sm);
            {
                int bW=240,bH=34,bX=cx2-bW/2,bY=panelTop+322;
                HitAdd(bX,bY,bW,bH,TSimBtn,(int)HidNpadButton_X);
                FillRect(bX,bY,bW,bH,COL_SEL); DrawRect(bX,bY,bW,bH,COL_ACCENT);
                DrawTextC("X  Edit Pronunciation",cx2,bY+bH/2,COL_ACCENT,Font::Sm);
            }
            if (!gMiiStatsMsg.empty())
                DrawTextC(gMiiStatsMsg, cx2, panelTop+376, gMiiStatsMsgCol);
        } else if (gMiiStatsSubTab == MiiStatsSubTab::Relations) {
            // ── Relations editor ─────────────────────────────────────────────────
            static const uint32_t H_BASE_RP  = 0x8b41897eu;
            static const uint32_t H_METER_RP = 0x42c2fc2fu;
            static const uint32_t H_IDA_RP   = 0xf7420afbu;
            static const uint32_t H_IDB_RP   = 0x4071f71cu;
            static const uint32_t H_MNAME_RP = 0x2499bfdau;
            int cx2 = editX + editW/2;
            if (gMiiRelPairIdx < 0) {
                DrawTextC("no relations", cx2, panelTop + panelH/2, COL_DIM, Font::Md);
            } else {
                int myDir  = gMiiRelSelfA ? gMiiRelPairIdx*2   : gMiiRelPairIdx*2+1;
                int otDir  = gMiiRelSelfA ? gMiiRelPairIdx*2+1 : gMiiRelPairIdx*2;
                uint32_t myType = SaveEditor::GetEnumAt(gMiiSav, H_BASE_RP, myDir, 0x0784a8dcu);
                uint32_t otType = SaveEditor::GetEnumAt(gMiiSav, H_BASE_RP, otDir, 0x0784a8dcu);
                int32_t  myMeter = SaveEditor::GetIntAt(gMiiSav, H_METER_RP, myDir);
                bool fixed = RelMeterFixed(myType);
                int other = gMiiRelSelfA ? SaveEditor::GetIntAt(gMiiSav, H_IDB_RP, gMiiRelPairIdx)
                                         : SaveEditor::GetIntAt(gMiiSav, H_IDA_RP, gMiiRelPairIdx);
                std::string oName = SaveEditor::GetWStr32At(gMiiSav, H_MNAME_RP, other < 0 ? 0 : other);
                // Header: other Mii name
                DrawTextC(oName.empty()?"(unknown)":oName, cx2, panelTop+14, COL_TEXT, Font::Md);
                SDL_SetRenderDrawColor(gRen,COL_BORDER.r,COL_BORDER.g,COL_BORDER.b,80);
                SDL_RenderDrawLine(gRen, editX+12, panelTop+34, editX+editW-12, panelTop+34);
                // Your relationship type
                DrawTextC("Your type", cx2, panelTop+48, COL_DIM, Font::Sm);
                SDL_Color myTC = RelTypeColor(myType);
                const char* myTN = RelTypeName(myType);
                DrawTextC(myTN?myTN:"?", cx2, panelTop+74, myTC, Font::Lg);
                {
                    int bW=108,bH=28,bGap=14;
                    int bxL=cx2-bGap/2-bW, bxR=cx2+bGap/2, bY=panelTop+112;
                    HitAdd(bxL,bY,bW,bH,TSimBtn,(int)HidNpadButton_Left);
                    FillRect(bxL,bY,bW,bH,COL_PANEL); DrawRect(bxL,bY,bW,bH,COL_BORDER);
                    DrawTextC("< Type",bxL+bW/2,bY+bH/2,COL_DIM);
                    HitAdd(bxR,bY,bW,bH,TSimBtn,(int)HidNpadButton_Right);
                    FillRect(bxR,bY,bW,bH,COL_PANEL); DrawRect(bxR,bY,bW,bH,COL_BORDER);
                    DrawTextC("Type >",bxR+bW/2,bY+bH/2,COL_DIM);
                }
                // Their type (auto-counterpart, read-only display)
                SDL_SetRenderDrawColor(gRen,COL_BORDER.r,COL_BORDER.g,COL_BORDER.b,80);
                SDL_RenderDrawLine(gRen, editX+12, panelTop+148, editX+editW-12, panelTop+148);
                DrawTextC("Their type  (auto)", cx2, panelTop+160, COL_DIM, Font::Sm);
                SDL_Color otTC = RelTypeColor(otType);
                const char* otTN = RelTypeName(otType);
                DrawTextC(otTN?otTN:"?", cx2, panelTop+180, otTC, Font::Sm);
                // Meter
                SDL_SetRenderDrawColor(gRen,COL_BORDER.r,COL_BORDER.g,COL_BORDER.b,80);
                SDL_RenderDrawLine(gRen, editX+12, panelTop+206, editX+editW-12, panelTop+206);
                DrawTextC("Meter", cx2, panelTop+220, COL_DIM, Font::Sm);
                std::string mStr = fixed ? "100 (fixed)" : std::to_string(myMeter);
                DrawTextC(mStr, cx2, panelTop+246, fixed?COL_DIM:COL_TEXT, Font::Lg);
                if (!fixed) {
                    static const int MSTEPS[] = {10, 1};
                    int bW=62,bH=32,bGap=5;
                    int totalBW = (2*(bW+bGap))*2 + 20;
                    int bxBase = cx2 - totalBW/2, bY = panelTop+284;
                    int bx = bxBase;
                    for (int s : MSTEPS) {
                        HitAdd(bx,bY,bW,bH,TRelMeterAdj,-s);
                        FillRect(bx,bY,bW,bH,COL_PANEL); DrawRect(bx,bY,bW,bH,COL_RED);
                        DrawTextC("-"+std::to_string(s),bx+bW/2,bY+bH/2,COL_RED,Font::Sm);
                        bx+=bW+bGap;
                    }
                    bx+=20;
                    for (int j=1;j>=0;j--) {
                        int s=MSTEPS[j];
                        HitAdd(bx,bY,bW,bH,TRelMeterAdj,s);
                        FillRect(bx,bY,bW,bH,COL_PANEL); DrawRect(bx,bY,bW,bH,COL_GREEN);
                        DrawTextC("+"+std::to_string(s),bx+bW/2,bY+bH/2,COL_GREEN,Font::Sm);
                        bx+=bW+bGap;
                    }
                    DrawTextC("both directions set together", cx2, panelTop+326, COL_DIM, Font::Sm);
                }
            }
        } else {
            // Stats editor (original)
            const auto& fd = MII_STATS_FIELDS[gMiiStatsFieldSel];
            int cx2 = editX + editW/2;
            int midY2 = panelTop + panelH/2;

            DrawTextC(fd.label, cx2, midY2-90, COL_DIM, Font::Md);
            DrawTextC(fd.desc, cx2, midY2-62, COL_DIM, Font::Sm);

            if (fd.isStr) {
                std::string val = SaveEditor::GetWStr32At(gMiiSav, SaveEditor::Hash(fd.fieldName), miiSlotIdx);
                DrawTextC(val.empty()?"(empty)":val, cx2, midY2-30, COL_TEXT, Font::Lg);
                int bw=220, bh=36, bx=cx2-bw/2, by=midY2+10;
                HitAdd(bx,by,bw,bh, TMiiStatsKbd, 0);
                FillRect(bx,by,bw,bh,COL_SEL); DrawRect(bx,by,bw,bh,COL_ACCENT);
                DrawTextC("A  keyboard", cx2, by+bh/2, COL_ACCENT, Font::Md);
            } else if (fd.isEnum) {
                uint32_t enumVal = SaveEditor::GetEnumAt(gMiiSav, SaveEditor::Hash(fd.fieldName), miiSlotIdx);
                const char* lbl = FeelingName(enumVal);
                DrawTextC(lbl ? lbl : ("?" + std::to_string(enumVal)).c_str(), cx2, midY2-30, COL_TEXT, Font::Lg);
                const int btnW=140, btnH=36, btnGap=24;
                int bxL=cx2-btnGap/2-btnW, bxR=cx2+btnGap/2, btnY=midY2+10;
                HitAdd(bxL,btnY,btnW,btnH, TSimBtn, (int)HidNpadButton_Left);
                FillRect(bxL,btnY,btnW,btnH,COL_PANEL); DrawRect(bxL,btnY,btnW,btnH,COL_BORDER);
                DrawTextC("< Prev", bxL+btnW/2, btnY+btnH/2, COL_DIM);
                HitAdd(bxR,btnY,btnW,btnH, TSimBtn, (int)HidNpadButton_Right);
                FillRect(bxR,btnY,btnW,btnH,COL_PANEL); DrawRect(bxR,btnY,btnW,btnH,COL_BORDER);
                DrawTextC("Next >", bxR+btnW/2, btnY+btnH/2, COL_DIM);
                DrawTextC("Left/Right  cycle mood", cx2, btnY+btnH+14, COL_DIM);
            } else {
                int32_t rawVal = fd.isUInt ? (int32_t)SaveEditor::GetUIntAt(gMiiSav,SaveEditor::Hash(fd.fieldName),miiSlotIdx)
                                           : SaveEditor::GetIntAt(gMiiSav,SaveEditor::Hash(fd.fieldName),miiSlotIdx);
                DrawTextC(std::to_string(rawVal + fd.dispOffset), cx2, midY2-30, COL_TEXT, Font::Lg);

                static const int STEPS2[]={100,10,1};
                int bw2=60, bh2=60, gap2=6;
                int totalBW2=(3*(bw2+gap2))*2+16;
                int bx2=cx2-totalBW2/2, by2=midY2+10;
                int btn2Idx=0;
                for (int s : STEPS2) {
                    bool focused=(gMiiBtnSel==btn2Idx);
                    HitAdd(bx2,by2,bw2,bh2, TAdjust, -s);
                    FillRect(bx2,by2,bw2,bh2, focused?COL_SEL:COL_PANEL);
                    DrawRect(bx2,by2,bw2,bh2, focused?COL_GOLD:COL_RED);
                    DrawTextC("-"+std::to_string(s), bx2+bw2/2, by2+bh2/2, focused?COL_GOLD:COL_RED, Font::Md);
                    bx2+=bw2+gap2; btn2Idx++;
                }
                bx2+=16;
                for (int j=2;j>=0;j--) {
                    bool focused=(gMiiBtnSel==btn2Idx);
                    int s=STEPS2[j];
                    HitAdd(bx2,by2,bw2,bh2, TAdjust, s);
                    FillRect(bx2,by2,bw2,bh2, focused?COL_SEL:COL_PANEL);
                    DrawRect(bx2,by2,bw2,bh2, focused?COL_GOLD:COL_GREEN);
                    DrawTextC("+"+std::to_string(s), bx2+bw2/2, by2+bh2/2, focused?COL_GOLD:COL_GREEN, Font::Md);
                    bx2+=bw2+gap2; btn2Idx++;
                }
                // Undo button
                {
                    bool canUndo = gMiiUndoValid[gMiiStatsFieldSel];
                    int ubW=100, ubH=44, ubX=cx2-ubW/2, ubY=by2+bh2+14;
                    HitAdd(ubX, ubY, ubW, ubH, TUndoMii, 0);
                    FillRect(ubX, ubY, ubW, ubH, COL_PANEL);
                    DrawRect(ubX, ubY, ubW, ubH, canUndo ? COL_GOLD : COL_BORDER);
                    DrawTextC("Undo", ubX+ubW/2, ubY+ubH/2, canUndo ? COL_GOLD : COL_DIM, Font::Md);
                }
            }

            if (!gMiiStatsMsg.empty())
                DrawTextC(gMiiStatsMsg, cx2, panelTop+14, gMiiStatsMsgCol);
        }
    }

    DrawFooter(
        gMiiStatsSubTab==MiiStatsSubTab::Social
            ? (gSocialExpanded ? "Y  sub-tab    ZL/ZR  prev/next mii    X  focus view    B  save and go back    +  discard"
                               : "Y  sub-tab    ZL/ZR  prev/next mii    X  global graph    B  save and go back    +  discard")
        : gMiiStatsSubTab==MiiStatsSubTab::Words
            ? "Y  sub-tab    ZL/ZR  prev/next mii    Up/Down  slot    Left/Right  kind    A  text    X  pronunciation    B  back    +  discard"
        : gMiiStatsSubTab==MiiStatsSubTab::Relations
            ? "Y  sub-tab    ZL/ZR  prev/next mii    Up/Down  relation    Left/Right  type    B  back    +  discard"
        : "Y  sub-tab    ZL/ZR  prev/next mii    Up/Down  field    X  undo    B  save and go back    +  discard");
    SDL_RenderPresent(gRen);
}

static void DrawSaveWarning() {
    HitClear();
    SDL_SetRenderDrawColor(gRen, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(gRen);
    const int mW = 620, mH = 230;
    int mx = (SCREEN_W - mW) / 2, my = (SCREEN_H - mH) / 2;
    FillRect(mx, my, mW, mH, {30, 18, 18, 255});
    DrawRect(mx, my, mW, mH, COL_RED);
    DrawTextC("Warning", SCREEN_W/2, my+28, COL_RED, Font::Lg);
    DrawTextC("Modifying these values can break your game.", SCREEN_W/2, my+84, COL_TEXT, Font::Md);
    DrawTextC("Make a backup in the Backup tab before editing.", SCREEN_W/2, my+114, COL_DIM, Font::Sm);
    const int btnW = 200, btnH = 44;
    int bx = SCREEN_W/2 - btnW/2, by = my + mH - 60;
    bool wReady = (gSaveWarningCountdown <= 0);
    if (wReady) HitAdd(bx, by, btnW, btnH, TAckSaveWarning, 0);
    FillRect(bx, by, btnW, btnH, COL_SEL);
    DrawRect(bx, by, btnW, btnH, wReady ? COL_ACCENT : COL_BORDER);
    int wSecs = (gSaveWarningCountdown + 59) / 60;
    std::string wLabel = wReady ? "I understand" : ("I understand  (" + std::to_string(wSecs) + ")");
    DrawTextC(wLabel, SCREEN_W/2, by + btnH/2, wReady ? COL_ACCENT : COL_DIM, Font::Md);
    DrawFooter(wReady ? "A  I understand" : "");
    SDL_RenderPresent(gRen);
}

static void TAckBackYes(int) {
    gShowBackPrompt = false;
    if (gBackPromptIsMii && gMiiSavDirty) {
        std::string err = SaveEditor::Save(SAVE_MII_SAV, gMiiSav);
        if (err.empty()) { SaveMount::Commit(); gMiiSavDirty=false; }
    }
    if (!gBackPromptIsMii && gPlayerSavDirty) {
        std::string err = SaveEditor::Save(SAVE_PLAYER_SAV, gPlayerSav);
        if (err.empty()) { SaveMount::Commit(); gPlayerSavDirty=false; }
    }
    FreePreview(); HttpServer::Stop(); gLog.clear();
    gEntries.clear(); gMiis.clear();
    gPlayerSav=SaveEditor::SavFile{}; gMiiSav=SaveEditor::SavFile{};
    gPlayerSavDirty=false; gMiiSavDirty=false;
    memset(gPlayerUndoValid, 0, sizeof(gPlayerUndoValid));
    memset(gMiiUndoValid,    0, sizeof(gMiiUndoValid));
    SaveMount::Unmount();
    gSaveFeedbackFrames=90; gSaveFeedbackQuit=true;
    gScreen=Screen::SaveFeedback;
}
static void TAckBackNo(int) {
    gShowBackPrompt = false;
    FreePreview(); HttpServer::Stop(); gLog.clear();
    gEntries.clear(); gMiis.clear();
    gPlayerSav=SaveEditor::SavFile{}; gMiiSav=SaveEditor::SavFile{};
    gPlayerSavDirty=false; gMiiSavDirty=false;
    memset(gPlayerUndoValid, 0, sizeof(gPlayerUndoValid));
    memset(gMiiUndoValid,    0, sizeof(gMiiUndoValid));
    SaveMount::Unmount();
    gQuitApp = true;
}
static void TDismissThumbTip(int) {
    if (gThumbTipCountdown > 0) return;
    gShowThumbTip = false;
    gThumbTipSeen = true;
    SaveConfig();
}
static void DrawThumbTip() {
    HitClear();
    SDL_SetRenderDrawColor(gRen, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(gRen);

    const int mW = 680, mH = 230;
    int mx = (SCREEN_W - mW) / 2, my = (SCREEN_H - mH) / 2;
    FillRect(mx, my, mW, mH, {18, 22, 18, 255});
    DrawRect(mx, my, mW, mH, COL_GOLD);

    DrawTextC("About inventory thumbnails", SCREEN_W/2, my + 28, COL_GOLD, Font::Md);

    DrawTextC("Your texture was imported, but the inventory thumbnail", SCREEN_W/2, my + 68, COL_TEXT, Font::Sm);
    DrawTextC("is managed by the game itself and won't update on its own.", SCREEN_W/2, my + 88, COL_TEXT, Font::Sm);
    DrawTextC("To refresh it: open this item in Studio Workshop,", SCREEN_W/2, my + 116, COL_DIM, Font::Sm);
    DrawTextC("enter its texture editor, and re-save — the game", SCREEN_W/2, my + 136, COL_DIM, Font::Sm);
    DrawTextC("will automatically rebuild the thumbnail.", SCREEN_W/2, my + 156, COL_DIM, Font::Sm);

    const int btnW = 200, btnH = 38;
    int bx = SCREEN_W/2 - btnW/2, by = my + mH - 54;
    bool ready = (gThumbTipCountdown <= 0);
    SDL_Color btnBorder = ready ? COL_ACCENT : COL_BORDER;
    SDL_Color btnText   = ready ? COL_ACCENT : COL_DIM;
    if (ready) HitAdd(bx, by, btnW, btnH, TDismissThumbTip, 0);
    FillRect(bx, by, btnW, btnH, COL_PANEL);
    DrawRect(bx, by, btnW, btnH, btnBorder);
    int secs = (gThumbTipCountdown + 59) / 60;
    std::string label = ready ? "Got it" : ("Got it  (" + std::to_string(secs) + ")");
    DrawTextC(label, SCREEN_W/2, by + btnH/2, btnText, Font::Md);
    SDL_RenderPresent(gRen);
}
static void DrawBackPrompt() {
    HitClear();
    SDL_SetRenderDrawColor(gRen, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(gRen);
    const int mW=540, mH=200;
    int mx=(SCREEN_W-mW)/2, my=(SCREEN_H-mH)/2;
    FillRect(mx, my, mW, mH, {20,24,20,255});
    DrawRect(mx, my, mW, mH, COL_GOLD);
    DrawTextC("Save changes?", SCREEN_W/2, my+28, COL_GOLD, Font::Lg);
    DrawTextC("Unsaved edits will be lost if you choose No.", SCREEN_W/2, my+70, COL_DIM, Font::Sm);
    const int bW=180, bH=48, gap=30;
    HitAdd(mx+mW/2-bW-gap/2, my+mH-70, bW, bH, TAckBackYes, 0);
    FillRect(mx+mW/2-bW-gap/2, my+mH-70, bW, bH, COL_SEL);
    DrawRect(mx+mW/2-bW-gap/2, my+mH-70, bW, bH, COL_GREEN);
    DrawTextC("A  Save & Exit", mx+mW/2-gap/2-bW/2, my+mH-70+bH/2, COL_GREEN, Font::Md);
    HitAdd(mx+mW/2+gap/2, my+mH-70, bW, bH, TAckBackNo, 0);
    FillRect(mx+mW/2+gap/2, my+mH-70, bW, bH, COL_SEL);
    DrawRect(mx+mW/2+gap/2, my+mH-70, bW, bH, COL_RED);
    DrawTextC("B  Undo & Exit", mx+mW/2+gap/2+bW/2, my+mH-70+bH/2, COL_RED, Font::Md);
    DrawFooter("A  save & exit    B  undo & exit");
    SDL_RenderPresent(gRen);
}
static void TUndoPlayer(int) {
    if (!gPlayerUndoValid[gPlayerFieldSel]) return;
    const auto& fd = PLAYER_FIELDS[gPlayerFieldSel];
    uint32_t h = PFHash(fd);
    uint32_t v = (uint32_t)gPlayerUndoVal[gPlayerFieldSel];
    if (fd.isEnum)      SaveEditor::SetEnum(gPlayerSav, h, v);
    else if (fd.isSkinTone) SaveEditor::SetUInt(gPlayerSav, h, v);
    else if (fd.isUInt) SaveEditor::SetUInt(gPlayerSav, h, v);
    else if (fd.isRegion) SaveEditor::SetEnum(gPlayerSav, h, v);
    else                SaveEditor::SetAnyScalar(gPlayerSav, h, v);
    gPlayerUndoValid[gPlayerFieldSel] = false;
    gPlayerSavDirty = true;
}
static void TUndoMii(int) {
    if (!gMiiUndoValid[gMiiStatsFieldSel]) return;
    const auto& fd = MII_STATS_FIELDS[gMiiStatsFieldSel];
    uint32_t h = SaveEditor::Hash(fd.fieldName);
    int miiIdx = gMiis[gMiiStatsMiiSel].slot - 1;
    if (fd.isUInt) SaveEditor::SetUIntAt(gMiiSav, h, miiIdx, (uint32_t)gMiiUndoVal[gMiiStatsFieldSel]);
    else           SaveEditor::SetIntAt(gMiiSav, h, miiIdx, gMiiUndoVal[gMiiStatsFieldSel]);
    gMiiUndoValid[gMiiStatsFieldSel] = false;
    gMiiSavDirty = true;
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
                    if (!s_touch.dragging) {
                        HitFire((int)s_touch.startX, (int)s_touch.startY);
                        ProcessTouch((int)s_touch.startX, (int)s_touch.startY, kDown);
                    }
                    s_touch.active = false;
                }
            }
        }
        if (gQuitApp) break;

        // + quits globally, but let Player/MiiStats tabs intercept it first for dirty check
        bool plusOwned = gScreen == Screen::OnSwitch &&
                         (gOnSwitchMode == OnSwitchMode::Player || gOnSwitchMode == OnSwitchMode::MiiStats);
        if ((kDown&HidNpadButton_Plus) && !plusOwned) break;

        kDown |= gSimKDown;
        gSimKDown = 0;
        u64 kNav = NavRepeat(kDown, kHeld);

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
                       if (!gEntries.empty()) LoadPreview(gEntries[gEntrySel < (int)gEntries.size() ? gEntrySel : 0]);
                       if (!gThumbTipSeen) { gShowThumbTip=true; gThumbTipCountdown=120; } }
            }
            if (HttpServer::HasPendingCommit()) {
                LogINF("Committing save...");
                HttpServer::ClearPendingCommit();
                std::string cerr=SaveMount::Commit();
                if (cerr.empty()) LogOK("Saved"); else LogERR("Commit: "+cerr);
            }
            if (HttpServer::HasPendingMiiRefresh()) {
                HttpServer::ClearPendingMiiRefresh();
                gMiis = MiiManager::ListMiis();
                LogOK("Mii list updated");
            }
            // Thumb tip modal
            if (gShowThumbTip) {
                if (gThumbTipCountdown > 0) gThumbTipCountdown--;
                if (gThumbTipCountdown <= 0 && (kDown & HidNpadButton_A)) TDismissThumbTip(0);
                DrawThumbTip();
                break;
            }
            // Warning modal takes priority over all other input
            if (gShowSaveWarning) {
                if (gSaveWarningCountdown > 0) gSaveWarningCountdown--;
                if (gSaveWarningCountdown <= 0 && (kDown & HidNpadButton_A)) TAckSaveWarning(0);
                if (gShowSaveWarning) DrawSaveWarning();
                break;
            }
            // Back-prompt modal
            if (gShowBackPrompt) {
                if (kDown & HidNpadButton_A) TAckBackYes(0);
                if (kDown & HidNpadButton_B) TAckBackNo(0);
                if (gShowBackPrompt) DrawBackPrompt();
                break;
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
                // Tab switch (cycle: UGC → Mii → Player → MiiStats → WebUI → UGC)
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
                    gBrowseForMii=false; gBrowseForExportDir=false;
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
                        TextureProcessor::ConvertLinearToSrgb(img.pixels);
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
                    if (!gSaveWarningAcked) {
                        gSaveWarningTarget=OnSwitchMode::Player; gShowSaveWarning=true; gSaveWarningCountdown=120; break;
                    }
                    gOnSwitchMode=OnSwitchMode::Player;
                    if (!gPlayerSav.loaded) {
                        std::string err;
                        if (!SaveEditor::Load(SAVE_PLAYER_SAV, gPlayerSav, err))
                            gPlayerMsg="Load error: "+err;
                    }
                    gOnSwitchMsg=""; break;
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
                    gBrowseForMii=true; gBrowseForExportDir=false;
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
            } else if (gOnSwitchMode == OnSwitchMode::Player) {
                // Lazy-load
                if (!gPlayerSav.loaded) {
                    std::string err;
                    if (!SaveEditor::Load(SAVE_PLAYER_SAV, gPlayerSav, err))
                        gPlayerMsg = "Load error: " + err;
                }
                // Tab switch
                if (kDown&(HidNpadButton_L|HidNpadButton_ZL)){
                    if (gPlayerSavDirty) {
                        std::string err = SaveEditor::Save(SAVE_PLAYER_SAV, gPlayerSav);
                        if (err.empty()) { SaveMount::Commit(); gPlayerSavDirty=false; gPlayerMsg="saved"; }
                        else gPlayerMsg = "save error: "+err;
                    }
                    gOnSwitchMode=OnSwitchMode::Mii; gOnSwitchMsg=""; break;
                }
                if (kDown&(HidNpadButton_R|HidNpadButton_ZR)){
                    if (gPlayerSavDirty) {
                        std::string err = SaveEditor::Save(SAVE_PLAYER_SAV, gPlayerSav);
                        if (err.empty()) { SaveMount::Commit(); gPlayerSavDirty=false; }
                    }
                    gOnSwitchMode=OnSwitchMode::MiiStats;
                    if (!gMiiSav.loaded) {
                        std::string err;
                        if (!SaveEditor::Load(SAVE_MII_SAV, gMiiSav, err))
                            gMiiStatsMsg="Load error: "+err;
                    }
                    gOnSwitchMsg=""; break;
                }
                // Field navigation
                if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp))
                    gPlayerFieldSel = (gPlayerFieldSel>0) ? gPlayerFieldSel-1 : PLAYER_FIELD_COUNT-1;
                if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown))
                    gPlayerFieldSel = (gPlayerFieldSel+1<PLAYER_FIELD_COUNT) ? gPlayerFieldSel+1 : 0;
                // Edit via callbacks
                if (gPlayerSav.loaded) {
                    const auto& fd = PLAYER_FIELDS[gPlayerFieldSel];
                    uint32_t h = PFHash(fd);

                    // Numeric nudge: UInt, Int, RawEnum
                    bool isNumeric = fd.isUInt || fd.isInt || fd.isRawEnum;
                    if (isNumeric) {
                        int scale = gTouchScale; gTouchScale = 1;
                        auto getV = [&]() -> uint32_t {
                            return fd.isUInt ? SaveEditor::GetUInt(gPlayerSav, h)
                                             : SaveEditor::GetAnyScalar(gPlayerSav, h);
                        };
                        auto setV = [&](uint32_t v) {
                            if (fd.isUInt) SaveEditor::SetUInt(gPlayerSav, h, v);
                            else           SaveEditor::SetAnyScalar(gPlayerSav, h, v);
                            gPlayerSavDirty = true;
                        };
                        auto adjustV = [&](int delta) {
                            uint32_t v = getV();
                            if (!gPlayerUndoValid[gPlayerFieldSel]) { gPlayerUndoVal[gPlayerFieldSel]=(int32_t)v; gPlayerUndoValid[gPlayerFieldSel]=true; }
                            if (fd.isInt) {
                                int32_t sv = (int32_t)v + delta;
                                if (fd.minVal != -1 && sv < fd.minVal) sv = fd.minVal;
                                if (fd.maxVal != -1 && sv > fd.maxVal) sv = fd.maxVal;
                                setV((uint32_t)sv);
                            } else {
                                if (delta < 0) {
                                    uint32_t ad=(uint32_t)(-delta);
                                    uint32_t nv=(v>=ad)?v-ad:0;
                                    if(fd.minVal>=0&&(int32_t)nv<fd.minVal)nv=(uint32_t)fd.minVal;
                                    setV(nv);
                                } else {
                                    uint32_t nv=v+(uint32_t)delta;
                                    if(fd.maxVal>=0&&(int32_t)nv>fd.maxVal)nv=(uint32_t)fd.maxVal;
                                    setV(nv);
                                }
                            }
                        };
                        if (gBtnTouchActive) {
                            gBtnTouchActive = false;
                            if (kDown&HidNpadButton_Left)  adjustV(-scale);
                            if (kDown&HidNpadButton_Right) adjustV( scale);
                        } else {
                            if (kNav&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft))
                                gPlayerBtnSel = (gPlayerBtnSel-1+8)%8;
                            if (kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight))
                                gPlayerBtnSel = (gPlayerBtnSel+1)%8;
                            if (kDown&HidNpadButton_A) {
                                static const int PSTEPS[]={1000,100,10,1};
                                int pdelta = (gPlayerBtnSel<4) ? -(int)PSTEPS[gPlayerBtnSel] : (int)PSTEPS[7-gPlayerBtnSel];
                                adjustV(pdelta);
                            }
                        }
                    }
                    // Currency enum cycle
                    if (fd.isEnum) {
                        uint32_t cur = SaveEditor::GetEnum(gPlayerSav, h);
                        int idx = CurrencyIndex(cur);
                        if (idx < 0) idx = 0;
                        if (kNav&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft)) {
                            if (!gPlayerUndoValid[gPlayerFieldSel]) { gPlayerUndoVal[gPlayerFieldSel]=(int32_t)cur; gPlayerUndoValid[gPlayerFieldSel]=true; }
                            int nidx = (idx > 0) ? idx-1 : CURRENCY_LABEL_COUNT-1;
                            SaveEditor::SetEnum(gPlayerSav, h, CURRENCY_LABELS[nidx].hash);
                            gPlayerSavDirty=true;
                        }
                        if (kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight)) {
                            if (!gPlayerUndoValid[gPlayerFieldSel]) { gPlayerUndoVal[gPlayerFieldSel]=(int32_t)cur; gPlayerUndoValid[gPlayerFieldSel]=true; }
                            int nidx = (idx+1 < CURRENCY_LABEL_COUNT) ? idx+1 : 0;
                            SaveEditor::SetEnum(gPlayerSav, h, CURRENCY_LABELS[nidx].hash);
                            gPlayerSavDirty=true;
                        }
                    }
                    // Region enum cycle
                    if (fd.isRegion) {
                        uint32_t cur = SaveEditor::GetEnum(gPlayerSav, h);
                        int ridx = 0;
                        for (int ri=0; ri<REGION_OPTION_COUNT; ri++)
                            if (SaveEditor::Hash(REGION_OPTIONS[ri].name)==cur) { ridx=ri; break; }
                        if (kNav&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft)) {
                            if (!gPlayerUndoValid[gPlayerFieldSel]) { gPlayerUndoVal[gPlayerFieldSel]=(int32_t)cur; gPlayerUndoValid[gPlayerFieldSel]=true; }
                            int nidx = (ridx-1+REGION_OPTION_COUNT) % REGION_OPTION_COUNT;
                            SaveEditor::SetEnum(gPlayerSav, h, SaveEditor::Hash(REGION_OPTIONS[nidx].name));
                            gPlayerSavDirty=true;
                        }
                        if (kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight)) {
                            if (!gPlayerUndoValid[gPlayerFieldSel]) { gPlayerUndoVal[gPlayerFieldSel]=(int32_t)cur; gPlayerUndoValid[gPlayerFieldSel]=true; }
                            int nidx = (ridx+1) % REGION_OPTION_COUNT;
                            SaveEditor::SetEnum(gPlayerSav, h, SaveEditor::Hash(REGION_OPTIONS[nidx].name));
                            gPlayerSavDirty=true;
                        }
                    }
                    // Skin tone cycle
                    if (fd.isSkinTone) {
                        uint32_t cur = SaveEditor::GetUInt(gPlayerSav, h);
                        if (kNav&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft)) {
                            if (!gPlayerUndoValid[gPlayerFieldSel]) { gPlayerUndoVal[gPlayerFieldSel]=(int32_t)cur; gPlayerUndoValid[gPlayerFieldSel]=true; }
                            SaveEditor::SetUInt(gPlayerSav, h, (cur>0)?cur-1:5); gPlayerSavDirty=true;
                        }
                        if (kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight)) {
                            if (!gPlayerUndoValid[gPlayerFieldSel]) { gPlayerUndoVal[gPlayerFieldSel]=(int32_t)cur; gPlayerUndoValid[gPlayerFieldSel]=true; }
                            SaveEditor::SetUInt(gPlayerSav, h, (cur<5)?cur+1:0); gPlayerSavDirty=true;
                        }
                    }
                    // Island size cycle (safe presets 1-4 only)
                    if (fd.isIslandSz) {
                        uint32_t cur = SaveEditor::GetAnyScalar(gPlayerSav, h);
                        if (cur<1||cur>4) cur=1;
                        if (kNav&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft)) {
                            if (!gPlayerUndoValid[gPlayerFieldSel]) { gPlayerUndoVal[gPlayerFieldSel]=(int32_t)cur; gPlayerUndoValid[gPlayerFieldSel]=true; }
                            SaveEditor::SetAnyScalar(gPlayerSav, h, (cur>1)?cur-1:4); gPlayerSavDirty=true;
                        }
                        if (kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight)) {
                            if (!gPlayerUndoValid[gPlayerFieldSel]) { gPlayerUndoVal[gPlayerFieldSel]=(int32_t)cur; gPlayerUndoValid[gPlayerFieldSel]=true; }
                            SaveEditor::SetAnyScalar(gPlayerSav, h, (cur<4)?cur+1:1); gPlayerSavDirty=true;
                        }
                    }
                    // A = keyboard for string fields
                    if (fd.isStr && (kDown&HidNpadButton_A)) {
                        std::string cur = SaveEditor::GetWStr32(gPlayerSav, h);
                        std::string nv = ShowKeyboard(fd.label, cur, 30);
                        if (nv != cur) { SaveEditor::SetWStr32(gPlayerSav, h, nv); gPlayerSavDirty=true; }
                        gPlayerMsg = ""; gPlayerMsgCol = COL_TEXT;
                    }
                    // X = undo
                    if (!fd.isStr && (kDown&HidNpadButton_X)) TUndoPlayer(0);
                }
                // B = save and go back
                if (kDown&HidNpadButton_B) {
                    bool wasDirty = gPlayerSavDirty;
                    if (wasDirty) {
                        std::string err = SaveEditor::Save(SAVE_PLAYER_SAV, gPlayerSav);
                        if (!err.empty()) { gPlayerMsg="save error: "+err; goto player_b_done; }
                        SaveMount::Commit(); gPlayerSavDirty=false;
                    }
                    FreePreview(); HttpServer::Stop(); gLog.clear();
                    gEntries.clear(); gMiis.clear();
                    gPlayerSav=SaveEditor::SavFile{}; gMiiSav=SaveEditor::SavFile{};
                    gPlayerSavDirty=false; gMiiSavDirty=false;
                    SaveMount::Unmount();
                    if (wasDirty) { gSaveFeedbackFrames=90; gSaveFeedbackQuit=false; gScreen=Screen::SaveFeedback; }
                    else { gScreen=Screen::UserPick; }
                    player_b_done:;
                }
                // Plus = exit (shows prompt if dirty, exits directly if clean)
                if (kDown&HidNpadButton_Plus) {
                    if (gPlayerSavDirty) { gBackPromptIsMii=false; gShowBackPrompt=true; }
                    else {
                        FreePreview(); HttpServer::Stop(); gLog.clear();
                        gEntries.clear(); gMiis.clear();
                        gPlayerSav=SaveEditor::SavFile{}; gMiiSav=SaveEditor::SavFile{};
                        SaveMount::Unmount();
                        gScreen=Screen::UserPick;
                    }
                }

            } else if (gOnSwitchMode == OnSwitchMode::MiiStats) {
                // Lazy-load
                if (!gMiiSav.loaded) {
                    std::string err;
                    if (!SaveEditor::Load(SAVE_MII_SAV, gMiiSav, err))
                        gMiiStatsMsg = "Load error: " + err;
                }
                // Tab switch (L/R for main tabs, ZL/ZR for Stats/Social sub-tabs)
                if (kDown&HidNpadButton_L){
                    if (gMiiSavDirty) {
                        std::string err = SaveEditor::Save(SAVE_MII_SAV, gMiiSav);
                        if (err.empty()) { SaveMount::Commit(); gMiiSavDirty=false; }
                    }
                    gOnSwitchMode=OnSwitchMode::Player; gOnSwitchMsg=""; break;
                }
                if (kDown&HidNpadButton_R){
                    if (gMiiSavDirty) {
                        std::string err = SaveEditor::Save(SAVE_MII_SAV, gMiiSav);
                        if (err.empty()) { SaveMount::Commit(); gMiiSavDirty=false; }
                    }
                    gOnSwitchMode=OnSwitchMode::WebUI; gOnSwitchMsg=""; break;
                }
                // Y cycles Stats → Words → Relations → Social → Stats
                if (kDown&HidNpadButton_Y) { gMiiStatsSubTab=(MiiStatsSubTab)(((int)gMiiStatsSubTab+1)%4); gSocialScroll=0; if(gMiiStatsSubTab!=MiiStatsSubTab::Social) gSocialExpanded=false; }
                // X toggles global graph in Social sub-tab only
                if (kDown&HidNpadButton_X && gMiiStatsSubTab==MiiStatsSubTab::Social) gSocialExpanded=!gSocialExpanded;
                // ZL/ZR switch selected mii
                if (!gMiis.empty()) {
                    int mseVis2=(SCREEN_H-SE_TOP_Y-32)/ITEM_H;
                    auto prevMii = [&](){
                        gMiiStatsMiiSel = (gMiiStatsMiiSel>0) ? gMiiStatsMiiSel-1 : (int)gMiis.size()-1;
                        if (gMiiStatsMiiSel < gMiiStatsScroll) gMiiStatsScroll=gMiiStatsMiiSel;
                    };
                    auto nextMii = [&](){
                        gMiiStatsMiiSel = (gMiiStatsMiiSel+1<(int)gMiis.size()) ? gMiiStatsMiiSel+1 : 0;
                        if (gMiiStatsMiiSel >= gMiiStatsScroll+mseVis2) gMiiStatsScroll=gMiiStatsMiiSel-mseVis2+1;
                    };
                    if (kDown&HidNpadButton_ZL) prevMii();
                    if (kDown&HidNpadButton_ZR) nextMii();
                    if (kNav&(HidNpadButton_StickLLeft|HidNpadButton_StickRLeft))  prevMii();
                    if (kNav&(HidNpadButton_StickLRight|HidNpadButton_StickRRight)) nextMii();
                }
                // D-pad Up/Down = field/slot nav depending on sub-tab
                if (gMiiStatsSubTab==MiiStatsSubTab::Stats) {
                    if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp))
                        gMiiStatsFieldSel = (gMiiStatsFieldSel>0) ? gMiiStatsFieldSel-1 : MII_STATS_FIELD_COUNT-1;
                    if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown))
                        gMiiStatsFieldSel = (gMiiStatsFieldSel+1<MII_STATS_FIELD_COUNT) ? gMiiStatsFieldSel+1 : 0;
                } else if (gMiiStatsSubTab==MiiStatsSubTab::Words) {
                    if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp))
                        gMiiWordsSlotSel = (gMiiWordsSlotSel>0) ? gMiiWordsSlotSel-1 : 11;
                    if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown))
                        gMiiWordsSlotSel = (gMiiWordsSlotSel<11) ? gMiiWordsSlotSel+1 : 0;
                } else if (gMiiStatsSubTab==MiiStatsSubTab::Relations) {
                    if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp))
                        { if (gMiiRelSel > 0) gMiiRelSel--; }
                    if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown))
                        gMiiRelSel++;
                    // gMiiRelSel clamped to valid range in draw phase
                }
                // Edit (Words sub-tab)
                if (gMiiStatsSubTab==MiiStatsSubTab::Words && gMiiSav.loaded && !gMiis.empty() && gMiiStatsMiiSel<(int)gMiis.size()) {
                    int miiIdx = gMiis[gMiiStatsMiiSel].slot - 1;
                    static const uint32_t H_WKIND_I  = SaveEditor::Hash("Mii.MiiMisc.WordInfo.WordArray.WordKind");
                    static const uint32_t H_WTXT_I   = SaveEditor::Hash("Mii.MiiMisc.WordInfo.WordArray.WordText");
                    static const uint32_t H_WHOW_I   = SaveEditor::Hash("Mii.MiiMisc.WordInfo.WordArray.WordHowToCall");
                    static const uint32_t H_WREGION_I= SaveEditor::Hash("Mii.MiiMisc.WordInfo.WordArray.WordRegionLanguageID");
                    static const uint32_t H_WINV_I   = SaveEditor::Hash("Invalid");
                    static const uint32_t H_WUSEN_I  = SaveEditor::Hash("USen");
                    static const uint32_t H_WJPJA_I  = SaveEditor::Hash("JPja");
                    int wIdx = miiIdx * 12 + gMiiWordsSlotSel;
                    uint32_t curKind = SaveEditor::GetAnyEnumAt(gMiiSav, H_WKIND_I, wIdx, H_WINV_I);
                    int kIdx = WordKindIndex(curKind);
                    if (kIdx < 0) kIdx = 0;
                    // Left/Right: cycle kind
                    auto cycleKind = [&](int dir) {
                        int nk = (kIdx + dir + WORD_KIND_COUNT) % WORD_KIND_COUNT;
                        uint32_t nHash = SaveEditor::Hash(WORD_KIND_LIST[nk].name);
                        SaveEditor::SetAnyEnumAt(gMiiSav, H_WKIND_I, wIdx, nHash);
                        if (nk == 0) { // clearing → wipe text/pronunciation/region
                            SaveEditor::SetWStr64At(gMiiSav, H_WTXT_I, wIdx, "");
                            SaveEditor::SetWStr64At(gMiiSav, H_WHOW_I, wIdx, "");
                            SaveEditor::SetAnyEnumAt(gMiiSav, H_WREGION_I, wIdx, H_WJPJA_I);
                        } else if (curKind == H_WINV_I) {
                            // activating from empty: auto-set region to USen
                            SaveEditor::SetAnyEnumAt(gMiiSav, H_WREGION_I, wIdx, H_WUSEN_I);
                        }
                        gMiiSavDirty = true;
                    };
                    if (kDown&HidNpadButton_Left)  cycleKind(-1);
                    if (kDown&HidNpadButton_Right) cycleKind(+1);
                    // A: edit text
                    if (kDown&HidNpadButton_A) {
                        std::string cur = SaveEditor::GetWStr64At(gMiiSav, H_WTXT_I, wIdx);
                        std::string nv = ShowKeyboard("Word Text (max 63 chars)", cur, 63);
                        if (nv != cur) {
                            SaveEditor::SetWStr64At(gMiiSav, H_WTXT_I, wIdx, nv);
                            if (!nv.empty() && curKind == H_WINV_I) {
                                SaveEditor::SetAnyEnumAt(gMiiSav, H_WKIND_I, wIdx, SaveEditor::Hash("Phrase"));
                                SaveEditor::SetAnyEnumAt(gMiiSav, H_WREGION_I, wIdx, H_WUSEN_I);
                            }
                            gMiiSavDirty = true;
                        }
                        gMiiStatsMsg=""; gMiiStatsMsgCol=COL_TEXT;
                    }
                    // X: edit pronunciation
                    if (kDown&HidNpadButton_X) {
                        std::string cur = SaveEditor::GetWStr64At(gMiiSav, H_WHOW_I, wIdx);
                        std::string nv = ShowKeyboard("Pronunciation (empty = same as text)", cur, 63);
                        if (nv != cur) {
                            SaveEditor::SetWStr64At(gMiiSav, H_WHOW_I, wIdx, nv);
                            gMiiSavDirty = true;
                        }
                        gMiiStatsMsg=""; gMiiStatsMsgCol=COL_TEXT;
                    }
                }
                // Edit (Relations sub-tab)
                if (gMiiStatsSubTab==MiiStatsSubTab::Relations && gMiiSav.loaded && !gMiis.empty() && gMiiStatsMiiSel<(int)gMiis.size()) {
                    if (gMiiRelPairIdx >= 0) {
                        static const uint32_t H_BASE_RI  = 0x8b41897eu;
                        static const uint32_t H_METER_RI = 0x42c2fc2fu;
                        int myDir = gMiiRelSelfA ? gMiiRelPairIdx*2   : gMiiRelPairIdx*2+1;
                        int otDir = gMiiRelSelfA ? gMiiRelPairIdx*2+1 : gMiiRelPairIdx*2;
                        uint32_t curType = SaveEditor::GetEnumAt(gMiiSav, H_BASE_RI, myDir, 0x0784a8dcu);
                        int tIdx = 0;
                        for (int ti=0; ti<RELATION_TYPE_COUNT; ti++)
                            if (RELATION_TYPE_NAMES[ti].hash == curType) { tIdx=ti; break; }
                        auto cycleType = [&](int dir) {
                            int nk = (tIdx + dir + RELATION_TYPE_COUNT) % RELATION_TYPE_COUNT;
                            uint32_t nHash = RELATION_TYPE_NAMES[nk].hash;
                            SaveEditor::SetEnumAt(gMiiSav, H_BASE_RI, myDir, nHash);
                            SaveEditor::SetEnumAt(gMiiSav, H_BASE_RI, otDir, RelTypeCounterpart(nHash));
                            if (RelMeterFixed(nHash)) {
                                SaveEditor::SetIntAt(gMiiSav, H_METER_RI, myDir, 100);
                                SaveEditor::SetIntAt(gMiiSav, H_METER_RI, otDir, 100);
                            }
                            gMiiSavDirty = true;
                        };
                        if (kDown&HidNpadButton_Left)  cycleType(-1);
                        if (kDown&HidNpadButton_Right) cycleType(+1);
                    }
                }
                // Edit field (Stats sub-tab only)
                if (gMiiStatsSubTab==MiiStatsSubTab::Stats && gMiiSav.loaded && !gMiis.empty() && gMiiStatsMiiSel<(int)gMiis.size()) {
                    int miiIdx = gMiis[gMiiStatsMiiSel].slot - 1;
                    const auto& fd = MII_STATS_FIELDS[gMiiStatsFieldSel];
                    int scale = gTouchScale;
                    gTouchScale=1;
                    auto adjustInt = [&](int delta){
                        uint32_t h = SaveEditor::Hash(fd.fieldName);
                        int32_t v = SaveEditor::GetIntAt(gMiiSav, h, miiIdx);
                        if (!gMiiUndoValid[gMiiStatsFieldSel]) { gMiiUndoVal[gMiiStatsFieldSel]=v; gMiiUndoValid[gMiiStatsFieldSel]=true; }
                        v += delta;
                        if (fd.minVal!=-1 && v < fd.minVal) v=fd.minVal;
                        if (fd.maxVal!=-1 && v > fd.maxVal) v=fd.maxVal;
                        SaveEditor::SetIntAt(gMiiSav, h, miiIdx, v);
                        gMiiSavDirty=true;
                    };
                    auto adjustUInt = [&](int delta){
                        uint32_t h = SaveEditor::Hash(fd.fieldName);
                        uint32_t v = SaveEditor::GetUIntAt(gMiiSav, h, miiIdx);
                        if (!gMiiUndoValid[gMiiStatsFieldSel]) { gMiiUndoVal[gMiiStatsFieldSel]=(int32_t)v; gMiiUndoValid[gMiiStatsFieldSel]=true; }
                        int64_t nv = (int64_t)v + delta;
                        if (nv < 0) nv=0;
                        if (fd.minVal>=0 && nv < (int64_t)fd.minVal) nv=fd.minVal;
                        if (fd.maxVal!=-1 && nv > (int64_t)fd.maxVal) nv=fd.maxVal;
                        SaveEditor::SetUIntAt(gMiiSav, h, miiIdx, (uint32_t)nv);
                        gMiiSavDirty=true;
                    };
                    if (!fd.isStr) {
                        if (fd.isEnum) {
                            // cycle through FEELING_LABELS on Left/Right
                            uint32_t fh = SaveEditor::Hash(fd.fieldName);
                            uint32_t cur = SaveEditor::GetEnumAt(gMiiSav, fh, miiIdx);
                            int curIdx = 0;
                            for (int fi=0; fi<FEELING_LABEL_COUNT; fi++)
                                if (FEELING_LABELS[fi].hash==cur) { curIdx=fi; break; }
                            if (kDown&HidNpadButton_Left) {
                                if (!gMiiUndoValid[gMiiStatsFieldSel]) { gMiiUndoVal[gMiiStatsFieldSel]=(int32_t)cur; gMiiUndoValid[gMiiStatsFieldSel]=true; }
                                curIdx = (curIdx>0) ? curIdx-1 : FEELING_LABEL_COUNT-1;
                                SaveEditor::SetEnumAt(gMiiSav, fh, miiIdx, FEELING_LABELS[curIdx].hash);
                                gMiiSavDirty=true;
                            }
                            if (kDown&HidNpadButton_Right) {
                                if (!gMiiUndoValid[gMiiStatsFieldSel]) { gMiiUndoVal[gMiiStatsFieldSel]=(int32_t)cur; gMiiUndoValid[gMiiStatsFieldSel]=true; }
                                curIdx = (curIdx+1<FEELING_LABEL_COUNT) ? curIdx+1 : 0;
                                SaveEditor::SetEnumAt(gMiiSav, fh, miiIdx, FEELING_LABELS[curIdx].hash);
                                gMiiSavDirty=true;
                            }
                        } else {
                            if (gBtnTouchActive) {
                                // Touch button tapped — apply value change
                                gBtnTouchActive = false;
                                if (kDown&HidNpadButton_Left)  { if (fd.isUInt) adjustUInt(-scale); else adjustInt(-scale); }
                                if (kDown&HidNpadButton_Right) { if (fd.isUInt) adjustUInt(scale);  else adjustInt(scale);  }
                            } else {
                                // D-pad Left/Right: navigate button focus
                                if (kNav&HidNpadButton_Left)  gMiiBtnSel = (gMiiBtnSel-1+6)%6;
                                if (kNav&HidNpadButton_Right) gMiiBtnSel = (gMiiBtnSel+1)%6;
                                // A fires focused button
                                if (kDown&HidNpadButton_A && !fd.isStr) {
                                    static const int MSTEPS[]={100,10,1};
                                    int mdelta = (gMiiBtnSel<3) ? -MSTEPS[gMiiBtnSel] : MSTEPS[5-gMiiBtnSel];
                                    if (fd.isUInt) adjustUInt(mdelta); else adjustInt(mdelta);
                                }
                            }
                        }
                    }
                    if (fd.isStr && (kDown&HidNpadButton_A)) {
                        uint32_t h = SaveEditor::Hash(fd.fieldName);
                        std::string cur = SaveEditor::GetWStr32At(gMiiSav, h, miiIdx);
                        std::string nv = ShowKeyboard(fd.label, cur, 30);
                        if (nv != cur) { SaveEditor::SetWStr32At(gMiiSav, h, miiIdx, nv); gMiiSavDirty=true; }
                        gMiiStatsMsg=""; gMiiStatsMsgCol=COL_TEXT;
                    }
                    // X = undo
                    if (!fd.isStr && (kDown&HidNpadButton_X)) TUndoMii(0);
                }
                // B = save and go back
                if (kDown&HidNpadButton_B) {
                    bool wasDirty = gMiiSavDirty;
                    if (wasDirty) {
                        std::string err = SaveEditor::Save(SAVE_MII_SAV, gMiiSav);
                        if (!err.empty()) { gMiiStatsMsg="save error: "+err; goto miistats_b_done; }
                        SaveMount::Commit(); gMiiSavDirty=false;
                    }
                    FreePreview(); HttpServer::Stop(); gLog.clear();
                    gEntries.clear(); gMiis.clear();
                    gPlayerSav=SaveEditor::SavFile{}; gMiiSav=SaveEditor::SavFile{};
                    gPlayerSavDirty=false; gMiiSavDirty=false;
                    SaveMount::Unmount();
                    if (wasDirty) { gSaveFeedbackFrames=90; gSaveFeedbackQuit=false; gScreen=Screen::SaveFeedback; }
                    else { gScreen=Screen::UserPick; }
                    miistats_b_done:;
                }
                // Plus = exit (shows prompt if dirty, exits directly if clean)
                if (kDown&HidNpadButton_Plus) {
                    if (gMiiSavDirty) { gBackPromptIsMii=true; gShowBackPrompt=true; }
                    else {
                        FreePreview(); HttpServer::Stop(); gLog.clear();
                        gEntries.clear(); gMiis.clear();
                        gPlayerSav=SaveEditor::SavFile{}; gMiiSav=SaveEditor::SavFile{};
                        SaveMount::Unmount();
                        gScreen=Screen::UserPick;
                    }
                }

            } else { // WebUI tab
                // X = restart HTTP server
                if (kDown&HidNpadButton_X) {
                    HttpServer::Stop();
                    HttpServer::Start(HTTP_PORT, SAVE_UGC_PATH);
                    LogOK("WebUI restarted");
                }
                // Tab switch
                if (kDown&(HidNpadButton_L|HidNpadButton_ZL)){
                    if (!gSaveWarningAcked) {
                        gSaveWarningTarget=OnSwitchMode::MiiStats; gShowSaveWarning=true; gSaveWarningCountdown=120; break;
                    }
                    gOnSwitchMode=OnSwitchMode::MiiStats; gOnSwitchMsg="";
                    if (!gMiiSav.loaded) {
                        std::string err;
                        if (!SaveEditor::Load(SAVE_MII_SAV, gMiiSav, err))
                            gMiiStatsMsg="Load error: "+err;
                    }
                    break;
                }
                if (kDown&(HidNpadButton_R|HidNpadButton_ZR)){
                    gOnSwitchMode=OnSwitchMode::UGC; gOnSwitchMsg=""; break;
                }
                if (kDown&HidNpadButton_B){
                    FreePreview(); HttpServer::Stop(); gLog.clear();
                    gEntries.clear(); gMiis.clear();
                    gPlayerSav=SaveEditor::SavFile{}; gMiiSav=SaveEditor::SavFile{};
                    gPlayerSavDirty=false; gMiiSavDirty=false;
                    gScreen=Screen::UserPick; SaveMount::Unmount();
                }
            }
            // WebUI standby prevention: keep screen on while a client connected recently
            {
                bool onWebUI = (gOnSwitchMode == OnSwitchMode::WebUI);
                if (!onWebUI && gStandbyOff) {
                    appletSetMediaPlaybackState(false); gStandbyOff = false;
                } else if (onWebUI) {
                    u64 lastTick = HttpServer::LastConnectTick();
                    // 60-second window: armGetSystemTickFreq() = 19200000
                    bool recent = lastTick > 0 &&
                        (armGetSystemTick() - lastTick) < (u64)60 * armGetSystemTickFreq();
                    if (recent && !gStandbyOff) {
                        appletSetMediaPlaybackState(true); gStandbyOff = true;
                    } else if (!recent && gStandbyOff) {
                        appletSetMediaPlaybackState(false); gStandbyOff = false;
                    }
                }
                gPrevOnSwitchMode = gOnSwitchMode;
            }
            // Dispatch draw to correct function for current tab
            if (gShowSaveWarning) {
                DrawSaveWarning();
            } else if (gShowBackPrompt) {
                DrawBackPrompt();
            } else {
                switch (gOnSwitchMode) {
                    case OnSwitchMode::Player:   DrawPlayer();   break;
                    case OnSwitchMode::MiiStats: DrawMiiStats(); break;
                    default:                     DrawOnSwitch(); break;
                }
            }
            break;


        case Screen::SaveFeedback: {
            SDL_SetRenderDrawColor(gRen, COL_BG.r, COL_BG.g, COL_BG.b, 255);
            SDL_RenderClear(gRen);
            DrawTextC("Saved!", SCREEN_W/2, SCREEN_H/2 - 20, COL_GREEN, Font::Lg);
            DrawTextC("Changes saved successfully.", SCREEN_W/2, SCREEN_H/2 + 28, COL_DIM, Font::Md);
            SDL_RenderPresent(gRen);
            if (--gSaveFeedbackFrames <= 0) {
                if (gSaveFeedbackQuit) gQuitApp = true;
                else gScreen = Screen::UserPick;
            }
            continue;
        }

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
