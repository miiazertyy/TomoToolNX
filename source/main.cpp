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
#include "belongings_data.h"
#include "habits_data.h"
#include "u2net_infer.h"
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <ctime>
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

static void FillRect(int x,int y,int w,int h,SDL_Color c){
    SDL_SetRenderDrawColor(gRen,c.r,c.g,c.b,c.a);
    SDL_Rect r{x,y,w,h};SDL_RenderFillRect(gRen,&r);
}
static void DrawRect(int x,int y,int w,int h,SDL_Color c){
    SDL_SetRenderDrawColor(gRen,c.r,c.g,c.b,c.a);
    SDL_Rect r{x,y,w,h};SDL_RenderDrawRect(gRen,&r);
}






// ─── Screens ──────────────────────────────────────────────────────────────────

enum class Screen { AppletWarning, UpdateCheck, UpdateAvailable, Downloading, UserPick, BackupPrompt, BackingUp, Mounting, OnSwitch, SaveFeedback, Error, Settings };

static Screen      gScreen  = Screen::UpdateCheck;
static std::string gError;
static std::string gIP;
static std::vector<SaveMount::UserInfo> gUsers;
static int         gUserSel   = 0;
static bool        gBackupFull = false;  // true when MAX_BACKUPS slots are used
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
static int          gEntryScroll = 0;          // position in filtered list (top of visible window)
static std::string  gUgcFilter;                // active substring filter (lowercase)
static std::vector<int> gFilteredEntries;      // indices into gEntries that match the filter
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
static void TUgcOpenSearch(int);
static void TUgcClearSearch(int);
static void TSelBrowse(int idx);
static void TAdjust  (int delta); // encoded: negative=left, positive=right; |val| = scale
static void TUndoNameLang(int);
static void TUndoIslandLang(int);
static void TSelPlayerField(int idx);
static void TSetSkinTone(int idx);
static void TSetIslandSize(int val);
static void TSelMiiStatsMii(int idx);
static void TSelMiiStatsField(int idx);
static void TSelMiiWordsSlot(int idx);
static void TSelMiiRelation(int idx);
static void TRelMeterAdj(int delta);
static void TRelDateKbd(int idx);
static void TSetSocialView(int v);
static void TSelHabitCat(int c);
static void TSelHabitItem(int posInCat);
static void TToggleHabitOwn(int posInCat);
static void TBLSel(int idx);
static void TBLPickSel(int idx);
static void BlOpenPicker(int blSel, int miiIdx);
static void TAckSaveWarning(int);
static void TPlayerKbd(int);
static void TMiiStatsKbd(int);
static void DrawSaveWarning();
static void DrawFileBrowser();
static void TUndoPlayer(int);
static void TUndoMii(int);
static void TUndoWords(int);
static void DrawBackPrompt();
static void TAckBackYes(int);
static void TAckBackNo(int);
static void TDismissThumbTip(int);
static void DrawThumbTip();
static void SaveConfig();

// ─── Save editor state ────────────────────────────────────────────────────────
static SaveEditor::SavFile gPlayerSav, gMiiSav;
static bool   gPlayerSavDirty   = false, gMiiSavDirty = false, gUgcDirty = false, gWebUiDirty = false;
static int    gPlayerFieldSel   = 0;
static int    gPlayerScroll     = 0;
static int    gMiiStatsMiiSel   = 0; // index into gMiis
static int    gMiiStatsFieldSel = 0;
static int    gMiiStatsScroll   = 0;
static std::string gPlayerMsg, gMiiStatsMsg;
static SDL_Color   gPlayerMsgCol = COL_TEXT, gMiiStatsMsgCol = COL_TEXT;
static int         gMiiStatsMsgFrames = 0;  // auto-fade countdown for transient habit/edit messages

// Undo state (single-step undo per field)
static int32_t gPlayerUndoVal[16]   = {};
static bool    gPlayerUndoValid[16] = {};
static int32_t gNameLangUndo        = 0; static bool gNameLangUndoValid    = false;
static int32_t gIslandLangUndo      = 0; static bool gIslandLangUndoValid  = false;
static int32_t gMiiUndoVal[16]     = {};
static bool    gMiiUndoValid[16]   = {};

// Words tab undo — snapshot of the whole slot before the first edit
struct WordSlotUndo {
    uint32_t kind = 0;
    std::string text;
    std::string how;
    bool valid    = false;
};
static WordSlotUndo gWordUndo;

// Button focus (joystick navigation of +/- buttons)
static int  gPlayerBtnSel   = 3;     // 0-7 for 8 Player numeric buttons
static int  gMiiBtnSel      = 2;     // 0-5 for 6 MiiStats numeric buttons
static int  gMiiLtdBtnSel   = 0;     // 0=import .ltd  1=export .ltd (Name field)
static bool gMiiNameKbdReq  = false; // touch-triggered open of name keyboard
static bool gBtnTouchActive = false; // set by TAdjust (touch), cleared after use
static bool gRelDateKbdReq  = false;
static int  gRelDatePairIdx = -1;

// Back-prompt state
static bool gShowBackPrompt       = false;
static bool gBackPromptIsMii      = false;
static bool gBackPromptToUserPick = false; // true = save/discard→UserPick; false = save/discard→quit app
static bool gQuitApp              = false;

// Save-feedback state (brief "Saved!" screen after B or + save)
static int  gSaveFeedbackFrames = 0;
static bool gSaveFeedbackQuit   = false;

// Thumb-tip modal (shown once after first successful UGC import)
static bool gShowThumbTip      = false;
static int  gThumbTipCountdown = 0;  // frames remaining before button enables
static bool gThumbTipSeen      = false;

// Background-removal state
static bool gShowBgRemovePrompt = false;

static constexpr const char* kBgModelPath = "romfs:/u2netp.bin";
static int  gMaxBackups        = 8;

static int         gSettingsSel    = 0;
static std::string gSettingsMsg;
static SDL_Color   gSettingsMsgCol = COL_TEXT;

static bool                     gShowRestorePicker = false;
static std::vector<std::string> gRestoreList;
static int                      gRestoreSel        = 0;
static int                      gRestoreScroll     = 0;

static void TOpenSettings(int)      { gSettingsMsg = ""; gScreen = Screen::Settings; }
static void TSelSettingsItem(int i) { gSettingsSel = i; }
static void TRestorePickSel(int i)  { gRestoreSel  = i; }

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
    bool isRegion;   // Named region enum cycle (9 values)
    bool isLang;     // Named language enum cycle (preset labels)
    bool isRawEnum;  // Raw unsigned nudge (GetAnyScalar/SetAnyScalar)
    int32_t minVal, maxVal; // -1 = unclamped
    const char* desc;
};
static uint32_t PFHash(const SaveFieldDef& fd) {
    return fd.rawHash ? fd.rawHash : SaveEditor::Hash(fd.fieldName);
}
static const struct { const char* name; const char* label; } REGION_OPTIONS[] = {
    {"Japan",          "Japan"},
    {"Europe",         "Europe"},
    {"NorthAmerica",   "North America"},
    {"SouthAmericaN",  "S.America (N)"},
    {"SouthAmericaS",  "S.America (S)"},
    {"Australia",      "Australia/NZ"},
    {"Asia",           "HK/TW/Korea"},
    {"OthersN",        "Other (N)"},
    {"OthersS",        "Other (S)"},
};
static const int REGION_OPTION_COUNT = 9;
static const struct { const char* name; const char* label; } LANG_OPTIONS[] = {
    {"JPja",  "Japanese"},
    {"USen",  "English (US)"},
    {"EUen",  "English (EU)"},
    {"EUfr",  "French (EU)"},
    {"EUde",  "German (EU)"},
    {"EUes",  "Spanish (EU)"},
    {"EUit",  "Italian (EU)"},
    {"EUnl",  "Dutch (EU)"},
    {"CNzh",  "Chinese (Simplified)"},
    {"KRko",  "Korean"},
    {"TWzh",  "Chinese (Traditional)"},
};
static const int LANG_OPTION_COUNT = 11;
//                    label           fieldName                              rawHash      Str    UInt   Enum   Int    Skin   Island Region Lang   RawEn  min  max  desc
static const SaveFieldDef PLAYER_FIELDS[] = {
    {"Name",          "Player.Name",                           0,           true,  false, false, false, false, false, false, false, false, 0,   0,   "Your player's display name."},
    {"Island",        "Player.IslandName",                     0,           true,  false, false, false, false, false, false, false, false, 0,   0,   "The name of your island."},
    {"Money",         "Player.Money",                          0,           false, true,  false, false, false, false, false, false, false, 0,   -1,  "Current coin balance."},
    {"Currency",      "Player.Currency",                       0,           false, false, true,  false, false, false, false, false, false, 0,   -1,  "Regional currency symbol shown in-game."},
    {"Boot Count",    "Player.BootNum",                        0,           false, true,  false, false, false, false, false, false, false, 0,   -1,  "Number of times the game has been launched."},
    {"Skin Tone",     "Player.SkinColorIndex",                 0,           false, false, false, false, true,  false, false, false, false, 0,   5,   "Player hand/skin color (0-5)."},
    {"Bday Day",      "",                                      0xdb7786bbu, false, false, false, false, false, false, false, false, true,  1,   31,  "Birthday day of the month (1-31)."},
    {"Bday Month",    "",                                      0xc754bef3u, false, false, false, false, false, false, false, false, true,  1,   12,  "Birthday month (1-12)."},
    {"Bday Year",     "",                                      0x11996629u, false, false, false, true,  false, false, false, false, false, -1,  -1,  "Birthday year."},
    {"Island Size",   "",                                      0x870a807cu, false, false, false, false, false, true,  false, false, false, 1,   4,   "Island size preset. Other values corrupt the save!"},
    {"Fountain Lv",   "Liberation.FountainLevel",              0,           false, false, false, false, false, false, false, false, true,  0,   -1,  "Wishing fountain upgrade level."},
    {"Wishes",        "",                                      0xa32f7e47u, false, false, false, false, false, false, false, false, true,  0,   -1,  "Number of wishes made."},
    {"Region",        "Player.Region",                         0,           false, false, false, false, false, false, true,  false, false, 0,   -1,  "Player region (affects seasons and weather)."},
};
static const int PLAYER_FIELD_COUNT = 13;

// ─── Mii stats field table ─────────────────────────────────────────────────────
struct MiiStatsDef {
    const char* label;
    const char* fieldName;
    bool isStr;   // WStr32Array
    bool isUInt;  // UIntArray; otherwise IntArray or EnumArray
    bool isEnum;  // EnumArray
    bool isAction; // navigable action item (no fieldName used)
    int actionId;  // 1=import .ltd, 2=export .ltd
    int dispOffset; // add when displaying (level is stored 0-based, shown +1)
    int32_t minVal, maxVal;
    const char* desc;
};
static const MiiStatsDef MII_STATS_FIELDS[] = {
    {"Name",        "Mii.Name.Name",                       true, false,false, false,0, 0,  0,  0, "The Mii's in-game display name."},
    {"Level",       "Mii.MiiMisc.SatisfyInfo.Level",      false,false,false, false,0, 1,  0, -1, "Friendship level with the player."},
    {"Lv Meter",    "Mii.MiiMisc.SatisfyInfo.Meter",      false,false,false, false,0, 0,  0,100, "Progress bar toward the next level (0-100)."},
    {"Money",       "Mii.Belongings.Money",                false,true, false, false,0, 0,  0, -1, "Coins currently held by this Mii."},
    {"Bday Day",    "Mii.MiiMisc.BirthdayInfo.Day",       false,false,false, false,0, 0,  1, 31, "Birthday day of the month (1-31)."},
    {"Bday Month",  "Mii.MiiMisc.BirthdayInfo.Month",     false,false,false, false,0, 0,  1, 12, "Birthday month (1-12)."},
    {"Birth Year",  "Mii.MiiMisc.BirthdayInfo.Year",      false,false,false, false,0, 0, -1, -1, "Year of birth."},
    {"Direct Age",  "Mii.MiiMisc.BirthdayInfo.DirectAge", false,false,false, false,0, 0, -1, -1, "Age override. -1 means auto (from birthday)."},
    {"Activeness",  "Mii.CharacterParam.Activeness",       false,false,false, false,0, 0,  1, 10, "How active and energetic the Mii is (1-10)."},
    {"Audacity",    "Mii.CharacterParam.Audaciousness",    false,false,false, false,0, 0,  1, 10, "How bold and daring the Mii is (1-10)."},
    {"Commonsense", "Mii.CharacterParam.Commonsense",      false,false,false, false,0, 0,  1, 10, "How wise and sensible the Mii is (1-10)."},
    {"Gaiety",      "Mii.CharacterParam.Gaiety",           false,false,false, false,0, 0,  1, 10, "How cheerful and upbeat the Mii is (1-10)."},
    {"Sociability", "Mii.CharacterParam.Sociability",      false,false,false, false,0, 0,  1, 10, "How sociable and outgoing the Mii is (1-10)."},
    {"Bond Meter",  "Mii.MiiMisc.BondInfo.Meter",         false,false,false, false,0, 0,  0,100, "Strength of the bond with the player (0-100)."},
    {"Mood",        "Mii.Feeling.Type",                    false,false,true,  false,0, 0,  0, -1, "Current emotional state."},
    {"Fullness",    "Mii.MiiMisc.EatInfo.EatFullness",     false,false,false, false,0, 0,  0, -1, "How well-fed the Mii is. Can exceed 100 with certain items."},
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
    { 0x0784a8dcu, "Unknown"     }, { 0x7e3d1e46u, "Invalid"     },
    { 0x354a0515u, "Know"        }, { 0xba939a42u, "Friend"      },
    { 0x535e93a8u, "Ex-Friend"   }, { 0xc2d067a7u, "Couple"      },
    { 0xb7ce0c18u, "Lover"       }, { 0x7783d4c3u, "Ex-Lover"    },
    { 0xfe59f825u, "Divorce"     }, { 0xdcfc7603u, "Parent"      },
    { 0xe193c5a2u, "Child"       }, { 0x1918f808u, "SiblingOld"  },
    { 0x3b1d200au, "SiblingYng"  }, { 0x2fd9785bu, "Grandparent" },
    { 0x7e3cd550u, "Grandchild"  }, { 0x804172f3u, "Relative"    },
};
static const int RELATION_TYPE_COUNT = 16;
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
    if (hash==0x535e93a8u) return {160,180, 80,255}; // Ex-Friend → muted yellow-green
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

// Fit a UTF-8 string to maxW pixels wide using font f, appending "…" if truncated.
static std::string FitNodeName(const std::string& s, TTF_Font* f, int maxW) {
    const char* ELL = "\xE2\x80\xA6"; // U+2026 HORIZONTAL ELLIPSIS
    const std::string src = s.empty() ? "?" : s;
    if (!f) return src;
    int w = 0, h = 0;
    TTF_SizeUTF8(f, src.c_str(), &w, &h);
    if (w <= maxW) return src;
    const char* tail = TTF_GlyphIsProvided(f, 0x2026) ? ELL : "...";
    std::string t = src;
    while (!t.empty()) {
        // strip trailing bytes until we're at a UTF-8 codepoint boundary
        do { t.pop_back(); } while (!t.empty() && (uint8_t(t.back()) & 0xC0) == 0x80);
        std::string cand = t + tail;
        TTF_SizeUTF8(f, cand.c_str(), &w, &h);
        if (w <= maxW) return cand;
    }
    return tail;
}

// MiiStats sub-tab state
enum class MiiStatsSubTab { Stats=0, Belongings=1, Habits=2, Words=3, Relations=4, Social=5 };
static const int MII_SUBTAB_COUNT = 6;
static MiiStatsSubTab gMiiStatsSubTab  = MiiStatsSubTab::Stats;

// Habits sub-tab state
struct HabitHashes { uint32_t isOwn, isChecked, state; };
static HabitHashes gHabitHashes[HABIT_COUNT];
static uint32_t    gHabitStateOwnH        = 0;
static uint32_t    gHabitStateUnownH      = 0;
static uint32_t    gHabitStateNeverOwnedH = 0;
static bool        gHabitHashesReady      = false;
static int         gHabitCatSel           = 0; // category index (0..HABIT_CAT_COUNT-1)
static int         gHabitItemSel          = 0; // index within current category
static int         gHabitScroll           = 0; // first visible row within current category

static void EnsureHabitHashes() {
    if (gHabitHashesReady) return;
    for (int i = 0; i < HABIT_COUNT; i++) {
        std::string base = std::string("Mii.Belongings.HabitOwnInfo.") + HABITS[i].name;
        gHabitHashes[i].isOwn     = SaveEditor::Hash((base + ".IsOwn").c_str());
        gHabitHashes[i].isChecked = SaveEditor::Hash((base + ".IsChecked").c_str());
        gHabitHashes[i].state     = SaveEditor::Hash((base + ".State").c_str());
    }
    gHabitStateOwnH        = SaveEditor::Hash("Own");
    gHabitStateUnownH      = SaveEditor::Hash("Unown");
    gHabitStateNeverOwnedH = SaveEditor::Hash("NeverOwned");
    gHabitHashesReady = true;
}

// Returns the global indices of habits in the given category.
static std::vector<int> HabitsInCategory(int cat) {
    std::vector<int> out;
    out.reserve(16);
    for (int i = 0; i < HABIT_COUNT; i++)
        if (HABITS[i].category == cat) out.push_back(i);
    return out;
}
static bool           gSocialExpanded  = false;
static int            gSocialScroll    = 0;
static int            gBlSel           = 0;   // selected item in Belongings (0–24)
static int            gBlScroll        = 0;   // visual-row scroll for Belongings
static bool           gBlPickerOpen    = false;
static int            gBlPickerSel     = 0;          // index into gBlFiltered
static int            gBlPickerScroll  = 0;
static std::vector<uint32_t>    gBlPickerHashes;
static std::vector<const char*> gBlPickerLabels;
static std::string              gBlFilter;          // active substring filter (lowercase)
static std::vector<int>         gBlFiltered;        // indices into gBlPickerHashes/Labels
static int            gMiiWordsSlotSel = 0;   // 0-11: selected word slot
static int            gMiiRelSel       = 0;   // selected row in filtered relations list
static int            gMiiRelScroll    = 0;   // first visible row in filtered relations list
static int            gMiiRelCount     = 0;   // total filtered rows for the selected Mii (updated each draw)
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
        HidNpadButton_ZL    | HidNpadButton_ZR    |
        HidNpadButton_StickLUp   | HidNpadButton_StickLDown  |
        HidNpadButton_StickLLeft | HidNpadButton_StickLRight |
        HidNpadButton_StickRUp   | HidNpadButton_StickRDown  |
        HidNpadButton_StickRLeft | HidNpadButton_StickRRight;
    for (u64 m = MASK, bit; m; m &= ~bit) {
        bit = m & (u64)(-(s64)m);
        int idx = __builtin_ctzll(bit);
        if (kHeld & bit) {
            int f = ++s_navHold[idx];
            if (f > 15 && f % 2 == 0) result |= bit;
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
// TSelUgc defined after VISIBLE and LoadPreview are declared (below)
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
    if (idx>=0 && idx<(int)gMiis.size()) { gMiiStatsMiiSel=idx; gMiiRelSel=0; gMiiRelScroll=0; }
}
static void TSelMiiStatsField(int idx) { gMiiStatsFieldSel=idx; }
static void TSelMiiWordsSlot(int idx)  { gMiiWordsSlotSel=idx; gWordUndo.valid=false; }
static void TSelMiiRelation(int idx)   { gMiiRelSel=idx; }
static void TRelDateKbd(int idx)       { gRelDateKbdReq=true; gRelDatePairIdx=idx; }
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
static void TSelHabitCat(int c) {
    if (c >= 0 && c < HABIT_CAT_COUNT) {
        gHabitCatSel = c; gHabitItemSel = 0; gHabitScroll = 0;
    }
}
// Apply the same logic the controller's A button does, but inline so the touch path
// doesn't depend on cross-frame gSimKDown ordering.
static void TSelHabitItem(int posInCat) {
    gHabitItemSel = posInCat;
    if (!gMiiSav.loaded || gMiis.empty() || gMiiStatsMiiSel >= (int)gMiis.size()) return;
    EnsureHabitHashes();
    int miiIdx = gMiis[gMiiStatsMiiSel].slot - 1;
    std::vector<int> items = HabitsInCategory(gHabitCatSel);
    if (posInCat < 0 || posInCat >= (int)items.size()) return;
    int chosen = items[posInCat];
    bool wasActive = SaveEditor::GetBoolAt(gMiiSav, gHabitHashes[chosen].isChecked, miiIdx, false);
    if (wasActive) {
        // Tapping the active gold box again: unset it (allows touch users to clear without keyboard).
        SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[chosen].isChecked, miiIdx, false);
    } else {
        // Make this habit the active one in its category (exclusive).
        for (int idx : items) {
            bool active = (idx == chosen);
            SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[idx].isChecked, miiIdx, active);
            if (active) {
                SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[idx].isOwn, miiIdx, true);
                SaveEditor::SetEnumAt(gMiiSav, gHabitHashes[idx].state, miiIdx, gHabitStateOwnH);
            }
        }
    }
    gMiiSavDirty = true;
    gMiiStatsMsg = std::string(wasActive?"Cleared: ":"Active: ") + HABITS[chosen].label;
    gMiiStatsMsgCol = COL_GREEN;
    gMiiStatsMsgFrames = 120;
}
static void TToggleHabitOwn(int posInCat) {
    gHabitItemSel = posInCat;
    if (!gMiiSav.loaded || gMiis.empty() || gMiiStatsMiiSel >= (int)gMiis.size()) return;
    EnsureHabitHashes();
    int miiIdx = gMiis[gMiiStatsMiiSel].slot - 1;
    std::vector<int> items = HabitsInCategory(gHabitCatSel);
    if (posInCat < 0 || posInCat >= (int)items.size()) return;
    int chosen = items[posInCat];
    bool isOwn = SaveEditor::GetBoolAt(gMiiSav, gHabitHashes[chosen].isOwn, miiIdx, false);
    if (isOwn) {
        uint32_t st = SaveEditor::GetEnumAt(gMiiSav, gHabitHashes[chosen].state, miiIdx, gHabitStateNeverOwnedH);
        SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[chosen].isOwn,     miiIdx, false);
        SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[chosen].isChecked, miiIdx, false);
        if (st == gHabitStateOwnH)
            SaveEditor::SetEnumAt(gMiiSav, gHabitHashes[chosen].state, miiIdx, gHabitStateUnownH);
    } else {
        SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[chosen].isOwn, miiIdx, true);
        SaveEditor::SetEnumAt(gMiiSav, gHabitHashes[chosen].state,  miiIdx, gHabitStateOwnH);
    }
    gMiiSavDirty = true;
    gMiiStatsMsg = std::string(isOwn?"Removed: ":"Added: ") + HABITS[chosen].label;
    gMiiStatsMsgCol = COL_GREEN;
    gMiiStatsMsgFrames = 120;
}
static void TSetSocialView(int v) {
    gMiiStatsSubTab = (MiiStatsSubTab)v;
    gSocialScroll = 0;
    if (gMiiStatsSubTab != MiiStatsSubTab::Social) gSocialExpanded = false;
}
static void TBLSel(int idx) {
    gBlSel = idx;
    if (idx < 21 && !gMiis.empty() && gMiiStatsMiiSel < (int)gMiis.size())
        BlOpenPicker(idx, gMiis[gMiiStatsMiiSel].slot - 1);
}
static void TBLPickSel(int idx) {
    if (idx >= 0 && idx < (int)gBlFiltered.size()) {
        gBlPickerSel = idx;
        gSimKDown |= HidNpadButton_A; // confirm immediately on tap
    }
}

// Rebuild gBlFiltered from gBlPickerLabels using gBlFilter (case-insensitive substring).
// Empty filter → identity (all indices). Always at least keeps "(none)"/"(empty)" sentinel at index 0.
static void BlRebuildFilter() {
    gBlFiltered.clear();
    if (gBlFilter.empty()) {
        gBlFiltered.reserve(gBlPickerLabels.size());
        for (int i = 0; i < (int)gBlPickerLabels.size(); i++) gBlFiltered.push_back(i);
        return;
    }
    // Always include sentinel at index 0 so the user can still clear the slot
    if (!gBlPickerLabels.empty()) gBlFiltered.push_back(0);
    for (int i = 1; i < (int)gBlPickerLabels.size(); i++) {
        const char* lbl = gBlPickerLabels[i];
        if (!lbl) continue;
        // case-insensitive substring match
        bool match = false;
        const char* hay = lbl;
        const char* needle = gBlFilter.c_str();
        size_t nlen = gBlFilter.size();
        for (const char* p = hay; *p; p++) {
            bool eq = true;
            for (size_t k = 0; k < nlen; k++) {
                char a = p[k]; if (!a) { eq = false; break; }
                char b = needle[k];
                if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                if (a != b) { eq = false; break; }
            }
            if (eq) { match = true; break; }
        }
        if (match) gBlFiltered.push_back(i);
    }
}
static void TAckSaveWarning(int) {
    gSaveWarningAcked = true; gShowSaveWarning = false; SaveConfig(); HttpServer::SetSaveWarnAcked(true);
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
static void TMiiStatsKbd(int) { gMiiNameKbdReq = true; }

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

// ── UGC list filter ─────────────────────────────────────────────────────────
// Case-insensitive substring match against display name AND stem.
static bool UgcMatchesFilter(int gEntriesIdx) {
    if (gUgcFilter.empty()) return true;
    auto contains = [&](const std::string& hay) -> bool {
        size_t nlen = gUgcFilter.size();
        if (hay.size() < nlen) return false;
        for (size_t p = 0; p + nlen <= hay.size(); p++) {
            bool eq = true;
            for (size_t k = 0; k < nlen; k++) {
                char a = hay[p+k]; char b = gUgcFilter[k];
                if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                if (a != b) { eq = false; break; }
            }
            if (eq) return true;
        }
        return false;
    };
    const auto& e = gEntries[gEntriesIdx];
    std::string name = MiiManager::GetUgcName(e.stem);
    if (!name.empty() && contains(name)) return true;
    return contains(e.stem);
}

// Rebuild gFilteredEntries from gEntries using gUgcFilter.
// Ensures gEntrySel still points at a visible entry; if not, snaps to first one.
static void RebuildUgcFilter() {
    gFilteredEntries.clear();
    gFilteredEntries.reserve(gEntries.size());
    for (int i = 0; i < (int)gEntries.size(); i++)
        if (UgcMatchesFilter(i)) gFilteredEntries.push_back(i);

    bool selVisible = false;
    for (int i : gFilteredEntries) if (i == gEntrySel) { selVisible = true; break; }
    if (!selVisible && !gFilteredEntries.empty()) gEntrySel = gFilteredEntries[0];
    if (gEntryScroll < 0) gEntryScroll = 0;
    if (gEntryScroll > std::max(0, (int)gFilteredEntries.size() - 1))
        gEntryScroll = std::max(0, (int)gFilteredEntries.size() - 1);
}

// Find the position of gEntries index `realIdx` in gFilteredEntries; -1 if absent.
static int UgcFilterPos(int realIdx) {
    for (int p = 0; p < (int)gFilteredEntries.size(); p++)
        if (gFilteredEntries[p] == realIdx) return p;
    return -1;
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

    static const char* kUgcImportExts[] = {".png",".ltdf",".ltdc",".ltdg",".ltdi",".ltde",".ltdo",".ltdl",nullptr};

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
            bool keep = false;
            if (gBrowseForMii) {
                keep = lower.size() >= 4 && lower.compare(lower.size()-4, 4, ".ltd") == 0;
            } else if (!gBrowseForExportDir) {
                for (int ei = 0; kUgcImportExts[ei]; ei++) {
                    size_t el = strlen(kUgcImportExts[ei]);
                    if (lower.size() >= el && lower.compare(lower.size()-el, el, kUgcImportExts[ei]) == 0) { keep = true; break; }
                }
            }
            if (keep) files.push_back(name);
        }
    }
    closedir(d);

    std::sort(dirs.begin(), dirs.end());
    std::sort(files.begin(), files.end());

    if (gBrowseCurPath != "/") gBrowseEntries.push_back({"[..]", true});
    for (auto& dn : dirs)  gBrowseEntries.push_back({dn, true});
    for (auto& fn : files) gBrowseEntries.push_back({fn, false});
}
static void TOpenImportBrowser(int) {
    gBrowseForMii=true; gBrowseForExportDir=false;
    BrowseRefresh(gBrowseLtdPath);
    gShowFileBrowser=true;
}
static void TDoMiiExport(int) {
    if (gMiis.empty() || gMiiStatsMiiSel >= (int)gMiis.size()) return;
    int slot=gMiis[gMiiStatsMiiSel].slot;
    std::string fname=gMiis[gMiiStatsMiiSel].name+"_slot"+std::to_string(slot)+".ltd";
    for(auto& c:fname){
        if(c==' '||c=='/'||c=='\\'||c==':'||c=='*'||c=='?'||
           c=='"'||c=='<'||c=='>'||c=='|'||(uint8_t)c>127)
            c='_';
    }
    std::string outPath=gExportPath+"/"+fname;
    LogINF("Exporting Mii slot "+std::to_string(slot)+"...");
    std::string err=MiiManager::ExportMii(slot,outPath);
    if(!err.empty()){gMiiStatsMsg=err;gMiiStatsMsgCol=COL_RED;LogERR("Mii export failed: "+err);}
    else{gMiiStatsMsg="Exported: "+fname;gMiiStatsMsgCol=COL_GREEN;LogOK("Exported to: "+outPath);}
}

static void DoMiiImport(const std::string& ltdPath) {
    if (gMiis.empty()) return;
    int slot = gMiis[gMiiStatsMiiSel].slot;
    LogINF("Importing Mii slot "+std::to_string(slot)+"...");
    std::string err = MiiManager::ImportMii(slot, ltdPath);
    if (!err.empty()) { gMiiStatsMsg=err; gMiiStatsMsgCol=COL_RED; LogERR("Mii import failed: "+err); return; }
    // Reload gMiiSav so in-memory state reflects the imported Mii, then mark dirty
    // so the normal save/discard prompt will commit (or revert) on exit.
    gMiiSav = SaveEditor::SavFile{};
    std::string lerr;
    SaveEditor::Load(SAVE_MII_SAV, gMiiSav, lerr);
    gMiiSavDirty = true;
    gMiis = MiiManager::ListMiis();
    if (gMiiStatsMiiSel >= (int)gMiis.size()) gMiiStatsMiiSel=(int)gMiis.size()-1;
    gMiiStatsMsg="Mii imported — save or discard on exit"; gMiiStatsMsgCol=COL_GREEN;
    LogOK("Mii import OK: slot "+std::to_string(slot));
}

static void DoOnSwitchImport(const std::string& pngPath) {
    if (gEntries.empty()) return;
    // Copy all needed fields before gEntries is replaced by the rescan below
    const std::string stem    = gEntries[gEntrySel].stem;
    TextureProcessor::ImportOptions opts;
    opts.pngPath            = pngPath;
    opts.destStem           = gEntries[gEntrySel].directory()+"/"+stem;
    opts.writeCanvas        = gEntries[gEntrySel].hasCanvas();
    opts.writeThumb         = gEntries[gEntrySel].hasThumb();
    opts.thumbPath          = gEntries[gEntrySel].thumbPath;
    opts.noSrgb             = false;
    opts.originalUgctexPath = gEntries[gEntrySel].ugctexPath;

    LogINF("Importing "+stem+"...");
    std::string err = TextureProcessor::ImportPng(opts);
    if (!err.empty()) {
        gOnSwitchMsg=err; gOnSwitchMsgCol=COL_RED;
        LogERR("Import failed: "+err); return;
    }
    // Mark dirty so the normal save/discard prompt will commit (or revert) on exit.
    gUgcDirty = true;
    // Reload entry list and preview (invalidates any prior reference into gEntries)
    gEntries = UgcScanner::Scan(SAVE_UGC_PATH);
    MiiManager::LoadUgcNames();
    RebuildUgcFilter();
    if (gEntrySel >= (int)gEntries.size()) gEntrySel=(int)gEntries.size()-1;
    if (!gEntries.empty()) LoadPreview(gEntries[gEntrySel]);
    gOnSwitchMsg="Imported — save or discard on exit"; gOnSwitchMsgCol=COL_GREEN;
    LogOK("Import OK: "+stem);
}

static void DoUgcExportLtd() {
    if (gEntries.empty() || gEntrySel < 0 || gEntrySel >= (int)gEntries.size()) return;
    const std::string& stem = gEntries[gEntrySel].stem;
    static const char* kPfx[] = {"Food","Cloth","Goods","Interior","Exterior","MapObject","MapFloor"};
    static const char* kExt[] = {".ltdf",".ltdc",".ltdg",".ltdi",".ltde",".ltdo",".ltdl"};
    int kind = -1, slot = 0;
    if (stem.size() > 3 && stem.substr(0,3) == "Ugc") {
        std::string rest = stem.substr(3);
        for (int k = 0; k < 7; k++) {
            size_t pl = strlen(kPfx[k]);
            if (rest.size() > pl && rest.substr(0,pl) == kPfx[k]) {
                kind = k; slot = atoi(rest.substr(pl).c_str()); break;
            }
        }
    }
    if (kind < 0) { gOnSwitchMsg="Not a UGC item"; gOnSwitchMsgCol=COL_RED; return; }
    std::string outPath = gExportPath + "/" + stem;
    std::string err = MiiManager::ExportUgc(kind, slot + 1, outPath);
    if (!err.empty()) { gOnSwitchMsg=err; gOnSwitchMsgCol=COL_RED; LogERR("Export .ltd failed: "+err); return; }
    gOnSwitchMsg = "Exported: " + stem + kExt[kind];
    gOnSwitchMsgCol = COL_GREEN;
    LogOK("Exported .ltd: " + outPath + kExt[kind]);
}
static void TDoUgcExportLtd(int) { DoUgcExportLtd(); }

static void DoUgcImportLtd(const std::string& ltdPath) {
    if (gEntries.empty() || gEntrySel < 0 || gEntrySel >= (int)gEntries.size()) return;
    const std::string& stem = gEntries[gEntrySel].stem;
    static const char* kPfx[] = {"Food","Cloth","Goods","Interior","Exterior","MapObject","MapFloor"};
    static const char* kExt[] = {".ltdf",".ltdc",".ltdg",".ltdi",".ltde",".ltdo",".ltdl"};
    int kind = -1, slot = 0;
    if (stem.size() > 3 && stem.substr(0,3) == "Ugc") {
        std::string rest = stem.substr(3);
        for (int k = 0; k < 7; k++) {
            size_t pl = strlen(kPfx[k]);
            if (rest.size() > pl && rest.substr(0,pl) == kPfx[k]) {
                kind = k; slot = atoi(rest.substr(pl).c_str()); break;
            }
        }
    }
    if (kind < 0) { gOnSwitchMsg="Not a UGC slot"; gOnSwitchMsgCol=COL_RED; return; }
    // Validate that the file extension matches the expected kind
    std::string lower = ltdPath;
    for (auto& c : lower) c = (char)tolower((unsigned char)c);
    size_t el = strlen(kExt[kind]);
    if (lower.size() < el || lower.compare(lower.size()-el, el, kExt[kind]) != 0) {
        gOnSwitchMsg = std::string("Wrong file type for ") + kPfx[kind] + " slot (expected " + kExt[kind] + ")";
        gOnSwitchMsgCol = COL_RED; return;
    }
    LogINF("Importing .ltd: " + ltdPath + " → " + stem);
    std::string err = MiiManager::ImportUgc(kind, slot + 1, ltdPath, false);
    if (!err.empty()) { gOnSwitchMsg=err; gOnSwitchMsgCol=COL_RED; LogERR("Import .ltd failed: "+err); return; }
    gUgcDirty = true;
    gPlayerSavDirty = true;
    gEntries = UgcScanner::Scan(SAVE_UGC_PATH);
    RebuildUgcFilter();
    if (gEntrySel >= (int)gEntries.size()) gEntrySel = (int)gEntries.size()-1;
    if (!gEntries.empty()) LoadPreview(gEntries[gEntrySel]);
    gOnSwitchMsg = "Imported " + stem + " — save or discard on exit";
    gOnSwitchMsgCol = COL_GREEN;
    LogOK("Import .ltd OK: " + stem);
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

static void DrawRestorePicker() {
    HitClear();
    SDL_SetRenderDrawColor(gRen, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(gRen);
    DrawHeader("restore backup");

    if (gRestoreList.empty()) {
        DrawTextC("No backups found.", SCREEN_W/2, SCREEN_H/2 - 10, COL_DIM, Font::Lg);
        DrawFooter("B  back");
        return;
    }

    const int listW = 640, listX = (SCREEN_W - listW) / 2;
    const int rowH = 58, rowGap = 5;
    const int listY = 84;
    const int RVIS  = 7;

    for (int i = 0; i < RVIS; i++) {
        int idx = gRestoreScroll + i;
        if (idx >= (int)gRestoreList.size()) break;
        bool sel = (idx == gRestoreSel);
        int ry = listY + i * (rowH + rowGap);
        HitAdd(listX, ry, listW, rowH, TRestorePickSel, idx);
        FillRect(listX, ry, listW, rowH, sel ? COL_SEL : COL_PANEL);
        DrawRect(listX, ry, listW, rowH, sel ? COL_GOLD : COL_BORDER);

        std::string name = gRestoreList[idx];
        size_t sl = name.rfind('/');
        if (sl != std::string::npos) name = name.substr(sl + 1);
        if (name.size() > 5 && name.substr(0, 5) == "save_") name = name.substr(5);
        for (char& c : name) if (c == '_') c = ' ';

        DrawText(name, listX + 18, ry + rowH/2, sel ? COL_TEXT : COL_DIM, Font::Md);
        if (idx == 0)
            DrawText("newest", listX + listW - 110, ry + rowH/2, COL_DIM, Font::Sm);
        if (sel)
            DrawText("A  restore", listX + listW - 110, ry + rowH/2, COL_GOLD, Font::Sm);
    }

    if ((int)gRestoreList.size() > RVIS) {
        int total = (int)gRestoreList.size();
        int barAreaH = RVIS * (rowH + rowGap) - rowGap;
        int barH = std::max(16, barAreaH * RVIS / total);
        int barY = listY + barAreaH * gRestoreScroll / total;
        FillRect(listX + listW + 6, barY, 4, barH, COL_BORDER);
    }

    DrawFooter("Up/Down  navigate    A  restore    B  cancel");
}

static void DrawSettings() {
    HitClear();
    SDL_SetRenderDrawColor(gRen, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(gRen);

    if (gShowFileBrowser)    { DrawFileBrowser();    SDL_RenderPresent(gRen); return; }
    if (gShowRestorePicker)  { DrawRestorePicker();  SDL_RenderPresent(gRen); return; }

    DrawHeader("settings");

    static const struct { const char* label; const char* desc; } ITEMS[] = {
        { "Export Path",     "folder used when exporting textures" },
        { "Max Backups",     "maximum number of save backups to keep (1–99)" },
        { "Restore Backup",  "restore a previous save backup for the selected user" },
    };
    static const int ITEM_COUNT = 3;

    const int listW = 720, listX = (SCREEN_W - listW) / 2;
    const int rowH  = 72,  rowGap = 6;
    int listY = 72;

    for (int i = 0; i < ITEM_COUNT; i++) {
        bool sel = (i == gSettingsSel);
        int ry = listY + i * (rowH + rowGap);
        HitAdd(listX, ry, listW, rowH, TSelSettingsItem, i);
        FillRect(listX, ry, listW, rowH, sel ? COL_SEL : COL_PANEL);
        DrawRect(listX, ry, listW, rowH, sel ? COL_GOLD : COL_BORDER);

        DrawText(ITEMS[i].label, listX + 16, ry + 12, sel ? COL_TEXT : COL_DIM, Font::Md);
        DrawText(ITEMS[i].desc,  listX + 16, ry + 42, COL_DIM, Font::Sm);

        if (i == 0) {
            // Export path — show current path right-aligned, "A  change" hint
            TTF_Font* fsm = GetFont(Font::Sm);
            int vw = 0, vh = 0;
            if (fsm) TTF_SizeUTF8(fsm, gExportPath.c_str(), &vw, &vh);
            int maxValW = listW / 2 - 20;
            std::string disp = gExportPath;
            while (fsm && vw > maxValW && disp.size() > 4) {
                disp = ".." + disp.substr(disp.size() - (disp.size() * 3 / 4));
                TTF_SizeUTF8(fsm, disp.c_str(), &vw, &vh);
            }
            DrawText(disp, listX + listW - vw - 16, ry + 14, sel ? COL_ACCENT : COL_DIM, Font::Sm);
            DrawText("A  change", listX + listW - 100, ry + 44, sel ? COL_GOLD : COL_DIM, Font::Sm);
        } else if (i == 2) {
            int cnt = BackupService::CountBackups();
            std::string v = (cnt == 0) ? "none" : std::to_string(cnt) + " available";
            DrawText(v,          listX + listW - 130, ry + 14, sel ? COL_ACCENT : COL_DIM, Font::Sm);
            DrawText("A  open",  listX + listW - 100, ry + 44, sel ? COL_GOLD   : COL_DIM, Font::Sm);
        } else if (i == 1) {
            // Max backups — left/right number selector
            const int arrowW = 36, numW = 48, ctrlH = 36;
            int ctrlX = listX + listW - arrowW - numW - arrowW - 20;
            int ctrlY = ry + (rowH - ctrlH) / 2;
            // Left arrow
            HitAdd(ctrlX, ctrlY, arrowW, ctrlH, TSimBtn, (int)HidNpadButton_Left);
            FillRect(ctrlX, ctrlY, arrowW, ctrlH, COL_PANEL);
            DrawRect(ctrlX, ctrlY, arrowW, ctrlH, sel ? COL_GOLD : COL_BORDER);
            DrawTextC("<", ctrlX + arrowW/2, ctrlY + ctrlH/2, sel ? COL_GOLD : COL_DIM, Font::Md);
            // Number
            FillRect(ctrlX + arrowW, ctrlY, numW, ctrlH, COL_PANEL);
            DrawRect(ctrlX + arrowW, ctrlY, numW, ctrlH, sel ? COL_GOLD : COL_BORDER);
            DrawTextC(std::to_string(gMaxBackups), ctrlX + arrowW + numW/2, ctrlY + ctrlH/2, sel ? COL_TEXT : COL_DIM, Font::Md);
            // Right arrow
            HitAdd(ctrlX + arrowW + numW, ctrlY, arrowW, ctrlH, TSimBtn, (int)HidNpadButton_Right);
            FillRect(ctrlX + arrowW + numW, ctrlY, arrowW, ctrlH, COL_PANEL);
            DrawRect(ctrlX + arrowW + numW, ctrlY, arrowW, ctrlH, sel ? COL_GOLD : COL_BORDER);
            DrawTextC(">", ctrlX + arrowW + numW + arrowW/2, ctrlY + ctrlH/2, sel ? COL_GOLD : COL_DIM, Font::Md);
        }
    }

    if (!gSettingsMsg.empty())
        DrawTextC(gSettingsMsg, SCREEN_W/2, listY + ITEM_COUNT*(rowH+rowGap) + 16,
                  gSettingsMsgCol, Font::Sm);

    DrawFooter("Up/Down  navigate    Left/Right  adjust    A  action    B  back");
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

    // Settings button — bottom right
    const int sBtnW = 170, sBtnH = 40;
    int sBtnX = SCREEN_W - sBtnW - 14;
    int sBtnY = SCREEN_H - 30 - sBtnH - 10;
    HitAdd(sBtnX, sBtnY, sBtnW, sBtnH, TOpenSettings, 0);
    FillRect(sBtnX, sBtnY, sBtnW, sBtnH, COL_PANEL);
    DrawRect(sBtnX, sBtnY, sBtnW, sBtnH, COL_BORDER);
    DrawTextC("X  Settings", sBtnX + sBtnW/2, sBtnY + sBtnH/2, COL_DIM, Font::Sm);

    DrawFooter("Left/Right  navigate    A  select    X  settings    +  quit");
    SDL_RenderPresent(gRen);
}

static void DrawBackupPrompt() {
    HitClear();
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);
    DrawHeader("");

    const int cw  = 460;
    const int ch  = 148;
    const int gap = 20;
    const int cx  = SCREEN_W/2 - (2*cw + gap)/2;
    const int cy  = SCREEN_H/2 - ch/2 + 10;
    int x1=cx, x2=cx+cw+gap;

    HitAdd(x1,cy,cw,ch, TSimBtn, (int)HidNpadButton_A);
    HitAdd(x2,cy,cw,ch, TSimBtn, (int)HidNpadButton_B);

    int curCount = BackupService::CountBackups();
    if (gBackupFull) {
        char fullMsg[64];
        snprintf(fullMsg, sizeof(fullMsg), "%d/%d backups — erase oldest to make room?", curCount, gMaxBackups);
        DrawTextC(fullMsg, SCREEN_W/2, 44, COL_DIM, Font::Md);
        FillRect(x1,cy,cw,ch,COL_PANEL); DrawRect(x1,cy,cw,ch,COL_RED);
        DrawTextC("[A]",                    x1+cw/2, cy+36,  COL_RED,  Font::Lg);
        DrawTextC("erase oldest + backup",  x1+cw/2, cy+86,  COL_TEXT, Font::Md);
        DrawTextC("removes the oldest backup", x1+cw/2, cy+116, COL_DIM, Font::Sm);
        DrawFooter("A  erase oldest + backup    B  skip    +  quit");
    } else {
        DrawTextC("backup save data?", SCREEN_W/2, 44, COL_DIM, Font::Md);
        FillRect(x1,cy,cw,ch,COL_PANEL); DrawRect(x1,cy,cw,ch,COL_ACCENT);
        DrawTextC("[A]",         x1+cw/2, cy+36,  COL_ACCENT, Font::Lg);
        DrawTextC("backup now",  x1+cw/2, cy+86,  COL_TEXT,   Font::Md);
        DrawTextC("creates a timestamped backup", x1+cw/2, cy+116, COL_DIM, Font::Sm);
        DrawFooter("A  backup    B  skip    +  quit");
    }

    FillRect(x2,cy,cw,ch,COL_PANEL); DrawRect(x2,cy,cw,ch,COL_RED);
    DrawTextC("[B]",         x2+cw/2, cy+36,  COL_RED,  Font::Lg);
    DrawTextC("skip backup", x2+cw/2, cy+86,  COL_TEXT, Font::Md);
    DrawTextC("continue without backup", x2+cw/2, cy+116, COL_DIM, Font::Sm);

    // tip: current / max backups
    char tipBuf[32];
    snprintf(tipBuf, sizeof(tipBuf), "%d / %d backups", curCount, gMaxBackups);
    DrawTextC(tipBuf, SCREEN_W/2, cy+ch+18, COL_DIM, Font::Sm);

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
        // Scroll position is in filtered space; keep selected entry visible
        int p = UgcFilterPos(gEntrySel);
        if (p >= 0) {
            if (p >= gEntryScroll+VISIBLE) gEntryScroll = p - VISIBLE + 1;
            if (p <  gEntryScroll)         gEntryScroll = p;
        }
        LoadPreview(gEntries[gEntrySel]);
    }
}

static void TUgcOpenSearch(int) {
    std::string q = ShowKeyboard("Filter textures (substring)", gUgcFilter, 30);
    for (auto& c : q) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    gUgcFilter = q;
    RebuildUgcFilter();
    gEntryScroll = 0;
    if (!gFilteredEntries.empty() && UgcFilterPos(gEntrySel) < 0) {
        gEntrySel = gFilteredEntries[0];
        if (!gEntries.empty()) LoadPreview(gEntries[gEntrySel]);
    }
}
static void TUgcClearSearch(int) {
    gUgcFilter.clear();
    RebuildUgcFilter();
    gEntryScroll = 0;
    int p = UgcFilterPos(gEntrySel);
    if (p >= 0) {
        if (p >= gEntryScroll+VISIBLE) gEntryScroll = p - VISIBLE + 1;
        if (p <  gEntryScroll)         gEntryScroll = p;
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
        if (key == "max_backups")    { int v = atoi(val.c_str()); if (v >= 1 && v <= 99) gMaxBackups = v; }
    }
    fclose(f);
}

static void SaveConfig() {
    FILE* f = fopen(CONFIG_PATH, "w");
    if (!f) return;
    fprintf(f, "export_path=%s\n",     gExportPath.c_str());
    fprintf(f, "thumb_tip_seen=%d\n",  gThumbTipSeen     ? 1 : 0);
    fprintf(f, "save_warn_acked=%d\n", gSaveWarningAcked ? 1 : 0);
    fprintf(f, "max_backups=%d\n",     gMaxBackups);
    fclose(f);
}

static void DrawFileBrowser() {
    HitClear();
    FillRect(40,36,SCREEN_W-80,SCREEN_H-66,COL_PANEL);
    DrawRect(40,36,SCREEN_W-80,SCREEN_H-66,COL_BORDER);
    if (gBrowseForExportDir)
        DrawTextC("select export folder", SCREEN_W/2, 52, COL_GOLD, Font::Md);
    else
        DrawTextC(gBrowseForMii ? "select .ltd" : "select PNG or .ltd", SCREEN_W/2, 52, COL_DIM, Font::Md);
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
        case OnSwitchMode::Player:   subtitle="player"; break;
        case OnSwitchMode::MiiStats: subtitle="mii"; break;
        default:                     subtitle="webui"; break;
    }
    DrawHeader(subtitle);

    // Tab bar — 4 tabs at 140px each
    {
        int tw=140, th=22, gap=4;
        int totalW=tw*4+gap*3, tx=SCREEN_W/2-totalW/2, ty=28;
        struct { const char* label; OnSwitchMode mode; } tabs[]={
            {"webui",    OnSwitchMode::WebUI},
            {"textures", OnSwitchMode::UGC},
            {"mii",      OnSwitchMode::MiiStats},
            {"player",   OnSwitchMode::Player},
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
        // Search bar — standalone, sits above the list panel with a clear gap
        const int SEARCH_H = 30;
        const int SEARCH_GAP = 10;
        int sbX = LIST_X, sbY = LIST_Y, sbW = LIST_W;
        bool searchActive = !gUgcFilter.empty();
        // Hit-area: tap to open keyboard; clear button hits separately when active
        int clearW = searchActive ? 28 : 0;
        HitAdd(sbX, sbY, sbW - clearW, SEARCH_H, TUgcOpenSearch, 0);
        FillRect(sbX, sbY, sbW, SEARCH_H, COL_BG);
        DrawRect(sbX, sbY, sbW, SEARCH_H, searchActive ? COL_ACCENT : COL_BORDER);
        TTF_Font* fsm0 = GetFont(Font::Sm);
        int sh = 0;
        if (searchActive) {
            std::string lbl = std::string("Filter:  ") + gUgcFilter;
            int tw0=0; if (fsm0) TTF_SizeUTF8(fsm0, lbl.c_str(), &tw0, &sh);
            DrawText(lbl, sbX+8, sbY+(SEARCH_H-sh)/2, COL_ACCENT);
            // Clear (✕) button
            HitAdd(sbX+sbW-clearW, sbY, clearW, SEARCH_H, TUgcClearSearch, 0);
            DrawRect(sbX+sbW-clearW, sbY, clearW, SEARCH_H, COL_BORDER);
            DrawTextC("X", sbX+sbW-clearW/2, sbY+SEARCH_H/2, COL_DIM, Font::Md);
        } else {
            const char* placeholder = "Tap or  -  to search...";
            int tw0=0; if (fsm0) TTF_SizeUTF8(fsm0, placeholder, &tw0, &sh);
            DrawText(placeholder, sbX+8, sbY+(SEARCH_H-sh)/2, COL_DIM);
        }

        // List panel below the search bar (with a clear gap so the two read as separate)
        int listPanelY = LIST_Y + SEARCH_H + SEARCH_GAP;
        int listPanelH = LIST_H - SEARCH_H - SEARCH_GAP;
        FillRect(LIST_X-4, listPanelY, LIST_W+8, listPanelH+4, COL_PANEL);
        DrawRect(LIST_X-4, listPanelY, LIST_W+8, listPanelH+4, COL_BORDER);

        const int LIST_TOP_Y = listPanelY + 4;
        const int LIST_VIS_H = listPanelH - 4;
        const int LIST_VISIBLE = LIST_VIS_H / ITEM_H;
        int totalFiltered = (int)gFilteredEntries.size();

        if (gEntryScroll < 0) gEntryScroll = 0;
        if (totalFiltered > 0 && gEntryScroll > totalFiltered - LIST_VISIBLE)
            gEntryScroll = std::max(0, totalFiltered - LIST_VISIBLE);

        for (int i = 0; i < LIST_VISIBLE; i++) {
            int filtPos = gEntryScroll + i;
            if (filtPos >= totalFiltered) break;
            int realIdx = gFilteredEntries[filtPos];
            bool sel = (realIdx == gEntrySel);
            int ry = LIST_TOP_Y + i*ITEM_H;
            HitAdd(LIST_X, ry, LIST_W, ITEM_H-1, TSelUgc, realIdx);
            FillRect(LIST_X, ry, LIST_W, ITEM_H-1, sel?COL_SEL:COL_BG);
            if (sel) DrawRect(LIST_X, ry, LIST_W, ITEM_H-1, COL_GOLD);
            std::string dn = MiiManager::GetUgcName(gEntries[realIdx].stem);
            if (dn.empty()) dn = FormatStem(gEntries[realIdx].stem);
            TTF_Font* f = GetFont(Font::Sm); int fw=0,fh=0; if(f)TTF_SizeUTF8(f,dn.c_str(),&fw,&fh);
            DrawText(dn, LIST_X+6, ry+(ITEM_H-fh)/2, sel?COL_TEXT:COL_DIM);
        }

        if (totalFiltered == 0 && !gEntries.empty()) {
            DrawTextC("no matches", LIST_X+LIST_W/2, LIST_TOP_Y + LIST_VIS_H/2, COL_DIM, Font::Md);
        }

        if (totalFiltered > LIST_VISIBLE) {
            int barH = listPanelH * LIST_VISIBLE / totalFiltered;
            int barY = listPanelY + listPanelH * gEntryScroll / totalFiltered;
            FillRect(LIST_X+LIST_W+2, barY, 4, barH, COL_BORDER);
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
            } else {
                DrawTextC("no preview", PREVIEW_X+PREVIEW_W/2, imgY+imgH/2, COL_BORDER);
            }
            if (!gEntries.empty()) {
                int cx   = PREVIEW_X + PREVIEW_W/2;
                int btnY = imgY + imgH + kBtnGap;
                const int btnW=155, btnGap=12, bgSep=36;
                int bx0  = cx - (4*btnW + 3*btnGap + bgSep)/2;
                int bxLtd = bx0 + 2*(btnW+btnGap);
                int bxBg  = bxLtd + btnW + btnGap + bgSep;
                // A: import
                FillRect(bx0,             btnY, btnW, kBtnH, COL_PANEL);
                DrawRect(bx0,             btnY, btnW, kBtnH, COL_ACCENT);
                DrawTextC("A  import", bx0+btnW/2, btnY+kBtnH/2, COL_ACCENT, Font::Md);
                // Y: export PNG
                FillRect(bx0+btnW+btnGap, btnY, btnW, kBtnH, COL_PANEL);
                DrawRect(bx0+btnW+btnGap, btnY, btnW, kBtnH, COL_GOLD);
                DrawTextC("Y  export pic", bx0+btnW+btnGap+btnW/2, btnY+kBtnH/2, COL_GOLD, Font::Md);
                // export .ltd (touch)
                FillRect(bxLtd,           btnY, btnW, kBtnH, COL_PANEL);
                DrawRect(bxLtd,           btnY, btnW, kBtnH, COL_GOLD);
                DrawTextC("export .ltd", bxLtd+btnW/2, btnY+kBtnH/2, COL_GOLD, Font::Md);
                HitAdd(bxLtd, btnY, btnW, kBtnH, TDoUgcExportLtd, 0);
                // X: remove BG (separated by gap)
                FillRect(bxBg,            btnY, btnW, kBtnH, COL_SEL);
                DrawRect(bxBg,            btnY, btnW, kBtnH, {90,140,200,255});
                DrawTextC("X  remove BG", bxBg+btnW/2, btnY+kBtnH/2, {90,140,200,255}, Font::Md);
            }
        }
        DrawFooter("ZL/ZR  scroll    Up/Down  select    A  import    Y  export pic    export .ltd  (touch)    X  remove BG    -  search    L/R  tab    B/+  back");
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

        DrawFooter("L/R  tab    X  restart server    B/+  back");
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
        int totalW=tw*4+gap*3, tx=SCREEN_W/2-totalW/2, ty=28;
        struct { const char* label; OnSwitchMode mode; } tabs[]={
            {"webui",OnSwitchMode::WebUI},{"textures",OnSwitchMode::UGC},
            {"mii",OnSwitchMode::MiiStats},{"player",OnSwitchMode::Player}};
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
    int midY = SE_TOP_Y + (SCREEN_H-SE_TOP_Y-32)/2 - 36;

    DrawTextC(fd.label, cx, midY-102, COL_DIM, Font::Md);
    DrawTextC(fd.desc,  cx, midY-76,  COL_DIM, Font::Sm);

    if (fd.isStr) {
        std::string val = SaveEditor::GetWStr32(gPlayerSav, ph);
        DrawTextC(val.empty() ? "(empty)" : val, cx, midY-40, COL_TEXT, Font::Lg);
        int bw=260, bh=40, bx=cx-bw/2, by=midY+10;
        HitAdd(bx,by,bw,bh, TPlayerKbd, 0);
        FillRect(bx,by,bw,bh,COL_SEL); DrawRect(bx,by,bw,bh,COL_ACCENT);
        DrawTextC("A  edit with keyboard", cx, by+bh/2, COL_ACCENT, Font::Md);
        const char* howName = (strcmp(fd.fieldName,"Player.Name")==0)       ? "Player.HowToCallName"
                            : (strcmp(fd.fieldName,"Player.IslandName")==0)  ? "Player.HowToCallIslandName"
                            : nullptr;
        if (howName) {
            std::string how = SaveEditor::GetWStr32(gPlayerSav, SaveEditor::Hash(howName));
            const std::string& dispHow = how.empty() ? val : how;
            DrawTextC(dispHow.empty()?"(empty)":dispHow.c_str(), cx, by+bh+16, COL_TEXT, Font::Sm);
            int pbX=cx-bw/2, pbY=by+bh+32;
            HitAdd(pbX,pbY,bw,bh,TSimBtn,(int)HidNpadButton_X);
            FillRect(pbX,pbY,bw,bh,COL_SEL); DrawRect(pbX,pbY,bw,bh,COL_ACCENT);
            DrawTextC("X  edit pronunciation",cx,pbY+bh/2,COL_ACCENT,Font::Md);
            // Language section
            bool isNameField = strcmp(fd.fieldName,"Player.Name")==0;
            const char* langFieldName = isNameField ? "Player.NameRegionLanguageID"
                                                    : "Player.IslandNameRegionLanguageID";
            uint32_t langEv = SaveEditor::GetEnum(gPlayerSav, SaveEditor::Hash(langFieldName));
            const char* langLabel = "Unknown";
            for (int li=0; li<LANG_OPTION_COUNT; li++)
                if (SaveEditor::Hash(LANG_OPTIONS[li].name)==langEv) { langLabel=LANG_OPTIONS[li].label; break; }
            int langY = pbY + bh + 28;
            DrawTextC(langLabel, cx, langY, COL_TEXT, Font::Md);
            const int lbW=130, lbH=36, lbGap=24;
            int lbxL=cx-lbGap/2-lbW, lbxR=cx+lbGap/2, lbY=langY+22;
            HitAdd(lbxL,lbY,lbW,lbH,TSimBtn,(int)HidNpadButton_Left);
            FillRect(lbxL,lbY,lbW,lbH,COL_PANEL); DrawRect(lbxL,lbY,lbW,lbH,COL_BORDER);
            DrawTextC("< Prev",lbxL+lbW/2,lbY+lbH/2,COL_DIM);
            HitAdd(lbxR,lbY,lbW,lbH,TSimBtn,(int)HidNpadButton_Right);
            FillRect(lbxR,lbY,lbW,lbH,COL_PANEL); DrawRect(lbxR,lbY,lbW,lbH,COL_BORDER);
            DrawTextC("Next >",lbxR+lbW/2,lbY+lbH/2,COL_DIM);
            DrawTextC("Left/Right  cycle language", cx, lbY+lbH+14, COL_DIM, Font::Sm);
            bool canUndoLang = isNameField ? gNameLangUndoValid : gIslandLangUndoValid;
            void (*undoLangFn)(int) = isNameField ? TUndoNameLang : TUndoIslandLang;
            int ubW=100, ubH=44, ubX=cx-ubW/2, ubY=lbY+lbH+36;
            HitAdd(ubX,ubY,ubW,ubH,undoLangFn,0);
            FillRect(ubX,ubY,ubW,ubH,COL_PANEL);
            DrawRect(ubX,ubY,ubW,ubH, canUndoLang?COL_GOLD:COL_BORDER);
            DrawTextC("Undo",ubX+ubW/2,ubY+ubH/2, canUndoLang?COL_GOLD:COL_DIM, Font::Md);
        }
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
            DrawRect(sbx, sby, sw, sh, ssel ? SDL_Color{255,255,255,255} : SDL_Color{80,80,80,255});
            if (ssel) DrawRect(sbx+2, sby+2, sw-4, sh-4, SDL_Color{255,255,255,255});
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

    {
        bool hasPron = fd.isStr && (strcmp(fd.fieldName,"Player.Name")==0 || strcmp(fd.fieldName,"Player.IslandName")==0);
        DrawFooter(hasPron
            ? "ZL/ZR  field    A  name    X  pronunciation    Left/Right  language    L/R  tab    B/+  back"
            : "ZL/ZR  field    Left/Right  value    A  edit    X  undo    L/R  tab    B/+  back");
    }
    SDL_RenderPresent(gRen);
}

// ─── Mii stats editor ─────────────────────────────────────────────────────────
static const int MSE_LIST_W = 240; // mii selector on left

// ── Belongings constants ───────────────────────────────────────────────────────
static const uint32_t BL_H_CLOTH_OWN  = 0x84824827u;
static const uint32_t BL_H_COORD_OWN  = 0x89ce9150u;
static const uint32_t BL_H_GOODS_ID   = 0xe3d6005eu;
static const uint32_t BL_H_GOODS_TIME = 0x48c3881au;
static const uint32_t BL_H_GOODS_UGC  = 0x2115afb6u;
static const int      BL_SLOTS_PER_MII    = 1200;
static const int      BL_COORD_SLOTS_MII  = 400;
static const int      BL_GOODS_SLOTS_MII  = 12;
static const uint32_t BL_H_INVALID        = 0x7e3d1e46u;

struct BlWornSlotDef { const char* name; uint32_t kh; uint32_t ch; const char* cat; bool addSocks; };
static const BlWornSlotDef BL_WORN_DEFS[] = {
    {"All",       0xb20fecedu, 0xee37f256u, "All",       false},
    {"Topslong",  0x35256740u, 0x1124095du, "Topslong",  false},
    {"Tops",      0xe81ee9c4u, 0x77cf0ff7u, "Tops",      false},
    {"BottomsA",  0x15aea81du, 0xec5010fbu, "Bottoms",   false},
    {"BottomsB",  0x7a1809d1u, 0x536dbec2u, "Bottoms",   true},
    {"Headwear",  0x98cc9cccu, 0xc499cda5u, "Headwear",  false},
    {"Shoes",     0x0a903d03u, 0xc0529e53u, "Shoes",     false},
    {"Accessory", 0xc1094d9fu, 0x64af701au, "Accessory", false},
};
static const uint32_t BL_COORD_KH = 0xd5ee521au;
static const uint32_t BL_COORD_CH = 0x763c97ccu;

// Selectable rows in Belongings:
//   0-7  : cloth worn slots (BL_WORN_DEFS[0-7])
//   8    : coordinate worn slot
//   9-20 : goods pocket slots 0-11
//   21   : Unlock All Clothes
//   22   : Lock All Clothes
//   23   : Unlock All Coords
//   24   : Lock All Coords
static const int BL_SEL_MAX = 25;
// Visual rows (includes section headers):
//   vr 0        : "── WORN OUTFIT ──" header
//   vr 1-9      : worn slots 0-8
//   vr 10       : "── GOODS POCKET ──" header
//   vr 11-22    : goods slots 0-11
//   vr 23       : "── OWNERSHIP ──" header
//   vr 24-27    : action rows 0-3
static const int BL_VIS_ROWS  = 28;
static const int BL_ROW_H     = 32;

static int BlSelToVis(int sel) {
    if (sel < 9)  return sel + 1;
    if (sel < 21) return sel + 2;
    return sel + 3;
}

static const char* BlClothLabel(uint32_t h) {
    if (!h || h == BL_H_INVALID) return "(none)";
    for (int i = 0; i < BL_CLOTH_COUNT; i++)
        if (BL_CLOTH_ITEMS[i].nameHash == h) return BL_CLOTH_ITEMS[i].label;
    return "?";
}
static const char* BlCoordLabel(uint32_t h) {
    if (!h || h == BL_H_INVALID) return "(none)";
    for (int i = 0; i < BL_COORD_COUNT; i++)
        if (BL_COORD_ITEMS[i].keyHash == h) return BL_COORD_ITEMS[i].label;
    return "?";
}
static const char* BlGoodsLabel(uint32_t h) {
    if (!h) return "(empty)";
    for (int i = 0; i < BL_GOODS_COUNT; i++)
        if (BL_GOODS_ITEMS[i].nameHash == h) return BL_GOODS_ITEMS[i].label;
    return "?";
}

static void BlOpenPicker(int blSel, int miiIdx) {
    gBlPickerHashes.clear();
    gBlPickerLabels.clear();
    uint32_t curHash = 0;
    if (blSel < 8) {
        const auto& ws = BL_WORN_DEFS[blSel];
        curHash = SaveEditor::GetAnyEnumAt(gMiiSav, ws.kh, miiIdx, BL_H_INVALID);
        gBlPickerHashes.push_back(BL_H_INVALID); gBlPickerLabels.push_back("(none)");
        for (int k = 0; k < BL_CLOTH_COUNT; k++) {
            const char* cat = BL_CLOTH_ITEMS[k].category;
            if (strcmp(cat, ws.cat)==0 || (ws.addSocks && strcmp(cat,"Socks")==0)) {
                gBlPickerHashes.push_back(BL_CLOTH_ITEMS[k].nameHash);
                gBlPickerLabels.push_back(BL_CLOTH_ITEMS[k].label);
            }
        }
    } else if (blSel == 8) {
        curHash = SaveEditor::GetAnyEnumAt(gMiiSav, BL_COORD_KH, miiIdx, BL_H_INVALID);
        gBlPickerHashes.push_back(BL_H_INVALID); gBlPickerLabels.push_back("(none)");
        for (int k = 0; k < BL_COORD_COUNT; k++) {
            gBlPickerHashes.push_back(BL_COORD_ITEMS[k].keyHash);
            gBlPickerLabels.push_back(BL_COORD_ITEMS[k].label);
        }
    } else if (blSel >= 9 && blSel < 21) {
        int ai = miiIdx * BL_GOODS_SLOTS_MII + (blSel - 9);
        curHash = SaveEditor::GetUIntAt(gMiiSav, BL_H_GOODS_ID, ai);
        gBlPickerHashes.push_back(0u); gBlPickerLabels.push_back("(empty)");
        for (int k = 0; k < BL_GOODS_COUNT; k++) {
            gBlPickerHashes.push_back(BL_GOODS_ITEMS[k].nameHash);
            gBlPickerLabels.push_back(BL_GOODS_ITEMS[k].label);
        }
    } else {
        return;
    }
    gBlFilter.clear();
    BlRebuildFilter();
    gBlPickerSel = 0;
    for (int i = 0; i < (int)gBlFiltered.size(); i++)
        if (gBlPickerHashes[gBlFiltered[i]] == curHash) { gBlPickerSel = i; break; }
    gBlPickerScroll = gBlPickerSel;
    gBlPickerOpen = true;
}

static void DrawMiiStats() {
    HitClear();
    // Auto-fade transient messages (e.g. habit toggle confirmations)
    if (gMiiStatsMsgFrames > 0) {
        if (--gMiiStatsMsgFrames == 0) { gMiiStatsMsg.clear(); gMiiStatsMsgCol = COL_TEXT; }
    }
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);
    if (gShowFileBrowser) { DrawFileBrowser(); SDL_RenderPresent(gRen); return; }
    DrawHeader("mii stats");

    // Tab bar
    {
        int tw=140, th=22, gap=4;
        int totalW=tw*4+gap*3, tx=SCREEN_W/2-totalW/2, ty=28;
        struct { const char* label; OnSwitchMode mode; } tabs[]={
            {"webui",OnSwitchMode::WebUI},{"textures",OnSwitchMode::UGC},
            {"mii",OnSwitchMode::MiiStats},{"player",OnSwitchMode::Player}};
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

    // Middle: field list for selected mii (hidden in Social/Belongings — panel expands)
    int midX = SE_LIST_X + MSE_LIST_W + 12;
    int midW = 340;
    bool midHidden = (gMiiStatsSubTab == MiiStatsSubTab::Social || gMiiStatsSubTab == MiiStatsSubTab::Belongings || gMiiStatsSubTab == MiiStatsSubTab::Habits);
    if (!midHidden) {
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
                std::string disp = filled ? (wt.empty()?"—":FitNodeName(wt,fsm,availW)) : "—";
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
            int visRow = 0, filtRow = 0;
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
                if (filtRow >= gMiiRelScroll) {
                    int ry = SE_TOP_Y + 2 + visRow * 34;
                    if (ry + 34 <= SCREEN_H - 32) {
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
                        visRow++;
                    }
                }
                filtRow++;
            }
            gMiiRelCount = filtRow;
            if (filtRow > 0 && gMiiRelSel >= filtRow) gMiiRelSel = filtRow-1;
            if (filtRow == 0)
                DrawTextC("no relations", midX+midW/2, SE_TOP_Y+80, COL_DIM);
        } else if (!midHidden) {
            // Stats fields list (only when the middle panel is visible)
            for (int i = 0; i < MII_STATS_FIELD_COUNT; i++) {
                bool sel = (i == gMiiStatsFieldSel);
                int ry = SE_TOP_Y + 2 + i * 34;
                if (ry + 34 > SCREEN_H - 32) break;
                HitAdd(midX+1, ry, midW-2, 33, TSelMiiStatsField, i);
                FillRect(midX+1, ry, midW-2, 33, sel?COL_SEL:COL_BG);
                const auto& fd = MII_STATS_FIELDS[i];
                SDL_Color borderCol = fd.isAction ? COL_GOLD : COL_GOLD;
                if (sel) DrawRect(midX+1, ry, midW-2, 33, borderCol);
                TTF_Font* fsm=GetFont(Font::Sm); int lw=0,lh=0,vw=0,vh=0;
                if(fsm) TTF_SizeUTF8(fsm,fd.label,&lw,&lh);
                if (fd.isAction) {
                    SDL_Color lCol = sel ? COL_GOLD : SDL_Color{160,120,60,255};
                    DrawText(fd.label, midX+8, ry+(33-lh)/2, lCol);
                    const char* arrow = "▶";
                    int aw=0,ah=0;
                    if(fsm) TTF_SizeUTF8(fsm,arrow,&aw,&ah);
                    DrawText(arrow, midX+midW-aw-8, ry+(33-ah)/2, sel?COL_GOLD:SDL_Color{100,80,40,255});
                } else {
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
                    if(fsm) TTF_SizeUTF8(fsm,val.c_str(),&vw,&vh);
                    DrawText(fd.label, midX+8, ry+(33-lh)/2, sel?COL_TEXT:COL_DIM);
                    DrawText(val, midX+midW-vw-8, ry+(33-vh)/2, sel?COL_ACCENT:COL_DIM);
                }
            }
        }
    }

    // Right: edit controls / social / belongings view
    // Social and Belongings expand left to cover the hidden middle panel
    int editX = midX + midW + 8;
    int editW = SCREEN_W - editX - 8;
    int panelX = midHidden ? midX   : editX;
    int panelW = midHidden ? (SCREEN_W - midX - 8) : editW;
    FillRect(panelX, SE_TOP_Y, panelW, SCREEN_H-SE_TOP_Y-32, {6,6,6,255});
    DrawRect(panelX, SE_TOP_Y, panelW, SCREEN_H-SE_TOP_Y-32, COL_BORDER);

    // Sub-tab bar: [Stats] [Belongings] [Habits] [Words] [Relations] [Social]
    {
        int stW=84, stH=22, stGap=4, stX=editX+8, stY=SE_TOP_Y+6;
        const char* stLabels[] = {"Stats","Belongings","Habits","Words","Relations","Social"};
        for (int ti=0; ti<MII_SUBTAB_COUNT; ti++) {
            bool sSel = ((int)gMiiStatsSubTab == ti);
            int sx = stX + ti*(stW+stGap);
            // Hit area is taller than the visual so it's easier to tap on touchscreen
            HitAdd(sx, stY-4, stW, stH+8, TSetSocialView, ti);
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
                // ── Global graph — multi-ring, handles up to 70 miis ─────────────
                int gcx = panelX + panelW/2;
                int gcy = panelTop + panelH/2;
                int N   = (int)gMiis.size();
                if (N == 0) {
                    DrawTextC("no miis", gcx, gcy, COL_DIM, Font::Md);
                } else {
                    // Adaptive node size: shrinks as count grows
                    int nW = N<=8?80 : N<=14?70 : N<=22?60 : N<=34?50 : N<=50?44 : 40;
                    int nH = N<=14?28 : N<=28?24 : N<=50?20 : 18;

                    // Build concentric rings (inner→outer) until all nodes placed
                    int maxR = std::min(panelW/2 - nW/2 - 4, panelH/2 - nH/2 - 4);
                    int Rcur = std::max(nH * 4, 70);

                    struct GRing { int R, first, count; };
                    std::vector<GRing> grings;
                    int placed = 0;
                    while (placed < N && Rcur <= maxR) {
                        float s = (float)(nW + 6) / (2.f * (float)Rcur);
                        int cap = (s >= 1.f) ? 1 : (int)((float)M_PI / asinf(s));
                        cap = std::max(1, std::min(cap, N - placed));
                        grings.push_back({Rcur, placed, cap});
                        placed += cap;
                        Rcur += nH + 8;
                    }

                    // Pre-compute node positions indexed by mii list order
                    std::vector<int> nx_(N, -9999), ny_(N, -9999);
                    for (auto& gr : grings) {
                        for (int i = 0; i < gr.count; i++) {
                            int j = gr.first + i;
                            float a = -(float)M_PI/2.f + (2.f*(float)M_PI*i)/gr.count;
                            nx_[j] = gcx + (int)((float)gr.R * cosf(a));
                            ny_[j] = gcy + (int)((float)gr.R * sinf(a));
                        }
                    }

                    // Edges first — colored, semi-transparent
                    for (int i = 0; i < pairCount; i++) {
                        int sa = SaveEditor::GetIntAt(gMiiSav, H_ID_A, i);
                        int sb = SaveEditor::GetIntAt(gMiiSav, H_ID_B, i);
                        if (sa < 0 || sb < 0) continue;
                        int ai = -1, bi = -1;
                        for (int j = 0; j < N; j++) {
                            if (gMiis[j].slot-1 == sa) ai = j;
                            if (gMiis[j].slot-1 == sb) bi = j;
                        }
                        if (ai < 0 || bi < 0 || nx_[ai] < -9990 || nx_[bi] < -9990) continue;
                        SDL_Color lc = RelTypeColor(SaveEditor::GetEnumAt(gMiiSav, H_BASE, i*2));
                        SDL_SetRenderDrawColor(gRen, lc.r, lc.g, lc.b, 100);
                        SDL_RenderDrawLine(gRen, nx_[ai], ny_[ai], nx_[bi], ny_[bi]);
                    }

                    // Nodes on top of edges
                    TTF_Font* fsm = GetFont(Font::Sm);
                    for (int i = 0; i < N; i++) {
                        if (nx_[i] < -9990) continue;
                        bool isSel = (i == gMiiStatsMiiSel);
                        std::string raw = SaveEditor::GetWStr32At(gMiiSav, H_NAME, gMiis[i].slot-1);
                        std::string name = FitNodeName(raw, fsm, nW - 8);
                        SDL_Color fill   = isSel ? COL_ACCENT : COL_PANEL2;
                        SDL_Color border = isSel ? COL_GOLD   : COL_BORDER;
                        SDL_Color text   = isSel ? SDL_Color{10,10,10,255} : COL_TEXT;
                        FillRect(nx_[i]-nW/2+2, ny_[i]-nH/2+2, nW, nH, {0,0,0,110});
                        HitAdd(nx_[i]-nW/2, ny_[i]-nH/2, nW, nH, TSelMiiStatsMii, i);
                        FillRect(nx_[i]-nW/2, ny_[i]-nH/2, nW, nH, fill);
                        if (isSel) FillRect(nx_[i]-nW/2, ny_[i]-nH/2, nW, 2, COL_GOLD);
                        DrawRect(nx_[i]-nW/2, ny_[i]-nH/2, nW, nH, border);
                        DrawTextC(name, nx_[i], ny_[i], text, Font::Sm);
                    }

                    // Dim center label showing total count
                    DrawTextC(std::to_string(N) + " miis", gcx, gcy, COL_DIM, Font::Sm);
                }
            } else {
                // ── Focus graph: multi-ring radial, handles up to 70 connections ─
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
                // Group same relationship types together for visual clarity
                std::sort(rows.begin(), rows.end(), [](const SRow& a, const SRow& b){ return a.outT < b.outT; });

                int gcx = panelX + panelW/2;
                int gcy = panelTop + panelH/2;

                if (rows.empty()) {
                    DrawTextC("no relationships", gcx, gcy, COL_DIM, Font::Md);
                } else {
                    int n   = (int)rows.size();
                    int cnW = 120, cnH = 44;

                    // Adaptive node size: shrinks gracefully as connections grow
                    int nW = n<=6?94 : n<=12?80 : n<=22?68 : n<=38?56 : 46;
                    int nH = n<=12?44 : n<=26?36 : 28;

                    // Build concentric rings (inner→outer), each ring non-overlapping
                    // Inner ring clears the center node with a margin
                    int maxR = std::min(panelW/2 - nW/2 - 6, panelH/2 - nH/2 - 6);
                    int Rcur = std::max(cnW/2 + nW/2 + 16, 80);

                    struct SRing { int R, first, count; };
                    std::vector<SRing> rings;
                    int placed = 0;
                    while (placed < n && Rcur <= maxR) {
                        float s = (float)(nW + 8) / (2.f * (float)Rcur);
                        int cap = (s >= 1.f) ? 1 : (int)((float)M_PI / asinf(s));
                        cap = std::max(1, std::min(cap, n - placed));
                        rings.push_back({Rcur, placed, cap});
                        placed += cap;
                        Rcur += nH + 10;
                    }
                    int hidden = n - placed;

                    TTF_Font* fsm = GetFont(Font::Sm);
                    TTF_Font* fmd = GetFont(Font::Md);
                    std::string cName = SaveEditor::GetWStr32At(gMiiSav, H_NAME, miiSlotIdx);
                    std::string cFit  = FitNodeName(cName.empty()?"?":cName, fmd, cnW - 14);

                    // 1. Edges (drawn under everything)
                    for (auto& ring : rings) {
                        for (int i = 0; i < ring.count; i++) {
                            const SRow& row = rows[ring.first + i];
                            SDL_Color lc = RelTypeColor(row.outT);
                            float angle = -(float)M_PI/2.f + (2.f*(float)M_PI*i)/ring.count;
                            int nx = gcx + (int)((float)ring.R * cosf(angle));
                            int ny = gcy + (int)((float)ring.R * sinf(angle));
                            SDL_SetRenderDrawColor(gRen, lc.r, lc.g, lc.b, 150);
                            SDL_RenderDrawLine(gRen, gcx, gcy, nx, ny);
                        }
                    }

                    // 2. Center node
                    FillRect(gcx-cnW/2+2, gcy-cnH/2+2, cnW, cnH, {0,0,0,110});
                    FillRect(gcx-cnW/2, gcy-cnH/2, cnW, cnH, COL_ACCENT);
                    DrawRect(gcx-cnW/2, gcy-cnH/2, cnW, cnH, COL_TEXT);
                    DrawTextC(cFit, gcx, gcy, {10,10,10,255}, Font::Md);

                    // 3. Satellite nodes (name + rel type, two lines when tall enough)
                    for (auto& ring : rings) {
                        for (int i = 0; i < ring.count; i++) {
                            const SRow& row = rows[ring.first + i];
                            SDL_Color lc = RelTypeColor(row.outT);
                            const char* relLbl = RelTypeName(row.outT);
                            float angle = -(float)M_PI/2.f + (2.f*(float)M_PI*i)/ring.count;
                            int nx = gcx + (int)((float)ring.R * cosf(angle));
                            int ny = gcy + (int)((float)ring.R * sinf(angle));
                            std::string nn   = SaveEditor::GetWStr32At(gMiiSav, H_NAME, row.other);
                            std::string nFit = FitNodeName(nn.empty()?"?":nn, fsm, nW - (nH>=36?16:8));
                            FillRect(nx-nW/2+2, ny-nH/2+2, nW, nH, {0,0,0,110});
                            FillRect(nx-nW/2,   ny-nH/2,   nW, nH, COL_PANEL2);
                            FillRect(nx-nW/2,   ny-nH/2,   3,  nH, lc);
                            DrawRect(nx-nW/2,   ny-nH/2,   nW, nH, lc);
                            if (nH >= 36) {
                                DrawTextC(nFit,                 nx+2, ny - nH/4, COL_TEXT, Font::Sm);
                                DrawTextC(relLbl ? relLbl : "?", nx+2, ny + nH/4, lc, Font::Sm);
                            } else {
                                DrawTextC(nFit, nx+2, ny, COL_TEXT, Font::Sm);
                            }
                        }
                    }

                    if (hidden > 0) {
                        std::string hstr = "+" + std::to_string(hidden) + " more";
                        DrawTextC(hstr, panelX + panelW - 60, panelTop + panelH - 16, COL_DIM, Font::Sm);
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
                      cx2, panelTop+16, COL_DIM, Font::Sm);
            // Kind section
            SDL_SetRenderDrawColor(gRen,COL_BORDER.r,COL_BORDER.g,COL_BORDER.b,80);
            SDL_RenderDrawLine(gRen, editX+12, panelTop+32, editX+editW-12, panelTop+32);
            DrawTextC("Kind", cx2, panelTop+50, COL_DIM, Font::Sm);
            DrawTextC(isFilled?(kindLbl?kindLbl:"?"):"—",
                      cx2, panelTop+84, isFilled?COL_TEXT:COL_DIM, Font::Lg);
            {
                int bW=108,bH=38,bGap=14;
                int bxL=cx2-bGap/2-bW, bxR=cx2+bGap/2, bY=panelTop+122;
                HitAdd(bxL,bY,bW,bH,TSimBtn,(int)HidNpadButton_Left);
                FillRect(bxL,bY,bW,bH,COL_PANEL); DrawRect(bxL,bY,bW,bH,COL_BORDER);
                DrawTextC("<",bxL+bW/2,bY+bH/2,COL_DIM,Font::Md);
                HitAdd(bxR,bY,bW,bH,TSimBtn,(int)HidNpadButton_Right);
                FillRect(bxR,bY,bW,bH,COL_PANEL); DrawRect(bxR,bY,bW,bH,COL_BORDER);
                DrawTextC(">",bxR+bW/2,bY+bH/2,COL_DIM,Font::Md);
            }
            // Text section
            SDL_SetRenderDrawColor(gRen,COL_BORDER.r,COL_BORDER.g,COL_BORDER.b,80);
            SDL_RenderDrawLine(gRen, editX+12, panelTop+182, editX+editW-12, panelTop+182);
            DrawTextC("Word Text", cx2, panelTop+200, COL_DIM, Font::Sm);
            std::string dispTxt = curText.empty() ? "(none)" : curText;
            DrawTextC(dispTxt, cx2, panelTop+232, (isFilled&&!curText.empty())?COL_TEXT:COL_DIM, Font::Md);
            {
                int bW=240,bH=42,bX=cx2-bW/2,bY=panelTop+262;
                HitAdd(bX,bY,bW,bH,TSimBtn,(int)HidNpadButton_A);
                FillRect(bX,bY,bW,bH,COL_SEL); DrawRect(bX,bY,bW,bH,COL_ACCENT);
                DrawTextC("A  Edit Text",cx2,bY+bH/2,COL_ACCENT,Font::Sm);
            }
            // Pronunciation section
            SDL_SetRenderDrawColor(gRen,COL_BORDER.r,COL_BORDER.g,COL_BORDER.b,80);
            SDL_RenderDrawLine(gRen, editX+12, panelTop+334, editX+editW-12, panelTop+334);
            DrawTextC("Pronunciation  (optional)", cx2, panelTop+352, COL_DIM, Font::Sm);
            std::string dispHow = curHow.empty() ? "(same as text)" : curHow;
            DrawTextC(dispHow, cx2, panelTop+384, !curHow.empty()?COL_TEXT:COL_DIM, Font::Sm);
            {
                int bW=240,bH=42,bX=cx2-bW/2,bY=panelTop+410;
                HitAdd(bX,bY,bW,bH,TSimBtn,(int)HidNpadButton_X);
                FillRect(bX,bY,bW,bH,COL_SEL); DrawRect(bX,bY,bW,bH,COL_ACCENT);
                DrawTextC("X  Edit Pronunciation",cx2,bY+bH/2,COL_ACCENT,Font::Sm);
            }
            // Undo button
            {
                bool canUndo = gWordUndo.valid;
                int ubW=120,ubH=44,ubX=cx2-ubW/2,ubY=panelTop+490;
                HitAdd(ubX,ubY,ubW,ubH,TUndoWords,0);
                FillRect(ubX,ubY,ubW,ubH,COL_PANEL);
                DrawRect(ubX,ubY,ubW,ubH,canUndo?COL_GOLD:COL_BORDER);
                DrawTextC("-  Undo",cx2,ubY+ubH/2,canUndo?COL_GOLD:COL_DIM,Font::Md);
            }
            if (!gMiiStatsMsg.empty())
                DrawTextC(gMiiStatsMsg, cx2, panelTop+554, gMiiStatsMsgCol);
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
                DrawTextC(oName.empty()?"(unknown)":oName, cx2, panelTop+18, COL_TEXT, Font::Md);
                SDL_SetRenderDrawColor(gRen,COL_BORDER.r,COL_BORDER.g,COL_BORDER.b,80);
                SDL_RenderDrawLine(gRen, editX+12, panelTop+42, editX+editW-12, panelTop+42);
                // Your relationship type
                DrawTextC("Your type", cx2, panelTop+62, COL_DIM, Font::Sm);
                SDL_Color myTC = RelTypeColor(myType);
                const char* myTN = RelTypeName(myType);
                DrawTextC(myTN?myTN:"?", cx2, panelTop+102, myTC, Font::Lg);
                {
                    int bW=130,bH=40,bGap=14;
                    int bxL=cx2-bGap/2-bW, bxR=cx2+bGap/2, bY=panelTop+144;
                    HitAdd(bxL,bY,bW,bH,TSimBtn,(int)HidNpadButton_Left);
                    FillRect(bxL,bY,bW,bH,COL_PANEL); DrawRect(bxL,bY,bW,bH,COL_BORDER);
                    DrawTextC("< Type",bxL+bW/2,bY+bH/2,COL_DIM);
                    HitAdd(bxR,bY,bW,bH,TSimBtn,(int)HidNpadButton_Right);
                    FillRect(bxR,bY,bW,bH,COL_PANEL); DrawRect(bxR,bY,bW,bH,COL_BORDER);
                    DrawTextC("Type >",bxR+bW/2,bY+bH/2,COL_DIM);
                }
                // Their type (auto-counterpart, read-only display)
                SDL_SetRenderDrawColor(gRen,COL_BORDER.r,COL_BORDER.g,COL_BORDER.b,80);
                SDL_RenderDrawLine(gRen, editX+12, panelTop+222, editX+editW-12, panelTop+222);
                DrawTextC("Their type  (auto)", cx2, panelTop+244, COL_DIM, Font::Sm);
                SDL_Color otTC = RelTypeColor(otType);
                const char* otTN = RelTypeName(otType);
                DrawTextC(otTN?otTN:"?", cx2, panelTop+282, otTC, Font::Md);
                // Meter
                SDL_SetRenderDrawColor(gRen,COL_BORDER.r,COL_BORDER.g,COL_BORDER.b,80);
                SDL_RenderDrawLine(gRen, editX+12, panelTop+336, editX+editW-12, panelTop+336);
                DrawTextC("Meter", cx2, panelTop+358, COL_DIM, Font::Sm);
                std::string mStr = fixed ? "100 (fixed)" : std::to_string(myMeter);
                DrawTextC(mStr, cx2, panelTop+398, fixed?COL_DIM:COL_TEXT, Font::Lg);
                if (!fixed) {
                    static const int MSTEPS[] = {10, 1};
                    int bW=76,bH=44,bGap=6;
                    int totalBW = (2*(bW+bGap))*2 + 20;
                    int bxBase = cx2 - totalBW/2, bY = panelTop+442;
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
                    DrawTextC("both directions set together", cx2, panelTop+514, COL_DIM, Font::Sm);
                }
                // ── Since (TypeSetTime) ───────────────────────────────────
                {
                    static const uint32_t H_TST_RP = 0x1a892e50u;
                    int sinceY = fixed ? panelTop+428 : panelTop+530;
                    SDL_SetRenderDrawColor(gRen,COL_BORDER.r,COL_BORDER.g,COL_BORDER.b,80);
                    SDL_RenderDrawLine(gRen, editX+12, sinceY, editX+editW-12, sinceY);
                    uint64_t tst = SaveEditor::GetUInt64At(gMiiSav, H_TST_RP, gMiiRelPairIdx);
                    char dateBuf[11] = "";
                    if (tst > 0) {
                        time_t t = (time_t)tst;
                        struct tm* ti = localtime(&t);
                        snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d",
                                 ti->tm_year+1900, ti->tm_mon+1, ti->tm_mday);
                    }
                    DrawTextC("Since", cx2, sinceY+14, COL_DIM, Font::Sm);
                    int rowH = 30, rowW = 150;
                    int rowY = sinceY+28, rowX = cx2 - rowW/2;
                    HitAdd(rowX, rowY, rowW, rowH, TRelDateKbd, gMiiRelPairIdx);
                    FillRect(rowX, rowY, rowW, rowH, COL_PANEL);
                    DrawRect(rowX, rowY, rowW, rowH, COL_BORDER);
                    DrawTextC(dateBuf[0] ? dateBuf : "(not set)", rowX+rowW/2, rowY+rowH/2,
                              dateBuf[0] ? COL_TEXT : COL_DIM, Font::Sm);
                }
            }
        } else if (gMiiStatsSubTab == MiiStatsSubTab::Belongings) {
            // ── Belongings panel (expanded, uses panelX/panelW) ─────────────────
            TTF_Font* fsm = GetFont(Font::Sm);

            if (gBlPickerOpen) {
                // ── Item picker overlay ──────────────────────────────────────────
                const int PK_ROW_H = 30;
                const int PK_HDR_H = 32;
                const int PK_FOOT_H = 22;
                int listTop = panelTop + PK_HDR_H;
                int listH   = panelH - PK_HDR_H - PK_FOOT_H;
                int pkVis   = listH / PK_ROW_H;
                int total   = (int)gBlFiltered.size();
                int totalAll = (int)gBlPickerHashes.size();

                if (gBlPickerSel < 0) gBlPickerSel = 0;
                if (total > 0 && gBlPickerSel >= total) gBlPickerSel = total - 1;
                if (gBlPickerScroll < 0) gBlPickerScroll = 0;
                if (total > 0 && gBlPickerScroll > total - pkVis) gBlPickerScroll = std::max(0, total - pkVis);

                // Header
                const char* slotName = (gBlSel < 8) ? BL_WORN_DEFS[gBlSel].name :
                                       (gBlSel == 8) ? "Coord" : "Pocket";
                FillRect(panelX+2, panelTop, panelW-4, PK_HDR_H, {28,28,28,255});
                DrawRect(panelX+2, panelTop, panelW-4, PK_HDR_H, COL_BORDER);
                std::string hdrStr = std::string("  ") + slotName;
                if (!gBlFilter.empty()) hdrStr += "  —  filter: \"" + gBlFilter + "\"";
                else                    hdrStr += "  —  select item";
                int hw=0,hh=0; if(fsm) TTF_SizeUTF8(fsm, hdrStr.c_str(), &hw, &hh);
                DrawText(hdrStr, panelX+10, panelTop+(PK_HDR_H-hh)/2,
                         gBlFilter.empty() ? COL_GOLD : COL_ACCENT);
                std::string cntStr = gBlFilter.empty()
                    ? (std::to_string(total) + " items")
                    : (std::to_string(total) + " of " + std::to_string(totalAll) + " items");
                int cw=0,cntH=0; if(fsm) TTF_SizeUTF8(fsm, cntStr.c_str(), &cw, &cntH);
                DrawText(cntStr, panelX+panelW-cw-14, panelTop+(PK_HDR_H-cntH)/2, COL_DIM);

                // Item list (uses filtered indices)
                for (int i = 0; i < pkVis; i++) {
                    int filtIdx = gBlPickerScroll + i;
                    if (filtIdx >= total) break;
                    int idx = gBlFiltered[filtIdx];
                    bool sel = (filtIdx == gBlPickerSel);
                    int ry = listTop + i * PK_ROW_H;
                    FillRect(panelX+2, ry, panelW-4, PK_ROW_H-1, sel?COL_SEL:COL_BG);
                    if (sel) DrawRect(panelX+2, ry, panelW-4, PK_ROW_H-1, COL_GOLD);
                    // Big "select" button on the left — same size as in the main belongings list
                    const int SEL_W = 18;
                    int selBoxX = panelX + 8;
                    int selBoxY = ry + (PK_ROW_H - SEL_W)/2;
                    FillRect(selBoxX, selBoxY, SEL_W, SEL_W, sel?COL_GOLD:COL_PANEL);
                    DrawRect(selBoxX, selBoxY, SEL_W, SEL_W, sel?COL_GOLD:COL_BORDER);
                    if (sel) FillRect(selBoxX+5, selBoxY+5, SEL_W-10, SEL_W-10, {10,10,10,255});
                    HitAdd(panelX+2, ry, panelW-4, PK_ROW_H-1, TBLPickSel, filtIdx);
                    int lw=0,lh=0; if(fsm) TTF_SizeUTF8(fsm, gBlPickerLabels[idx], &lw, &lh);
                    DrawText(gBlPickerLabels[idx], panelX+44, ry+(PK_ROW_H-lh)/2,
                             sel ? COL_GOLD : COL_TEXT);
                }

                // No-results hint
                if (total == 0) {
                    DrawTextC("no matches — press X to clear filter",
                              panelX+panelW/2, listTop+listH/2, COL_DIM, Font::Md);
                }

                // Scrollbar
                if (total > pkVis) {
                    int sbH = listH * pkVis / total;
                    int sbY = listTop + listH * gBlPickerScroll / total;
                    FillRect(panelX+panelW-5, sbY, 3, sbH, COL_BORDER);
                }

                // Footer hint
                int footY = panelTop + panelH - PK_FOOT_H;
                FillRect(panelX+2, footY, panelW-4, PK_FOOT_H, {18,18,18,255});
                const char* foot = gBlFilter.empty()
                    ? "A  confirm    -  search    B  cancel"
                    : "A  confirm    -  edit filter    X  clear filter    B  cancel";
                DrawTextC(foot, panelX+panelW/2, footY+PK_FOOT_H/2, COL_DIM, Font::Sm);
            } else {
                // ── Normal Belongings list ───────────────────────────────────────
                const int BL_VIS = panelH / BL_ROW_H;
                if (gBlScroll < 0) gBlScroll = 0;
                if (gBlScroll > BL_VIS_ROWS - BL_VIS) gBlScroll = BL_VIS_ROWS - BL_VIS;

                const char* ACT_LABELS[] = {
                    "Unlock All Clothes (this Mii)",
                    "Lock All Clothes (this Mii)",
                    "Unlock All Coord Outfits (this Mii)",
                    "Lock All Coord Outfits (this Mii)",
                };

                for (int vr = gBlScroll; vr < BL_VIS_ROWS; vr++) {
                    int ry = panelTop + (vr - gBlScroll) * BL_ROW_H;
                    if (ry + BL_ROW_H > panelTop + panelH) break;

                    bool isHdr = (vr == 0 || vr == 10 || vr == 23);
                    int  itemSel = isHdr ? -1 :
                                   (vr <= 9)  ? (vr - 1) :
                                   (vr <= 22) ? (vr - 2) : (vr - 3);
                    bool sel = (!isHdr && itemSel == gBlSel);

                    if (isHdr) {
                        const char* hdr = (vr == 0) ? "  WORN OUTFIT" :
                                          (vr == 10) ? "  GOODS POCKET" : "  OWNERSHIP";
                        FillRect(panelX+2, ry, panelW-4, BL_ROW_H-1, {28,28,28,255});
                        DrawRect(panelX+2, ry, panelW-4, BL_ROW_H-1, COL_BORDER);
                        int tw=0,th=0; if(fsm) TTF_SizeUTF8(fsm,hdr,&tw,&th);
                        DrawText(hdr, panelX+10, ry+(BL_ROW_H-th)/2, COL_GOLD);
                    } else {
                        FillRect(panelX+2, ry, panelW-4, BL_ROW_H-1, sel?COL_SEL:COL_BG);
                        if (sel) DrawRect(panelX+2, ry, panelW-4, BL_ROW_H-1, COL_GOLD);
                        HitAdd(panelX+2, ry, panelW-4, BL_ROW_H-1, TBLSel, itemSel);

                        if (itemSel < 8) {
                            // Cloth worn slot — label gray, item white, color accent
                            const auto& ws = BL_WORN_DEFS[itemSel];
                            uint32_t ih = SaveEditor::GetAnyEnumAt(gMiiSav, ws.kh, miiSlotIdx, BL_H_INVALID);
                            int ci = SaveEditor::GetIntAt(gMiiSav, ws.ch, miiSlotIdx);
                            int cc = 1;
                            for (int k = 0; k < BL_CLOTH_COUNT; k++)
                                if (BL_CLOTH_ITEMS[k].nameHash == ih) { cc = BL_CLOTH_ITEMS[k].colorCount; break; }
                            int pw=0,ph=0; if(fsm) TTF_SizeUTF8(fsm, ws.name, &pw, &ph);
                            DrawText(ws.name, panelX+12, ry+(BL_ROW_H-ph)/2, COL_DIM);
                            const char* lbl = BlClothLabel(ih);
                            int lw=0,lh=0; if(fsm) TTF_SizeUTF8(fsm,lbl,&lw,&lh);
                            DrawText(lbl, panelX+12+pw, ry+(BL_ROW_H-lh)/2, sel?COL_GOLD:COL_TEXT);
                            std::string cv = "color " + std::to_string(ci+1) + "/" + std::to_string(cc);
                            int vw=0,vh=0; if(fsm) TTF_SizeUTF8(fsm,cv.c_str(),&vw,&vh);
                            DrawText(cv, panelX+panelW-vw-14, ry+(BL_ROW_H-vh)/2, sel?COL_ACCENT:COL_TEXT);
                        } else if (itemSel == 8) {
                            // Coordinate worn slot — label gray, item white, color accent
                            uint32_t ih = SaveEditor::GetAnyEnumAt(gMiiSav, BL_COORD_KH, miiSlotIdx, BL_H_INVALID);
                            int ci = SaveEditor::GetIntAt(gMiiSav, BL_COORD_CH, miiSlotIdx);
                            int cc = 1;
                            for (int k = 0; k < BL_COORD_COUNT; k++)
                                if (BL_COORD_ITEMS[k].keyHash == ih) { cc = BL_COORD_ITEMS[k].colorCount; break; }
                            const char* pfx = "Coord";
                            int pw=0,ph=0; if(fsm) TTF_SizeUTF8(fsm,pfx,&pw,&ph);
                            DrawText(pfx, panelX+12, ry+(BL_ROW_H-ph)/2, COL_DIM);
                            const char* lbl = BlCoordLabel(ih);
                            int lw=0,lh=0; if(fsm) TTF_SizeUTF8(fsm,lbl,&lw,&lh);
                            DrawText(lbl, panelX+12+pw, ry+(BL_ROW_H-lh)/2, sel?COL_GOLD:COL_TEXT);
                            std::string cv = "color " + std::to_string(ci+1) + "/" + std::to_string(cc);
                            int vw=0,vh=0; if(fsm) TTF_SizeUTF8(fsm,cv.c_str(),&vw,&vh);
                            DrawText(cv, panelX+panelW-vw-14, ry+(BL_ROW_H-vh)/2, sel?COL_ACCENT:COL_TEXT);
                        } else if (itemSel < 21) {
                            // Goods pocket slot — slot number gray, item name white
                            int slot = itemSel - 9;
                            int ai = miiSlotIdx * BL_GOODS_SLOTS_MII + slot;
                            uint32_t sid = SaveEditor::GetUIntAt(gMiiSav, BL_H_GOODS_ID, ai);
                            std::string pfx = "Pocket " + std::to_string(slot+1);
                            int pw=0,ph=0; if(fsm) TTF_SizeUTF8(fsm,pfx.c_str(),&pw,&ph);
                            DrawText(pfx, panelX+12, ry+(BL_ROW_H-ph)/2, COL_DIM);
                            const char* lbl = BlGoodsLabel(sid);
                            int lw=0,lh=0; if(fsm) TTF_SizeUTF8(fsm,lbl,&lw,&lh);
                            DrawText(lbl, panelX+12+pw, ry+(BL_ROW_H-lh)/2, sel?COL_GOLD:COL_TEXT);
                        } else {
                            // Action row
                            int act = itemSel - 21;
                            const char* lbl = ACT_LABELS[act];
                            int lw=0,lh=0; if(fsm) TTF_SizeUTF8(fsm,lbl,&lw,&lh);
                            DrawText(lbl, panelX+12, ry+(BL_ROW_H-lh)/2, sel?COL_TEXT:COL_DIM);
                        }
                    }
                }
                // Scrollbar
                if (BL_VIS_ROWS > BL_VIS) {
                    int sbH = panelH * BL_VIS / BL_VIS_ROWS;
                    int sbY = panelTop + panelH * gBlScroll / BL_VIS_ROWS;
                    FillRect(panelX+panelW-5, sbY, 3, sbH, COL_BORDER);
                }
            }
            if (!gMiiStatsMsg.empty())
                DrawTextC(gMiiStatsMsg, panelX+panelW/2, panelTop+panelH-22, gMiiStatsMsgCol, Font::Sm);
        } else if (gMiiStatsSubTab == MiiStatsSubTab::Habits) {
            // ── Habits panel ────────────────────────────────────────────────────
            EnsureHabitHashes();
            TTF_Font* fsm = GetFont(Font::Sm);
            TTF_Font* fmd = GetFont(Font::Md);

            // Category strip (top row)
            const int CAT_H = 34, CAT_GAP = 4;
            int catStripY = panelTop;
            int catX0 = panelX + 8;
            int catW  = (panelW - 16 - (HABIT_CAT_COUNT-1)*CAT_GAP) / HABIT_CAT_COUNT;
            for (int c = 0; c < HABIT_CAT_COUNT; c++) {
                int cx = catX0 + c*(catW+CAT_GAP);
                bool sel = (c == gHabitCatSel);
                // Indicator: gold dot if any habit is currently active in this category
                bool catHasActive = false;
                for (int i = 0; i < HABIT_COUNT; i++) {
                    if (HABITS[i].category != c) continue;
                    if (SaveEditor::GetBoolAt(gMiiSav, gHabitHashes[i].isChecked, miiSlotIdx, false)) {
                        catHasActive = true; break;
                    }
                }
                HitAdd(cx, catStripY, catW, CAT_H, TSelHabitCat, c);
                FillRect(cx, catStripY, catW, CAT_H, sel?COL_SEL:COL_PANEL);
                DrawRect(cx, catStripY, catW, CAT_H, sel?COL_GOLD:COL_BORDER);
                DrawTextC(HABIT_CAT_LABEL[c], cx+catW/2, catStripY+CAT_H/2,
                          sel?COL_GOLD:(catHasActive?COL_ACCENT:COL_DIM), Font::Sm);
                if (catHasActive) {
                    int dotX = cx+catW-9, dotY = catStripY+5;
                    FillRect(dotX, dotY, 4, 4, COL_GOLD);
                }
            }

            // Item list (selected category)
            std::vector<int> items = HabitsInCategory(gHabitCatSel);
            int listTop = catStripY + CAT_H + 8;
            int listH   = panelH - (CAT_H + 8) - 36;
            const int ROW_H = 34;
            int visRows = listH / ROW_H;
            if ((int)items.size() <= visRows) gHabitScroll = 0;
            else {
                if (gHabitItemSel < gHabitScroll) gHabitScroll = gHabitItemSel;
                if (gHabitItemSel >= gHabitScroll+visRows) gHabitScroll = gHabitItemSel-visRows+1;
            }
            if (gHabitItemSel >= (int)items.size()) gHabitItemSel = std::max(0,(int)items.size()-1);

            for (int i = 0; i < visRows && i + gHabitScroll < (int)items.size(); i++) {
                int idx = items[i + gHabitScroll];
                bool selRow = (i + gHabitScroll == gHabitItemSel);
                int ry = listTop + i*ROW_H;
                bool isOwn     = SaveEditor::GetBoolAt(gMiiSav, gHabitHashes[idx].isOwn,     miiSlotIdx, false);
                bool isChecked = SaveEditor::GetBoolAt(gMiiSav, gHabitHashes[idx].isChecked, miiSlotIdx, false);

                // Row backdrop (no hit — the two boxes on the left are the only tap targets)
                FillRect(panelX+8, ry, panelW-16, ROW_H-2, selRow?COL_SEL:COL_BG);
                if (selRow) DrawRect(panelX+8, ry, panelW-16, ROW_H-2, COL_GOLD);

                // Big touch zones — 50px wide, full row height — with the visible 22×22 box centered.
                int rcy = ry + ROW_H/2;
                const int BOX_VIS = 22;          // visible box edge
                const int HIT_W   = 50;          // touch hit width per box

                // Active (gold) box
                int aZoneX = panelX + 8;
                int aBoxX  = aZoneX + (HIT_W - BOX_VIS)/2;
                HitAdd(aZoneX, ry, HIT_W, ROW_H-2, TSelHabitItem, i + gHabitScroll);
                FillRect(aBoxX, rcy-BOX_VIS/2, BOX_VIS, BOX_VIS, isChecked?COL_GOLD:COL_PANEL);
                DrawRect(aBoxX, rcy-BOX_VIS/2, BOX_VIS, BOX_VIS, isChecked?COL_GOLD:COL_BORDER);
                if (isChecked) FillRect(aBoxX+6, rcy-5, 10, 10, {10,10,10,255});

                // Owned (blue) box
                const SDL_Color C_OWN = {90,140,200,255};
                int oZoneX = aZoneX + HIT_W + 4;
                int oBoxX  = oZoneX + (HIT_W - BOX_VIS)/2;
                HitAdd(oZoneX, ry, HIT_W, ROW_H-2, TToggleHabitOwn, i + gHabitScroll);
                FillRect(oBoxX, rcy-BOX_VIS/2, BOX_VIS, BOX_VIS, isOwn?C_OWN:COL_PANEL);
                DrawRect(oBoxX, rcy-BOX_VIS/2, BOX_VIS, BOX_VIS, isOwn?C_OWN:COL_BORDER);
                if (isOwn) FillRect(oBoxX+6, rcy-5, 10, 10, {10,10,10,255});

                // Label
                SDL_Color lblCol = isChecked ? COL_GOLD : (isOwn ? COL_TEXT : COL_DIM);
                DrawText(HABITS[idx].label, oZoneX + HIT_W + 8, ry+(ROW_H-14)/2-2, lblCol, Font::Sm);
                // Label area hit — tapping the text also toggles active (same as the gold checkbox)
                int lblHitX = oZoneX + HIT_W;
                int lblHitW = (panelX + panelW - 8) - lblHitX;
                if (lblHitW > 0)
                    HitAdd(lblHitX, ry, lblHitW, ROW_H-2, TSelHabitItem, i + gHabitScroll);
            }

            if (items.empty()) {
                DrawTextC("no habits in this category", panelX+panelW/2, listTop+listH/2, COL_DIM, Font::Md);
            }

            // Scrollbar
            if ((int)items.size() > visRows) {
                int sbH = listH * visRows / (int)items.size();
                int sbY = listTop + listH * gHabitScroll / (int)items.size();
                FillRect(panelX+panelW-5, sbY, 3, sbH, COL_BORDER);
            }

            // Footer hint inside panel
            const char* hint = "A  toggle active    X  toggle owned    -  clear category    Left/Right  switch category";
            DrawTextC(hint, panelX+panelW/2, panelTop+panelH-14, COL_DIM, Font::Sm);

            (void)fmd;
            if (!gMiiStatsMsg.empty())
                DrawTextC(gMiiStatsMsg, panelX+panelW/2, panelTop+panelH-32, gMiiStatsMsgCol, Font::Sm);
        } else {
            // Stats editor
            const auto& fd = MII_STATS_FIELDS[gMiiStatsFieldSel];
            int cx2 = editX + editW/2;
            int midY2 = panelTop + panelH/2 - 20;

            DrawTextC(fd.label, cx2, midY2-102 - (fd.isStr ? 30 : 0), COL_DIM, Font::Md);
            DrawTextC(fd.desc, cx2, midY2-76 - (fd.isStr ? 30 : 0), COL_DIM, Font::Sm);

            if (fd.isStr) {
                std::string val = SaveEditor::GetWStr32At(gMiiSav, SaveEditor::Hash(fd.fieldName), miiSlotIdx);
                DrawTextC(val.empty()?"(empty)":val, cx2, midY2-70, COL_TEXT, Font::Lg);
                int bw=220, bh=46, bx=cx2-bw/2, by=midY2-20;
                HitAdd(bx,by,bw,bh, TMiiStatsKbd, 0);
                FillRect(bx,by,bw,bh,COL_SEL); DrawRect(bx,by,bw,bh,COL_ACCENT);
                DrawTextC("keyboard", cx2, by+bh/2, COL_ACCENT, Font::Md);
                {
                    static const uint32_t H_HOW_NAME = SaveEditor::Hash("Mii.Name.HowToCallName");
                    std::string how = SaveEditor::GetWStr64At(gMiiSav, H_HOW_NAME, miiSlotIdx);
                    DrawTextC(how.empty()?"(same as name)":how.c_str(), cx2, by+bh+16, how.empty()?COL_DIM:COL_TEXT, Font::Sm);
                    int pw=220,phh=42,px=cx2-pw/2,py=by+bh+32;
                    HitAdd(px,py,pw,phh,TSimBtn,(int)HidNpadButton_X);
                    FillRect(px,py,pw,phh,COL_SEL); DrawRect(px,py,pw,phh,COL_ACCENT);
                    DrawTextC("X  edit pronunciation",cx2,py+phh/2,COL_ACCENT,Font::Sm);
                }
            } else if (fd.isEnum) {
                uint32_t enumVal = SaveEditor::GetEnumAt(gMiiSav, SaveEditor::Hash(fd.fieldName), miiSlotIdx);
                const char* lbl = FeelingName(enumVal);
                DrawTextC(lbl ? lbl : ("?" + std::to_string(enumVal)).c_str(), cx2, midY2-40, COL_TEXT, Font::Lg);
                const int btnW=140, btnH=40, btnGap=24;
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
                DrawTextC(std::to_string(rawVal + fd.dispOffset), cx2, midY2-40, COL_TEXT, Font::Lg);

                static const int STEPS2[]={100,10,1};
                int bw2=60, bh2=64, gap2=6;
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
                    int ubW=100, ubH=44, ubX=cx2-ubW/2, ubY=by2+bh2+20;
                    HitAdd(ubX, ubY, ubW, ubH, TUndoMii, 0);
                    FillRect(ubX, ubY, ubW, ubH, COL_PANEL);
                    DrawRect(ubX, ubY, ubW, ubH, canUndo ? COL_GOLD : COL_BORDER);
                    DrawTextC("Undo", ubX+ubW/2, ubY+ubH/2, canUndo ? COL_GOLD : COL_DIM, Font::Md);
                }
            }

            // Import / Export .ltd section — only shown on Name field (str)
            if (fd.isStr && !gMiis.empty() && gMiiStatsMiiSel < (int)gMiis.size()) {
                // Anchor to panel bottom so it stays fixed regardless of upper block position
                int ibW=250,ibH=66,ibGap=20;
                int ibxL=cx2-ibGap/2-ibW, ibxR=cx2+ibGap/2;
                int ibY = panelTop + panelH - ibH - 20;
                SDL_SetRenderDrawColor(gRen,COL_BORDER.r,COL_BORDER.g,COL_BORDER.b,80);
                SDL_RenderDrawLine(gRen, editX+12, ibY-44, editX+editW-12, ibY-44);
                DrawTextC("Mii file", cx2, ibY-22, COL_DIM, Font::Sm);
                bool ltdImp = (gMiiLtdBtnSel == 0);
                bool ltdExp = (gMiiLtdBtnSel == 1);
                HitAdd(ibxL,ibY,ibW,ibH,TOpenImportBrowser,0);
                FillRect(ibxL,ibY,ibW,ibH,ltdImp?COL_SEL:COL_PANEL);
                DrawRect(ibxL,ibY,ibW,ibH,COL_ACCENT);
                DrawTextC("import .ltd",ibxL+ibW/2,ibY+ibH/2,COL_ACCENT,Font::Lg);
                HitAdd(ibxR,ibY,ibW,ibH,TDoMiiExport,0);
                FillRect(ibxR,ibY,ibW,ibH,ltdExp?COL_SEL:COL_PANEL);
                DrawRect(ibxR,ibY,ibW,ibH,COL_GOLD);
                DrawTextC("export .ltd",ibxR+ibW/2,ibY+ibH/2,COL_GOLD,Font::Lg);
            }

            if (!gMiiStatsMsg.empty())
                DrawTextC(gMiiStatsMsg, cx2, panelTop+16, gMiiStatsMsgCol);
        }
    }

    DrawFooter(
        gMiiStatsSubTab==MiiStatsSubTab::Social
            ? (gSocialExpanded ? "ZL/ZR  mii    Y  subtab    X  focus view    L/R  tab    B/+  back"
                               : "ZL/ZR  mii    Y  subtab    X  global graph    L/R  tab    B/+  back")
        : gMiiStatsSubTab==MiiStatsSubTab::Words
            ? "ZL/ZR  mii    Up/Down  slot    Left/Right  kind    A  text    X  pronunciation    -  undo    Y  subtab    L/R  tab    B/+  back"
        : gMiiStatsSubTab==MiiStatsSubTab::Relations
            ? "ZL/ZR  mii    Up/Down  relation    Left/Right  type    Y  subtab    L/R  tab    B/+  back"
        : gMiiStatsSubTab==MiiStatsSubTab::Belongings
            ? (gBlPickerOpen
                ? "ZL/ZR  item    Up/Down  item    Left/Right  skip 12    A  confirm    B  cancel"
                : "ZL/ZR  mii    Up/Down  item    A  pick item    Left/Right  color    X  clear    Y  subtab    L/R  tab    B/+  back")
        : (gMiiStatsFieldSel==0
            ? "ZL/ZR  mii    Up/Down  field    Left/Right  select    A  confirm    X  pronunciation    Y  subtab    L/R  tab    B/+  back"
            : "ZL/ZR  mii    Up/Down  field    Left/Right  value    X  undo    Y  subtab    L/R  tab    B/+  back"));
    SDL_RenderPresent(gRen);
}

static void DrawAppletWarning() {
    HitClear();
    SDL_SetRenderDrawColor(gRen, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(gRen);

    const int mW = 740, mH = 420;
    int mx = (SCREEN_W - mW) / 2, my = (SCREEN_H - mH) / 2;
    FillRect(mx, my, mW, mH, {18, 14, 10, 255});
    DrawRect(mx, my, mW, mH, COL_GOLD);
    FillRect(mx, my, mW, 4, COL_GOLD);

    DrawTextC("Applet Mode Not Supported", SCREEN_W/2, my + 36, COL_GOLD, Font::Lg);

    SDL_SetRenderDrawColor(gRen, COL_BORDER.r, COL_BORDER.g, COL_BORDER.b, 100);
    SDL_RenderDrawLine(gRen, mx + 24, my + 60, mx + mW - 24, my + 60);

    DrawTextC("TomoToolNX requires full system resources and cannot",
              SCREEN_W/2, my + 84, COL_TEXT, Font::Sm);
    DrawTextC("run from Applet mode (R + Album).",
              SCREEN_W/2, my + 106, COL_TEXT, Font::Sm);

    SDL_SetRenderDrawColor(gRen, COL_BORDER.r, COL_BORDER.g, COL_BORDER.b, 100);
    SDL_RenderDrawLine(gRen, mx + 24, my + 130, mx + mW - 24, my + 130);

    DrawTextC("How to launch correctly", SCREEN_W/2, my + 154, COL_ACCENT, Font::Md);

    // Step boxes
    const int sW = 320, sH = 72, sGap = 20;
    int sxL = SCREEN_W/2 - sGap/2 - sW;
    int sxR = SCREEN_W/2 + sGap/2;
    int sY  = my + 182;

    FillRect(sxL, sY, sW, sH, {20, 28, 20, 255});
    DrawRect(sxL, sY, sW, sH, COL_RED);
    DrawTextC("Wrong way", sxL + sW/2, sY + 18, COL_RED, Font::Sm);
    DrawTextC("open Album", sxL + sW/2, sY + 44, COL_DIM, Font::Sm);

    FillRect(sxR, sY, sW, sH, {16, 28, 16, 255});
    DrawRect(sxR, sY, sW, sH, COL_GREEN);
    DrawTextC("Correct way", sxR + sW/2, sY + 18, COL_GREEN, Font::Sm);
    DrawTextC("Hold R  +  open any game", sxR + sW/2, sY + 44, COL_TEXT, Font::Sm);

    DrawTextC("The game will not launch — the homebrew menu will open instead.",
              SCREEN_W/2, sY + sH + 26, COL_DIM, Font::Sm);

    const int btnW = 180, btnH = 44;
    int bx = SCREEN_W/2 - btnW/2, by = my + mH - 66;
    FillRect(bx, by, btnW, btnH, COL_PANEL);
    DrawRect(bx, by, btnW, btnH, COL_BORDER);
    DrawTextC("+ / B  Exit", SCREEN_W/2, by + btnH/2, COL_DIM, Font::Md);

    DrawFooter("B / +  exit app");
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
    if (gUgcDirty) { SaveMount::Commit(); gUgcDirty=false; }
    if (gWebUiDirty) { SaveMount::Commit(); gWebUiDirty=false; }
    FreePreview(); HttpServer::Stop(); gLog.clear();
    gEntries.clear(); gMiis.clear();
    gPlayerSav=SaveEditor::SavFile{}; gMiiSav=SaveEditor::SavFile{};
    gPlayerSavDirty=false; gMiiSavDirty=false; gUgcDirty=false; gWebUiDirty=false;
    memset(gPlayerUndoValid, 0, sizeof(gPlayerUndoValid));
    gNameLangUndoValid=false; gIslandLangUndoValid=false;
    memset(gMiiUndoValid,    0, sizeof(gMiiUndoValid));
    gWordUndo.valid = false;
    SaveMount::Unmount();
    gSaveFeedbackFrames=90; gSaveFeedbackQuit=!gBackPromptToUserPick;
    gScreen=Screen::SaveFeedback;
}
static void TAckBackNo(int) {
    gShowBackPrompt = false;
    FreePreview(); HttpServer::Stop(); gLog.clear();
    gEntries.clear(); gMiis.clear();
    gPlayerSav=SaveEditor::SavFile{}; gMiiSav=SaveEditor::SavFile{};
    gPlayerSavDirty=false; gMiiSavDirty=false; gUgcDirty=false; gWebUiDirty=false;
    memset(gPlayerUndoValid, 0, sizeof(gPlayerUndoValid));
    gNameLangUndoValid=false; gIslandLangUndoValid=false;
    memset(gMiiUndoValid,    0, sizeof(gMiiUndoValid));
    gWordUndo.valid = false;
    SaveMount::Unmount();
    if (gBackPromptToUserPick) gScreen = Screen::UserPick;
    else gQuitApp = true;
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

    const int mW = 820, mH = 310;
    int mx = (SCREEN_W - mW) / 2, my = (SCREEN_H - mH) / 2;
    FillRect(mx, my, mW, mH, {18, 22, 18, 255});
    DrawRect(mx, my, mW, mH, COL_GOLD);

    DrawTextC("About inventory thumbnails", SCREEN_W/2, my + 32, COL_GOLD, Font::Lg);

    DrawTextC("Your texture was imported, but the inventory thumbnail", SCREEN_W/2, my + 88, COL_TEXT, Font::Md);
    DrawTextC("is managed by the game itself and won't update on its own.", SCREEN_W/2, my + 114, COL_TEXT, Font::Md);
    DrawTextC("To refresh it: open this item in Studio Workshop,", SCREEN_W/2, my + 152, COL_DIM, Font::Md);
    DrawTextC("enter its texture editor, and re-save — the game", SCREEN_W/2, my + 176, COL_DIM, Font::Md);
    DrawTextC("will automatically rebuild the thumbnail.", SCREEN_W/2, my + 200, COL_DIM, Font::Md);

    const int btnW = 220, btnH = 44;
    int bx = SCREEN_W/2 - btnW/2, by = my + mH - 62;
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

// ── Background-removal modal ───────────────────────────────────────────────

static void TConfirmBgRemove(int);
static void TCancelBgRemove(int) { gShowBgRemovePrompt = false; }

static void DrawBgRemovePrompt() {
    HitClear();
    SDL_SetRenderDrawColor(gRen, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(gRen);
    const int mW = 580, mH = 230;
    int mx = (SCREEN_W - mW) / 2, my = (SCREEN_H - mH) / 2;
    FillRect(mx, my, mW, mH, {18, 22, 30, 255});
    DrawRect(mx, my, mW, mH, COL_ACCENT);
    DrawTextC("Remove Background?", SCREEN_W/2, my + 36, COL_ACCENT, Font::Lg);
    DrawTextC("The selected texture will be modified in-place.", SCREEN_W/2, my + 88, COL_TEXT, Font::Md);
    DrawTextC("This may take up to 30 seconds.", SCREEN_W/2, my + 114, COL_DIM, Font::Sm);
    const int bW = 180, bH = 46, gap = 28;
    int by2 = my + mH - 62;
    HitAdd(mx + mW/2 - bW - gap/2, by2, bW, bH, TConfirmBgRemove, 0);
    FillRect(mx + mW/2 - bW - gap/2, by2, bW, bH, COL_SEL);
    DrawRect(mx + mW/2 - bW - gap/2, by2, bW, bH, COL_ACCENT);
    DrawTextC("A  Confirm", mx + mW/2 - gap/2 - bW/2, by2 + bH/2, COL_ACCENT, Font::Md);
    HitAdd(mx + mW/2 + gap/2, by2, bW, bH, TCancelBgRemove, 0);
    FillRect(mx + mW/2 + gap/2, by2, bW, bH, COL_SEL);
    DrawRect(mx + mW/2 + gap/2, by2, bW, bH, {90,140,200,255});
    DrawTextC("B  Cancel", mx + mW/2 + gap/2 + bW/2, by2 + bH/2, {90,140,200,255}, Font::Md);
    DrawFooter("A  confirm    B  cancel");
    SDL_RenderPresent(gRen);
}

static void DrawBgRemoving() {
    SDL_SetRenderDrawColor(gRen, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(gRen);
    DrawTextC("Removing background...", SCREEN_W/2, SCREEN_H/2 - 18, COL_ACCENT, Font::Lg);
    DrawTextC("This may take up to 30 seconds.", SCREEN_W/2, SCREEN_H/2 + 22, COL_DIM, Font::Md);
    SDL_RenderPresent(gRen);
}

struct BgAnimState { int frame = 0; };

static void OnBgRemoveProgress(int done, int total, void* ud) {
    static const char kSpinner[] = "|/-\\";
    auto* s = static_cast<BgAnimState*>(ud);
    s->frame++;
    SDL_SetRenderDrawColor(gRen, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(gRen);
    DrawTextC("Removing background...", SCREEN_W/2, SCREEN_H/2 - 60, COL_ACCENT, Font::Lg);
    // Spinner
    char spin[2] = {kSpinner[s->frame % 4], '\0'};
    DrawTextC(spin, SCREEN_W/2, SCREEN_H/2 - 10, {90,140,200,255}, Font::Lg);
    // Progress bar
    const int barW = 480, barH = 12;
    int barX = (SCREEN_W - barW) / 2, barY = SCREEN_H/2 + 20;
    float pct = (total > 0) ? (float)done / total : 0.f;
    FillRect(barX, barY, barW, barH, COL_PANEL);
    DrawRect(barX, barY, barW, barH, COL_BORDER);
    if (pct > 0.f) FillRect(barX+1, barY+1, (int)((barW-2)*pct), barH-2, {90,140,200,255});
    DrawTextC("This may take up to 30 seconds.", SCREEN_W/2, SCREEN_H/2 + 52, COL_DIM, Font::Md);
    SDL_RenderPresent(gRen);
}

static void TConfirmBgRemove(int) {
    gShowBgRemovePrompt = false;
    if (gEntries.empty()) return;
    const auto& e = gEntries[gEntrySel];

    // Show loading screen before blocking inference
    DrawBgRemoving();

    // Decode current texture to sRGB
    RgbaImage img;
    std::string err = TextureProcessor::DecodeFile(e.ugctexPath, img);
    if (!err.empty()) { gOnSwitchMsg = err; gOnSwitchMsgCol = COL_RED; return; }

    // Run u2netp inference with animated progress
    BgAnimState bgAnim{};
    err = U2Net::RemoveBackground(img, kBgModelPath, OnBgRemoveProgress, &bgAnim);
    if (!err.empty()) { gOnSwitchMsg = "BG remove: " + err; gOnSwitchMsgCol = COL_RED; return; }

    // Re-encode and overwrite files on disk
    TextureProcessor::ImportOptions opts;
    opts.destStem           = e.directory() + "/" + e.stem;
    opts.writeCanvas        = e.hasCanvas();
    opts.writeThumb         = e.hasThumb();
    opts.thumbPath          = e.thumbPath;
    opts.noSrgb             = false;
    opts.originalUgctexPath = e.ugctexPath;
    err = TextureProcessor::ImportRgbaImage(img, opts);
    if (!err.empty()) { gOnSwitchMsg = err; gOnSwitchMsgCol = COL_RED; return; }

    // Reload preview and mark dirty
    gEntries = UgcScanner::Scan(SAVE_UGC_PATH);
    MiiManager::LoadUgcNames();
    RebuildUgcFilter();
    if (gEntrySel >= (int)gEntries.size()) gEntrySel = (int)gEntries.size() - 1;
    if (!gEntries.empty()) LoadPreview(gEntries[gEntrySel]);
    gUgcDirty = true;
    gOnSwitchMsg = "Background removed — save or discard on exit";
    gOnSwitchMsgCol = COL_GREEN;
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
    const int bW=180, bH=48, gap=30, bOff=90;
    const char* yesLbl = gBackPromptToUserPick ? "A  Save & Back" : "A  Save & Exit";
    const char* noLbl  = gBackPromptToUserPick ? "B  Discard & Back" : "B  Discard & Exit";
    const char* footer = gBackPromptToUserPick ? "A  save & back    B  discard & back    X  cancel"
                                               : "A  save & exit    B  discard & exit    X  cancel";
    HitAdd(mx+mW/2-bW-gap/2, my+mH-bOff, bW, bH, TAckBackYes, 0);
    FillRect(mx+mW/2-bW-gap/2, my+mH-bOff, bW, bH, COL_SEL);
    DrawRect(mx+mW/2-bW-gap/2, my+mH-bOff, bW, bH, COL_GREEN);
    DrawTextC(yesLbl, mx+mW/2-gap/2-bW/2, my+mH-bOff+bH/2, COL_GREEN, Font::Md);
    HitAdd(mx+mW/2+gap/2, my+mH-bOff, bW, bH, TAckBackNo, 0);
    FillRect(mx+mW/2+gap/2, my+mH-bOff, bW, bH, COL_SEL);
    DrawRect(mx+mW/2+gap/2, my+mH-bOff, bW, bH, COL_RED);
    DrawTextC(noLbl, mx+mW/2+gap/2+bW/2, my+mH-bOff+bH/2, COL_RED, Font::Md);
    DrawFooter(footer);
    SDL_RenderPresent(gRen);
}
static void TUndoPlayer(int) {
    if (!gPlayerUndoValid[gPlayerFieldSel]) return;
    const auto& fd = PLAYER_FIELDS[gPlayerFieldSel];
    uint32_t h = PFHash(fd);
    uint32_t v = (uint32_t)gPlayerUndoVal[gPlayerFieldSel];
    if (fd.isEnum)          SaveEditor::SetEnum(gPlayerSav, h, v);
    else if (fd.isSkinTone) SaveEditor::SetUInt(gPlayerSav, h, v);
    else if (fd.isUInt)     SaveEditor::SetUInt(gPlayerSav, h, v);
    else if (fd.isRegion)   SaveEditor::SetEnum(gPlayerSav, h, v);
    else                    SaveEditor::SetAnyScalar(gPlayerSav, h, v);
    gPlayerUndoValid[gPlayerFieldSel] = false;
    gPlayerSavDirty = true;
}
[[maybe_unused]] static void TUndoNameLang(int) {
    if (!gNameLangUndoValid || !gPlayerSav.loaded) return;
    SaveEditor::SetEnum(gPlayerSav, SaveEditor::Hash("Player.NameRegionLanguageID"), (uint32_t)gNameLangUndo);
    gNameLangUndoValid = false;
    gPlayerSavDirty = true;
}
[[maybe_unused]] static void TUndoIslandLang(int) {
    if (!gIslandLangUndoValid || !gPlayerSav.loaded) return;
    SaveEditor::SetEnum(gPlayerSav, SaveEditor::Hash("Player.IslandNameRegionLanguageID"), (uint32_t)gIslandLangUndo);
    gIslandLangUndoValid = false;
    gPlayerSavDirty = true;
}
static void TUndoMii(int) {
    if (!gMiiUndoValid[gMiiStatsFieldSel]) return;
    if (gMiis.empty() || gMiiStatsMiiSel >= (int)gMiis.size()) return;
    const auto& fd = MII_STATS_FIELDS[gMiiStatsFieldSel];
    uint32_t h = SaveEditor::Hash(fd.fieldName);
    int miiIdx = gMiis[gMiiStatsMiiSel].slot - 1;
    if (fd.isUInt) SaveEditor::SetUIntAt(gMiiSav, h, miiIdx, (uint32_t)gMiiUndoVal[gMiiStatsFieldSel]);
    else           SaveEditor::SetIntAt(gMiiSav, h, miiIdx, gMiiUndoVal[gMiiStatsFieldSel]);
    gMiiUndoValid[gMiiStatsFieldSel] = false;
    gMiiSavDirty = true;
}
static void TUndoWords(int) {
    if (!gWordUndo.valid) return;
    if (gMiis.empty() || gMiiStatsMiiSel >= (int)gMiis.size()) return;
    static const uint32_t H_WKIND_U = SaveEditor::Hash("Mii.MiiMisc.WordInfo.WordArray.WordKind");
    static const uint32_t H_WTXT_U  = SaveEditor::Hash("Mii.MiiMisc.WordInfo.WordArray.WordText");
    static const uint32_t H_WHOW_U  = SaveEditor::Hash("Mii.MiiMisc.WordInfo.WordArray.WordHowToCall");
    int miiIdx = gMiis[gMiiStatsMiiSel].slot - 1;
    int wIdx   = miiIdx * 12 + gMiiWordsSlotSel;
    SaveEditor::SetAnyEnumAt(gMiiSav, H_WKIND_U, wIdx, gWordUndo.kind);
    SaveEditor::SetWStr64At(gMiiSav,  H_WTXT_U,  wIdx, gWordUndo.text);
    SaveEditor::SetWStr64At(gMiiSav,  H_WHOW_U,  wIdx, gWordUndo.how);
    gWordUndo.valid = false;
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

// Mark s_touch.dragging only when a scroll variable actually changes value.
// Called unconditionally from FINGERMOTION so any drift that doesn't complete a
// scroll step is invisible to the tap-fire logic.
static void ApplyTouchScroll(float ddx, float ddy) {
    if (gScreen == Screen::UserPick) {
        int prev = gUserSel;
        s_touch.accumX += ddx;
        while (s_touch.accumX >=  100.f && gUserSel > 0)
            { s_touch.accumX -= 100.f; gUserSel--; }
        while (s_touch.accumX <= -100.f && gUserSel < (int)gUsers.size()-1)
            { s_touch.accumX += 100.f; gUserSel++; }
        if (gUserSel != prev) s_touch.dragging = true;
        return;
    }
    if (gScreen != Screen::OnSwitch) return;

    if (gShowFileBrowser) {
        int itemTop = gBrowseForExportDir ? 106 : 88;
        if (s_touch.startY < itemTop || s_touch.startY >= SCREEN_H-30) return;
        const int ph   = 26;
        const int pvis = (SCREEN_H - (gBrowseForExportDir ? 166 : 148)) / ph;
        int prev = gBrowseScroll;
        s_touch.accumY += ddy;
        while (s_touch.accumY >=  ph) { s_touch.accumY -= ph; gBrowseScroll = std::max(0, gBrowseScroll-1); }
        while (s_touch.accumY <= -ph) { s_touch.accumY += ph; gBrowseScroll = std::min(gBrowseScroll+1, std::max(0,(int)gBrowseEntries.size()-pvis)); }
        if (gBrowseScroll != prev) s_touch.dragging = true;
        return;
    }

    if (gOnSwitchMode == OnSwitchMode::UGC && !gEntries.empty()) {
        if (s_touch.startX < LIST_X || s_touch.startX >= LIST_X+LIST_W) return;
        if (s_touch.startY < LIST_Y || s_touch.startY >= LIST_Y+LIST_H) return;
        int prev = gEntryScroll;
        s_touch.accumY += ddy;
        while (s_touch.accumY >=  ITEM_H) { s_touch.accumY -= ITEM_H; gEntryScroll = std::max(0, gEntryScroll-1); }
        while (s_touch.accumY <= -ITEM_H) { s_touch.accumY += ITEM_H; gEntryScroll = std::min(gEntryScroll+1, std::max(0,(int)gEntries.size()-VISIBLE)); }
        if (gEntryScroll != prev) s_touch.dragging = true;
    }

    if (gOnSwitchMode == OnSwitchMode::MiiStats) {
        if (s_touch.startX >= SE_LIST_X && s_touch.startX < SE_LIST_X + MSE_LIST_W
            && s_touch.startY >= SE_TOP_Y && s_touch.startY < SCREEN_H - 32
            && !gMiis.empty()) {
            int mseVis = (SCREEN_H - SE_TOP_Y - 32) / ITEM_H;
            int prev = gMiiStatsScroll;
            s_touch.accumY += ddy;
            while (s_touch.accumY >=  ITEM_H) { s_touch.accumY -= ITEM_H; gMiiStatsScroll = std::max(0, gMiiStatsScroll - 1); }
            while (s_touch.accumY <= -ITEM_H) { s_touch.accumY += ITEM_H; gMiiStatsScroll = std::min(gMiiStatsScroll + 1, std::max(0, (int)gMiis.size() - mseVis)); }
            if (gMiiStatsScroll != prev) s_touch.dragging = true;
            return;
        }

        const int panelTop = SE_TOP_Y + 32;
        const int panelH   = SCREEN_H - panelTop - 32;
        const int midX2    = SE_LIST_X + MSE_LIST_W + 12;
        const int midW2    = 340;
        const int blPanX   = midX2;
        const int blPanW   = SCREEN_W - blPanX - 8;

        if (gMiiStatsSubTab == MiiStatsSubTab::Relations) {
            if (s_touch.startX < midX2 || s_touch.startX >= midX2 + midW2) return;
            if (s_touch.startY < SE_TOP_Y || s_touch.startY >= SCREEN_H - 32) return;
            const int relVis = (SCREEN_H - 32 - (SE_TOP_Y + 2)) / 34;
            int prev = gMiiRelScroll;
            s_touch.accumY += ddy;
            while (s_touch.accumY >=  ITEM_H) { s_touch.accumY -= ITEM_H; gMiiRelScroll = std::max(0, gMiiRelScroll - 1); }
            while (s_touch.accumY <= -ITEM_H) { s_touch.accumY += ITEM_H; gMiiRelScroll = std::min(gMiiRelScroll + 1, std::max(0, gMiiRelCount - relVis)); }
            if (gMiiRelScroll != prev) s_touch.dragging = true;
        } else if (gMiiStatsSubTab == MiiStatsSubTab::Belongings) {
            if (s_touch.startX < blPanX || s_touch.startX >= blPanX + blPanW) return;
            if (gBlPickerOpen) {
                const int PK_HDR_H = 32;
                const int listTop  = panelTop + PK_HDR_H;
                const int listH    = panelH - PK_HDR_H;
                if (s_touch.startY < listTop || s_touch.startY >= listTop + listH) return;
                const int total = (int)gBlFiltered.size();
                int prev = gBlPickerScroll;
                s_touch.accumY += ddy;
                {
                    const int pkVis = listH / 30;
                    while (s_touch.accumY >=  ITEM_H) { s_touch.accumY -= ITEM_H; gBlPickerScroll = std::max(0, gBlPickerScroll - 1); }
                    while (s_touch.accumY <= -ITEM_H) { s_touch.accumY += ITEM_H; gBlPickerScroll = std::min(gBlPickerScroll + 1, std::max(0, total - pkVis)); }
                }
                if (gBlPickerScroll != prev) s_touch.dragging = true;
            } else {
            }
        }
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
        const int cw=460, ch=148, gap=20;
        const int bx = SCREEN_W/2 - (2*cw + gap)/2;
        const int by = SCREEN_H/2 - ch/2 + 10;
        if      (hit(bx,        by, cw, ch)) kDown |= HidNpadButton_A;
        else if (hit(bx+cw+gap, by, cw, ch)) kDown |= HidNpadButton_B;
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
            const int btnW=155, btnGap=12, bgSep=36;
            int bx0  = cx - (4*btnW + 3*btnGap + bgSep)/2;
            int bxBg = bx0 + 3*(btnW+btnGap) + bgSep;
            if (hit(bx0,             btnY, btnW, kBtnH)) kDown |= HidNpadButton_A;
            if (hit(bx0+btnW+btnGap, btnY, btnW, kBtnH)) kDown |= HidNpadButton_Y;
            if (hit(bxBg,            btnY, btnW, kBtnH)) kDown |= HidNpadButton_X;
        }
    }
}


// ─── Main ─────────────────────────────────────────────────────────────────────

int main(int,char**) {
    romfsInit();
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

    if (appletGetAppletType() != AppletType_Application) {
        gScreen = Screen::AppletWarning;
    } else {

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

    } // end else (not applet mode)

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
                if (ev.type == SDL_FINGERDOWN) {
                    // Always reset on any new finger — prevents stuck-active state when
                    // a previous FINGERUP was silently dropped by the platform.
                    s_touch = { true, false,
                                ev.tfinger.x*SCREEN_W, ev.tfinger.y*SCREEN_H,
                                0.f, 0.f, ev.tfinger.fingerId };
                }
                else if (ev.type == SDL_FINGERMOTION && s_touch.active
                         && ev.tfinger.fingerId == s_touch.fid) {
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

        // + returns to user select globally; OnSwitch tabs handle it themselves (auto-save)
        bool plusOwned = gScreen == Screen::OnSwitch;
        if ((kDown&HidNpadButton_Plus) && !plusOwned) break;

        kDown |= gSimKDown;
        gSimKDown = 0;
        u64 kNav = NavRepeat(kDown, kHeld);

        switch (gScreen) {

        case Screen::AppletWarning:
            if (kDown&(HidNpadButton_B|HidNpadButton_Plus)) gQuitApp=true;
            DrawAppletWarning();
            break;

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
            if (kDown&HidNpadButton_X) { TOpenSettings(0); break; }
            if (kDown&HidNpadButton_A) {
                // Mount save first so we can check backup state
                gScreen=Screen::Mounting;
                DrawMounting();
                {
                    std::string err=SaveMount::Mount(gUsers[gUserSel].uid);
                    if (!err.empty()){gError=err;gScreen=Screen::Error;break;}
                }
                gBackupFull = (BackupService::CountBackups() >= gMaxBackups);
                gScreen=Screen::BackupPrompt;
            }
            DrawUserPick();
            break;

        case Screen::Settings:
            if (gShowRestorePicker) {
                const int RVIS = 7;
                if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp)) {
                    if (gRestoreSel > 0) { gRestoreSel--; if (gRestoreSel < gRestoreScroll) gRestoreScroll = gRestoreSel; }
                }
                if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown)) {
                    if (gRestoreSel+1 < (int)gRestoreList.size()) {
                        gRestoreSel++;
                        if (gRestoreSel >= gRestoreScroll+RVIS) gRestoreScroll = gRestoreSel-RVIS+1;
                    }
                }
                if (kDown&HidNpadButton_B) { gShowRestorePicker = false; }
                if (kDown&HidNpadButton_A && !gRestoreList.empty()) {
                    if (gUsers.empty()) {
                        gSettingsMsg = "No user selected."; gSettingsMsgCol = COL_RED;
                        gShowRestorePicker = false;
                    } else {
                        std::string merr = SaveMount::Mount(gUsers[gUserSel].uid);
                        if (!merr.empty()) {
                            gSettingsMsg = "Mount failed: " + merr; gSettingsMsgCol = COL_RED;
                            gShowRestorePicker = false;
                        } else {
                            SDL_SetRenderDrawColor(gRen, COL_BG.r, COL_BG.g, COL_BG.b, 255);
                            SDL_RenderClear(gRen);
                            DrawTextC("Restoring backup...", SCREEN_W/2, SCREEN_H/2 - 18, COL_ACCENT, Font::Lg);
                            DrawTextC("Please wait.", SCREEN_W/2, SCREEN_H/2 + 22, COL_DIM, Font::Md);
                            SDL_RenderPresent(gRen);
                            std::string rerr = BackupService::RestoreBackup(gRestoreList[gRestoreSel], "tomodata:/");
                            SaveMount::Commit();
                            SaveMount::Unmount();
                            gShowRestorePicker = false;
                            if (rerr.empty()) { gSettingsMsg = "Backup restored successfully."; gSettingsMsgCol = COL_GREEN; }
                            else              { gSettingsMsg = "Restore failed: " + rerr;       gSettingsMsgCol = COL_RED; }
                        }
                    }
                }
            } else if (gShowFileBrowser) {
                int pvis = (SCREEN_H - 148) / 26;
                if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp)) {
                    if (gBrowseSel > 0) { gBrowseSel--; if (gBrowseSel < gBrowseScroll) gBrowseScroll = gBrowseSel; }
                }
                if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown)) {
                    if (gBrowseSel+1 < (int)gBrowseEntries.size()) {
                        gBrowseSel++;
                        if (gBrowseSel >= gBrowseScroll+pvis) gBrowseScroll = gBrowseSel-pvis+1;
                    }
                }
                if (kDown&HidNpadButton_A && !gBrowseEntries.empty()) {
                    auto& entry = gBrowseEntries[gBrowseSel];
                    if (entry.isDir) {
                        std::string newPath;
                        if (entry.name == "[..]") {
                            size_t pos = gBrowseCurPath.rfind('/');
                            newPath = (pos == 0) ? "/" : gBrowseCurPath.substr(0, pos);
                        } else {
                            newPath = gBrowseCurPath + "/" + entry.name;
                        }
                        BrowseRefresh(newPath);
                    }
                }
                if (kDown&HidNpadButton_B && gBrowseCurPath != "/") {
                    size_t pos = gBrowseCurPath.rfind('/');
                    BrowseRefresh((pos == 0) ? "/" : gBrowseCurPath.substr(0, pos));
                }
                if (kDown&HidNpadButton_Y) {
                    gExportPath = gBrowseCurPath;
                    mkdir(gExportPath.c_str(), 0777);
                    SaveConfig();
                    gShowFileBrowser = false;
                    gSettingsMsg = "Export path set to: " + gExportPath;
                    gSettingsMsgCol = COL_GREEN;
                }
                if (kDown&HidNpadButton_X) { gShowFileBrowser = false; }
            } else {
                if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp))
                    gSettingsSel = std::max(0, gSettingsSel - 1);
                if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown))
                    gSettingsSel = std::min(2, gSettingsSel + 1);
                if (gSettingsSel == 2 && kDown&HidNpadButton_A) {
                    gRestoreList   = BackupService::ListBackups();
                    gRestoreSel    = 0;
                    gRestoreScroll = 0;
                    gShowRestorePicker = true;
                }
                if (gSettingsSel == 0 && kDown&HidNpadButton_A) {
                    gBrowseForExportDir = true; gBrowseForMii = false;
                    std::string sp = gExportPath;
                    struct stat _st;
                    if (stat(sp.c_str(),&_st)!=0||!S_ISDIR(_st.st_mode)) {
                        size_t p=sp.rfind('/'); sp=(p&&p!=std::string::npos)?sp.substr(0,p):"/";
                    }
                    BrowseRefresh(sp); gShowFileBrowser = true;
                }
                if (gSettingsSel == 1) {
                    if (kNav&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft)) {
                        gMaxBackups = std::max(1, gMaxBackups - 1); SaveConfig();
                    }
                    if (kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight)) {
                        gMaxBackups = std::min(99, gMaxBackups + 1); SaveConfig();
                    }
                }
                if (kDown&HidNpadButton_B) { gScreen = Screen::UserPick; }
            }
            DrawSettings();
            break;

        case Screen::BackupPrompt:
            if (kDown&HidNpadButton_A) {
                if (gBackupFull) BackupService::DeleteOldestBackup();
                BackupService::StartFullBackup("tomodata:/");
                gScreen=Screen::BackingUp;
            }
            if (kDown&HidNpadButton_B) {
                gScreen=Screen::OnSwitch;
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
                HttpServer::SetSaveWarnAcked(gSaveWarningAcked);
                gEntries=UgcScanner::Scan(SAVE_UGC_PATH);
                MiiManager::LoadUgcNames();
                gUgcFilter.clear();
                RebuildUgcFilter();
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
                       RebuildUgcFilter();
                       if (!gEntries.empty()) LoadPreview(gEntries[gEntrySel < (int)gEntries.size() ? gEntrySel : 0]); }
            }
            if (HttpServer::HasPendingBgRemove()) {
                auto job = HttpServer::TakePendingBgRemove();
                DrawBgRemoving();
                RgbaImage img;
                std::string bgErr = TextureProcessor::DecodeFile(job.ugctexPath, img, false);
                BgAnimState bgAnim{};
                if (bgErr.empty()) bgErr = U2Net::RemoveBackground(img, kBgModelPath, OnBgRemoveProgress, &bgAnim);
                if (bgErr.empty()) bgErr = TextureProcessor::ImportRgbaImage(img, job.opts);
                HttpServer::FinishBgRemove(bgErr);
                if (!bgErr.empty()) LogERR("BgRemove: "+bgErr);
                else { LogOK("BgRemove OK"); gEntries=UgcScanner::Scan(SAVE_UGC_PATH);
                       MiiManager::LoadUgcNames();
                       RebuildUgcFilter();
                       if (!gEntries.empty()) LoadPreview(gEntries[gEntrySel < (int)gEntries.size() ? gEntrySel : 0]);
                       gUgcDirty=true; }
            }
            if (HttpServer::HasPendingCommit()) {
                HttpServer::ClearPendingCommit();
                gWebUiDirty = true;
                LogINF("WebUI change pending — will save on exit");
            }
            if (HttpServer::HasPendingMiiRefresh()) {
                HttpServer::ClearPendingMiiRefresh();
                gMiis = MiiManager::ListMiis();
                if (gMiiStatsMiiSel >= (int)gMiis.size()) gMiiStatsMiiSel = (int)gMiis.size()-1;
                if (!gMiiSavDirty) {
                    gMiiSav = SaveEditor::SavFile{};
                    std::string lerr;
                    if (SaveEditor::Load(SAVE_MII_SAV, gMiiSav, lerr))
                        gMiiSavDirty = true;
                }
                LogOK("Mii list updated");
            }
            if (HttpServer::HasPendingPlayerSavReload()) {
                HttpServer::ClearPendingPlayerSavReload();
                if (gPlayerSav.loaded) {
                    std::string lerr;
                    SaveEditor::Load(SAVE_PLAYER_SAV, gPlayerSav, lerr);
                    gPlayerSavDirty = false;
                    memset(gPlayerUndoValid, 0, sizeof(gPlayerUndoValid));
                    gNameLangUndoValid = false; gIslandLangUndoValid = false;
                    LogOK("Player save synced from WebUI");
                }
            }
            if (HttpServer::HasPendingMiiSavReload()) {
                HttpServer::ClearPendingMiiSavReload();
                if (gMiiSav.loaded) {
                    std::string lerr;
                    SaveEditor::Load(SAVE_MII_SAV, gMiiSav, lerr);
                    gMiiSavDirty = false;
                    memset(gMiiUndoValid, 0, sizeof(gMiiUndoValid));
                    gWordUndo.valid = false;
                    LogOK("Mii save synced from WebUI");
                }
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
            // Background-removal confirm modal
            if (gShowBgRemovePrompt) {
                if (kDown & HidNpadButton_A) TConfirmBgRemove(0);
                if (kDown & HidNpadButton_B) TCancelBgRemove(0);
                if (gShowBgRemovePrompt) DrawBgRemovePrompt();
                break;
            }
            // Back-prompt modal
            if (gShowBackPrompt) {
                if (kDown & HidNpadButton_A) TAckBackYes(0);
                if (kDown & HidNpadButton_B) TAckBackNo(0);
                if (kDown & HidNpadButton_X) gShowBackPrompt = false;
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
                        if (gBrowseForMii) {
                            DoMiiImport(fullPath);
                        } else {
                            std::string lower=entry.name;
                            for(auto& c:lower)c=(char)tolower((unsigned char)c);
                            bool isLtd = lower.size()>=5 && lower.compare(lower.size()-5,4,".ltd")==0;
                            if(isLtd) DoUgcImportLtd(fullPath);
                            else DoOnSwitchImport(fullPath);
                        }
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
                // Tab switch (cycle: UGC → MiiStats → Player → WebUI → UGC)
                if (kDown&HidNpadButton_R){
                    if (!gSaveWarningAcked) {
                        gSaveWarningTarget=OnSwitchMode::MiiStats; gShowSaveWarning=true; gSaveWarningCountdown=120; break;
                    }
                    gOnSwitchMode=OnSwitchMode::MiiStats;
                    if (!gMiiSav.loaded) {
                        std::string err;
                        if (!SaveEditor::Load(SAVE_MII_SAV, gMiiSav, err))
                            gMiiStatsMsg="Load error: "+err;
                    }
                    gOnSwitchMsg=""; break;
                }
                if (kDown&HidNpadButton_L){
                    gOnSwitchMode=OnSwitchMode::WebUI; gOnSwitchMsg=""; break;
                }
                // Filtered-list navigation helpers (gEntrySel is real index, gEntryScroll is filtered position)
                auto navPrev = [&](){
                    if (gFilteredEntries.empty()) return;
                    int p = UgcFilterPos(gEntrySel);
                    if (p < 0) p = 0;
                    p = (p > 0) ? p - 1 : (int)gFilteredEntries.size() - 1;
                    gEntrySel = gFilteredEntries[p];
                    if (p < gEntryScroll) gEntryScroll = p;
                    if (p >= gEntryScroll + VISIBLE) gEntryScroll = p - VISIBLE + 1;
                    LoadPreview(gEntries[gEntrySel]);
                };
                auto navNext = [&](){
                    if (gFilteredEntries.empty()) return;
                    int p = UgcFilterPos(gEntrySel);
                    if (p < 0) p = 0;
                    p = (p + 1 < (int)gFilteredEntries.size()) ? p + 1 : 0;
                    gEntrySel = gFilteredEntries[p];
                    if (p >= gEntryScroll + VISIBLE) gEntryScroll = p - VISIBLE + 1;
                    if (p < gEntryScroll) gEntryScroll = p;
                    LoadPreview(gEntries[gEntrySel]);
                };
                if ((kNav&HidNpadButton_ZL) || (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp))) navPrev();
                if ((kNav&HidNpadButton_ZR) || (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown))) navNext();
                if (kDown&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft) && !gFilteredEntries.empty()){
                    gEntrySel = gFilteredEntries.front(); gEntryScroll = 0; LoadPreview(gEntries[gEntrySel]);
                }
                if (kDown&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight) && !gFilteredEntries.empty()){
                    gEntrySel = gFilteredEntries.back();
                    gEntryScroll = std::max(0, (int)gFilteredEntries.size() - VISIBLE);
                    LoadPreview(gEntries[gEntrySel]);
                }
                if (kDown&HidNpadButton_Minus){
                    TUgcOpenSearch(0);
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
                if (kDown&HidNpadButton_X && !gEntries.empty()) {
                    FILE* mf = fopen(kBgModelPath, "rb");
                    if (!mf) {
                        gOnSwitchMsg = "u2netp.bin missing from romfs (corrupt install?)";
                        gOnSwitchMsgCol = COL_RED;
                    } else {
                        fclose(mf);
                        gShowBgRemovePrompt = true;
                    }
                }
                if (kDown&(HidNpadButton_B|HidNpadButton_Plus)){
                    bool hasDirty = gPlayerSavDirty || gMiiSavDirty || gUgcDirty || gWebUiDirty;
                    bool toUserPick = (kDown&HidNpadButton_Plus) == 0;
                    if (hasDirty) {
                        gShowBackPrompt=true;
                        gBackPromptIsMii=gMiiSavDirty; gBackPromptToUserPick=toUserPick;
                    } else if (toUserPick) {
                        FreePreview(); HttpServer::Stop(); gLog.clear();
                        gEntries.clear(); gMiis.clear();
                        gPlayerSav=SaveEditor::SavFile{}; gMiiSav=SaveEditor::SavFile{};
                        gPlayerSavDirty=false; gMiiSavDirty=false; gUgcDirty=false;
                        SaveMount::Unmount(); gScreen=Screen::UserPick;
                    } else { gQuitApp=true; }
                }
            } else if (gOnSwitchMode == OnSwitchMode::Player) {
                // Lazy-load
                if (!gPlayerSav.loaded) {
                    std::string err;
                    if (!SaveEditor::Load(SAVE_PLAYER_SAV, gPlayerSav, err))
                        gPlayerMsg = "Load error: " + err;
                }
                // Tab switch (changes stay in memory; prompt only on back/quit)
                if (kDown&HidNpadButton_L){
                    gOnSwitchMode=OnSwitchMode::MiiStats;
                    if (!gMiiSav.loaded) {
                        std::string err;
                        if (!SaveEditor::Load(SAVE_MII_SAV, gMiiSav, err))
                            gMiiStatsMsg="Load error: "+err;
                    }
                    gOnSwitchMsg=""; break;
                }
                if (kDown&HidNpadButton_R){
                    gOnSwitchMode=OnSwitchMode::WebUI; gOnSwitchMsg=""; break;
                }
                // Field navigation
                if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp|HidNpadButton_ZL))
                    gPlayerFieldSel = (gPlayerFieldSel>0) ? gPlayerFieldSel-1 : PLAYER_FIELD_COUNT-1;
                if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown|HidNpadButton_ZR))
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
                    // Language cycle embedded in Name/Island str fields
                    if (fd.isStr) {
                        const char* lf = (strcmp(fd.fieldName,"Player.Name")==0)      ? "Player.NameRegionLanguageID"
                                       : (strcmp(fd.fieldName,"Player.IslandName")==0) ? "Player.IslandNameRegionLanguageID"
                                       : nullptr;
                        if (lf) {
                            bool isNameF = strcmp(fd.fieldName,"Player.Name")==0;
                            uint32_t lh = SaveEditor::Hash(lf);
                            uint32_t cur = SaveEditor::GetEnum(gPlayerSav, lh);
                            int lidx = 0;
                            for (int li=0; li<LANG_OPTION_COUNT; li++)
                                if (SaveEditor::Hash(LANG_OPTIONS[li].name)==cur) { lidx=li; break; }
                            auto cycleLang = [&](int dir) {
                                if (isNameF) { if (!gNameLangUndoValid)   { gNameLangUndo  =(int32_t)cur; gNameLangUndoValid  =true; } }
                                else         { if (!gIslandLangUndoValid) { gIslandLangUndo=(int32_t)cur; gIslandLangUndoValid=true; } }
                                int nidx = (lidx+dir+LANG_OPTION_COUNT)%LANG_OPTION_COUNT;
                                SaveEditor::SetEnum(gPlayerSav, lh, SaveEditor::Hash(LANG_OPTIONS[nidx].name));
                                gPlayerSavDirty=true;
                            };
                            if (kNav&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft))   cycleLang(-1);
                            if (kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight)) cycleLang(1);
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
                    // X = pronunciation for Name/Island; X = undo for non-string fields
                    if (fd.isStr && (kDown&HidNpadButton_X)) {
                        const char* howName = (strcmp(fd.fieldName,"Player.Name")==0)      ? "Player.HowToCallName"
                                            : (strcmp(fd.fieldName,"Player.IslandName")==0) ? "Player.HowToCallIslandName"
                                            : nullptr;
                        if (howName) {
                            uint32_t hh = SaveEditor::Hash(howName);
                            std::string cur = SaveEditor::GetWStr32(gPlayerSav, hh);
                            std::string nv = ShowKeyboard("Pronunciation (optional, empty = same as name)", cur, 30);
                            if (nv != cur) { SaveEditor::SetWStr32(gPlayerSav, hh, nv); gPlayerSavDirty=true; }
                            gPlayerMsg = ""; gPlayerMsgCol = COL_TEXT;
                        }
                    }
                    if (!fd.isStr && (kDown&HidNpadButton_X)) TUndoPlayer(0);
                }
                // B = back to user pick, Plus = quit app (both prompt when dirty)
                if (kDown&(HidNpadButton_B|HidNpadButton_Plus)) {
                    bool toUserPick = (kDown&HidNpadButton_B) != 0;
                    if (gPlayerSavDirty || gUgcDirty || gWebUiDirty) {
                        gShowBackPrompt=true; gBackPromptIsMii=false; gBackPromptToUserPick=toUserPick;
                    } else if (toUserPick) {
                        FreePreview(); HttpServer::Stop(); gLog.clear();
                        gEntries.clear(); gMiis.clear();
                        gPlayerSav=SaveEditor::SavFile{}; gMiiSav=SaveEditor::SavFile{};
                        gPlayerSavDirty=false; gMiiSavDirty=false;
                        SaveMount::Unmount();
                        gScreen=Screen::UserPick;
                    } else {
                        gQuitApp=true;
                    }
                }

            } else if (gOnSwitchMode == OnSwitchMode::MiiStats) {
                // Lazy-load
                if (!gMiiSav.loaded) {
                    std::string err;
                    if (!SaveEditor::Load(SAVE_MII_SAV, gMiiSav, err))
                        gMiiStatsMsg = "Load error: " + err;
                }
                // Tab switch (L/R for main tabs — changes stay in memory; prompt only on back/quit)
                if (kDown&HidNpadButton_L){
                    gOnSwitchMode=OnSwitchMode::UGC; gOnSwitchMsg=""; break;
                }
                if (kDown&HidNpadButton_R){
                    gOnSwitchMode=OnSwitchMode::Player;
                    if (!gPlayerSav.loaded) {
                        std::string err;
                        if (!SaveEditor::Load(SAVE_PLAYER_SAV, gPlayerSav, err))
                            gPlayerMsg="Load error: "+err;
                    }
                    gOnSwitchMsg=""; break;
                }
                // Y cycles Stats → Words → Relations → Social → Stats
                if (kDown&HidNpadButton_Y) { gMiiStatsSubTab=(MiiStatsSubTab)(((int)gMiiStatsSubTab+1)%MII_SUBTAB_COUNT); gSocialScroll=0; gBlPickerOpen=false; if(gMiiStatsSubTab!=MiiStatsSubTab::Social) gSocialExpanded=false; }
                // X toggles global graph in Social sub-tab only
                if (kDown&HidNpadButton_X && gMiiStatsSubTab==MiiStatsSubTab::Social) gSocialExpanded=!gSocialExpanded;
                // ZL/ZR switch selected mii
                if (!gMiis.empty()) {
                    int mseVis2=(SCREEN_H-SE_TOP_Y-32)/ITEM_H;
                    auto prevMii = [&](){
                        gMiiStatsMiiSel = (gMiiStatsMiiSel>0) ? gMiiStatsMiiSel-1 : (int)gMiis.size()-1;
                        if (gMiiStatsMiiSel < gMiiStatsScroll) gMiiStatsScroll=gMiiStatsMiiSel;
                        gWordUndo.valid = false; gMiiRelSel=0; gMiiRelScroll=0; gBlPickerOpen=false;
                    };
                    auto nextMii = [&](){
                        gMiiStatsMiiSel = (gMiiStatsMiiSel+1<(int)gMiis.size()) ? gMiiStatsMiiSel+1 : 0;
                        if (gMiiStatsMiiSel >= gMiiStatsScroll+mseVis2) gMiiStatsScroll=gMiiStatsMiiSel-mseVis2+1;
                        gWordUndo.valid = false; gMiiRelSel=0; gMiiRelScroll=0; gBlPickerOpen=false;
                    };
                    if (!gBlPickerOpen) {
                        if (kNav&HidNpadButton_ZL) prevMii();
                        if (kNav&HidNpadButton_ZR) nextMii();
                    }
                }
                // D-pad Up/Down = field/slot nav depending on sub-tab
                if (gMiiStatsSubTab==MiiStatsSubTab::Stats) {
                    if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp))
                        gMiiStatsFieldSel = (gMiiStatsFieldSel>0) ? gMiiStatsFieldSel-1 : MII_STATS_FIELD_COUNT-1;
                    if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown))
                        gMiiStatsFieldSel = (gMiiStatsFieldSel+1<MII_STATS_FIELD_COUNT) ? gMiiStatsFieldSel+1 : 0;
                } else if (gMiiStatsSubTab==MiiStatsSubTab::Words) {
                    if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp))
                        { gMiiWordsSlotSel = (gMiiWordsSlotSel>0) ? gMiiWordsSlotSel-1 : 11; gWordUndo.valid=false; }
                    if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown))
                        { gMiiWordsSlotSel = (gMiiWordsSlotSel<11) ? gMiiWordsSlotSel+1 : 0; gWordUndo.valid=false; }
                } else if (gMiiStatsSubTab==MiiStatsSubTab::Relations) {
                    const int REL_VIS = (SCREEN_H - 32 - (SE_TOP_Y + 2)) / 34;
                    if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp)) {
                        if (gMiiRelSel > 0) {
                            gMiiRelSel--;
                            if (gMiiRelSel < gMiiRelScroll) gMiiRelScroll = gMiiRelSel;
                        }
                    }
                    if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown)) {
                        if (gMiiRelSel+1 < gMiiRelCount) {
                            gMiiRelSel++;
                            if (gMiiRelSel >= gMiiRelScroll + REL_VIS) gMiiRelScroll = gMiiRelSel - REL_VIS + 1;
                        }
                    }
                    // gMiiRelSel additionally clamped to valid range in draw phase
                } else if (gMiiStatsSubTab==MiiStatsSubTab::Belongings) {
                    if (gBlPickerOpen) {
                        int total = (int)gBlFiltered.size();
                        const int pk_vis_nav = (SCREEN_H - (SE_TOP_Y + 32) - 32 - 32) / 30;
                        auto pkScrollTo = [&]() {
                            if (gBlPickerSel < gBlPickerScroll) gBlPickerScroll = gBlPickerSel;
                            else if (gBlPickerSel >= gBlPickerScroll + pk_vis_nav) gBlPickerScroll = gBlPickerSel - pk_vis_nav + 1;
                        };
                        if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp))
                            { gBlPickerSel = (gBlPickerSel > 0) ? gBlPickerSel - 1 : total - 1; pkScrollTo(); }
                        if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown))
                            { gBlPickerSel = (gBlPickerSel + 1 < total) ? gBlPickerSel + 1 : 0; pkScrollTo(); }
                        if (total > 0) {
                            if (kNav&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft))
                                { gBlPickerSel = std::max(0, gBlPickerSel - 12); pkScrollTo(); }
                            if (kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight))
                                { gBlPickerSel = std::min(total - 1, gBlPickerSel + 12); pkScrollTo(); }
                        }
                    } else {
                        const int bl_vis_nav = (SCREEN_H - (SE_TOP_Y + 32) - 32) / BL_ROW_H;
                        auto blScrollTo = [&]() {
                            int sv = BlSelToVis(gBlSel);
                            if (sv < gBlScroll) gBlScroll = sv;
                            else if (sv >= gBlScroll + bl_vis_nav) gBlScroll = sv - bl_vis_nav + 1;
                        };
                        if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp))
                            { gBlSel = (gBlSel > 0) ? gBlSel - 1 : BL_SEL_MAX - 1; blScrollTo(); }
                        if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown))
                            { gBlSel = (gBlSel + 1 < BL_SEL_MAX) ? gBlSel + 1 : 0; blScrollTo(); }
                    }
                } else if (gMiiStatsSubTab==MiiStatsSubTab::Habits) {
                    int catSize = 0;
                    for (int i = 0; i < HABIT_COUNT; i++) if (HABITS[i].category == gHabitCatSel) catSize++;
                    if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp))
                        gHabitItemSel = (gHabitItemSel > 0) ? gHabitItemSel-1 : std::max(0, catSize-1);
                    if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown))
                        gHabitItemSel = (gHabitItemSel+1 < catSize) ? gHabitItemSel+1 : 0;
                    if (kNav&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft))
                        { gHabitCatSel = (gHabitCatSel>0) ? gHabitCatSel-1 : HABIT_CAT_COUNT-1; gHabitItemSel=0; gHabitScroll=0; }
                    if (kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight))
                        { gHabitCatSel = (gHabitCatSel+1<HABIT_CAT_COUNT) ? gHabitCatSel+1 : 0; gHabitItemSel=0; gHabitScroll=0; }
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
                    int wIdx = miiIdx * 12 + gMiiWordsSlotSel;
                    uint32_t curKind = SaveEditor::GetAnyEnumAt(gMiiSav, H_WKIND_I, wIdx, H_WINV_I);
                    int kIdx = WordKindIndex(curKind);
                    if (kIdx < 0) kIdx = 0;
                    auto captureWordUndo = [&]() {
                        if (!gWordUndo.valid) {
                            gWordUndo.kind  = curKind;
                            gWordUndo.text  = SaveEditor::GetWStr64At(gMiiSav, H_WTXT_I, wIdx);
                            gWordUndo.how   = SaveEditor::GetWStr64At(gMiiSav, H_WHOW_I, wIdx);
                            gWordUndo.valid = true;
                        }
                    };
                    // Left/Right: cycle kind
                    auto cycleKind = [&](int dir) {
                        captureWordUndo();
                        int nk = (kIdx + dir + WORD_KIND_COUNT) % WORD_KIND_COUNT;
                        uint32_t nHash = SaveEditor::Hash(WORD_KIND_LIST[nk].name);
                        SaveEditor::SetAnyEnumAt(gMiiSav, H_WKIND_I, wIdx, nHash);
                        if (curKind == H_WINV_I && nk != 0) {
                            // activating from empty: auto-set region to USen
                            SaveEditor::SetAnyEnumAt(gMiiSav, H_WREGION_I, wIdx, H_WUSEN_I);
                        }
                        gMiiSavDirty = true;
                    };
                    if (kDown&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft))   cycleKind(-1);
                    if (kDown&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight)) cycleKind(+1);
                    // A: edit text
                    if (kDown&HidNpadButton_A) {
                        std::string cur = SaveEditor::GetWStr64At(gMiiSav, H_WTXT_I, wIdx);
                        std::string nv = ShowKeyboard("Word Text (max 63 chars)", cur, 63);
                        if (nv != cur) {
                            captureWordUndo();
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
                            captureWordUndo();
                            SaveEditor::SetWStr64At(gMiiSav, H_WHOW_I, wIdx, nv);
                            gMiiSavDirty = true;
                        }
                        gMiiStatsMsg=""; gMiiStatsMsgCol=COL_TEXT;
                    }
                    // Minus: undo
                    if (kDown&HidNpadButton_Minus) TUndoWords(0);
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
                        if (kNav&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft))   cycleType(-1);
                        if (kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight)) cycleType(+1);
                    }
                    if (gRelDateKbdReq) {
                        gRelDateKbdReq = false;
                        static const uint32_t H_TST_KBD = 0x1a892e50u;
                        uint64_t cur = SaveEditor::GetUInt64At(gMiiSav, H_TST_KBD, gRelDatePairIdx);
                        char initBuf[11] = "";
                        if (cur > 0) {
                            time_t t = (time_t)cur;
                            struct tm* ti = localtime(&t);
                            snprintf(initBuf, sizeof(initBuf), "%04d-%02d-%02d",
                                     ti->tm_year+1900, ti->tm_mon+1, ti->tm_mday);
                        }
                        std::string raw = ShowKeyboard("Since (YYYY-MM-DD)", initBuf, 10);
                        int y=0, mo=0, d=0;
                        if (sscanf(raw.c_str(), "%d-%d-%d", &y, &mo, &d)==3
                            && y>1900 && mo>=1 && mo<=12 && d>=1 && d<=31) {
                            struct tm t = {};
                            t.tm_year = y-1900; t.tm_mon = mo-1; t.tm_mday = d; t.tm_hour = 12;
                            time_t secs = mktime(&t);
                            if (secs >= 0) {
                                SaveEditor::SetUInt64At(gMiiSav, H_TST_KBD, gRelDatePairIdx, (uint64_t)secs);
                                gMiiSavDirty = true;
                            }
                        }
                    }
                }
                // Edit (Belongings sub-tab)
                if (gMiiStatsSubTab==MiiStatsSubTab::Belongings && gMiiSav.loaded && !gMiis.empty() && gMiiStatsMiiSel<(int)gMiis.size()) {
                    int miiIdx = gMiis[gMiiStatsMiiSel].slot - 1;
                    int blSel  = gBlSel;

                    // Helper: cycle color for worn slot (Left/Right)
                    auto cycleColor = [&](int dir) {
                        if (blSel < 8) {
                            const auto& ws = BL_WORN_DEFS[blSel];
                            uint32_t ih = SaveEditor::GetAnyEnumAt(gMiiSav, ws.kh, miiIdx, BL_H_INVALID);
                            if (!ih || ih == BL_H_INVALID) return;
                            int cc = 1;
                            for (int k=0;k<BL_CLOTH_COUNT;k++) if(BL_CLOTH_ITEMS[k].nameHash==ih){cc=BL_CLOTH_ITEMS[k].colorCount;break;}
                            int cur = SaveEditor::GetIntAt(gMiiSav, ws.ch, miiIdx);
                            SaveEditor::SetIntAt(gMiiSav, ws.ch, miiIdx, ((cur+dir)%cc+cc)%cc);
                            gMiiSavDirty = true;
                        } else if (blSel == 8) {
                            uint32_t ih = SaveEditor::GetAnyEnumAt(gMiiSav, BL_COORD_KH, miiIdx, BL_H_INVALID);
                            if (!ih || ih == BL_H_INVALID) return;
                            int cc = 1;
                            for (int k=0;k<BL_COORD_COUNT;k++) if(BL_COORD_ITEMS[k].keyHash==ih){cc=BL_COORD_ITEMS[k].colorCount;break;}
                            int cur = SaveEditor::GetIntAt(gMiiSav, BL_COORD_CH, miiIdx);
                            SaveEditor::SetIntAt(gMiiSav, BL_COORD_CH, miiIdx, ((cur+dir)%cc+cc)%cc);
                            gMiiSavDirty = true;
                        }
                    };

                    // Helper: apply selected item from picker
                    auto applyPickerItem = [&](uint32_t newHash) {
                        if (blSel < 8) {
                            const auto& ws = BL_WORN_DEFS[blSel];
                            uint32_t old = SaveEditor::GetAnyEnumAt(gMiiSav, ws.kh, miiIdx, BL_H_INVALID);
                            SaveEditor::SetAnyEnumAt(gMiiSav, ws.kh, miiIdx, newHash);
                            if (newHash != old) SaveEditor::SetIntAt(gMiiSav, ws.ch, miiIdx, 0);
                            gMiiSavDirty = true;
                        } else if (blSel == 8) {
                            uint32_t old = SaveEditor::GetAnyEnumAt(gMiiSav, BL_COORD_KH, miiIdx, BL_H_INVALID);
                            SaveEditor::SetAnyEnumAt(gMiiSav, BL_COORD_KH, miiIdx, newHash);
                            if (newHash != old) SaveEditor::SetIntAt(gMiiSav, BL_COORD_CH, miiIdx, 0);
                            gMiiSavDirty = true;
                        } else if (blSel >= 9 && blSel < 21) {
                            int ai = miiIdx * BL_GOODS_SLOTS_MII + (blSel - 9);
                            if (newHash == 0u) {
                                SaveEditor::SetUIntAt(gMiiSav, BL_H_GOODS_ID, ai, 0u);
                                SaveEditor::SetUInt64At(gMiiSav, BL_H_GOODS_TIME, ai, 0ull);
                                SaveEditor::SetIntAt(gMiiSav, BL_H_GOODS_UGC, ai, -1);
                            } else {
                                SaveEditor::SetUIntAt(gMiiSav, BL_H_GOODS_ID, ai, newHash);
                                uint64_t t = SaveEditor::GetUInt64At(gMiiSav, BL_H_GOODS_TIME, ai);
                                if (!t) SaveEditor::SetUInt64At(gMiiSav, BL_H_GOODS_TIME, ai, 1735689600ull);
                                SaveEditor::SetIntAt(gMiiSav, BL_H_GOODS_UGC, ai, -1);
                            }
                            gMiiSavDirty = true;
                        }
                    };

                    if (gBlPickerOpen) {
                        // A = confirm, B = cancel, Y = search, X = clear filter
                        if (kDown&HidNpadButton_A && !gBlFiltered.empty()) {
                            int realIdx = gBlFiltered[std::max(0, std::min(gBlPickerSel, (int)gBlFiltered.size()-1))];
                            applyPickerItem(gBlPickerHashes[realIdx]);
                            gBlPickerOpen = false;
                        }
                        if (kDown&HidNpadButton_Minus) {
                            std::string q = ShowKeyboard("Filter items (substring, case-insensitive)", gBlFilter, 30);
                            // lowercase
                            for (auto& c : q) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
                            gBlFilter = q;
                            BlRebuildFilter();
                            gBlPickerSel = 0;
                            gBlPickerScroll = 0;
                        }
                        if (kDown&HidNpadButton_X && !gBlFilter.empty()) {
                            gBlFilter.clear();
                            BlRebuildFilter();
                            gBlPickerSel = 0;
                            gBlPickerScroll = 0;
                        }
                    } else {
                        // A = open item picker
                        if (kDown&HidNpadButton_A && blSel < 21)
                            BlOpenPicker(blSel, miiIdx);
                        if (kNav&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft))   cycleColor(-1);
                        if (kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight)) cycleColor(+1);
                    }

                    // X = clear selected slot (only when picker is closed)
                    if (!gBlPickerOpen && kDown&HidNpadButton_X) {
                        if (blSel < 8) {
                            const auto& ws = BL_WORN_DEFS[blSel];
                            SaveEditor::SetAnyEnumAt(gMiiSav, ws.kh, miiIdx, BL_H_INVALID);
                            SaveEditor::SetIntAt(gMiiSav, ws.ch, miiIdx, 0);
                            gMiiSavDirty = true;
                        } else if (blSel == 8) {
                            SaveEditor::SetAnyEnumAt(gMiiSav, BL_COORD_KH, miiIdx, BL_H_INVALID);
                            SaveEditor::SetIntAt(gMiiSav, BL_COORD_CH, miiIdx, 0);
                            gMiiSavDirty = true;
                        } else if (blSel >= 9 && blSel < 21) {
                            int ai = miiIdx * BL_GOODS_SLOTS_MII + (blSel - 9);
                            SaveEditor::SetUIntAt(gMiiSav, BL_H_GOODS_ID, ai, 0u);
                            SaveEditor::SetUInt64At(gMiiSav, BL_H_GOODS_TIME, ai, 0ull);
                            SaveEditor::SetIntAt(gMiiSav, BL_H_GOODS_UGC, ai, -1);
                            gMiiSavDirty = true;
                        }
                    }

                    // Action rows (A to execute, only when picker is closed)
                    if (!gBlPickerOpen && kDown&HidNpadButton_A && blSel >= 21) {
                        int act = blSel - 21;
                        int total;
                        if (act == 0) { // Unlock All Clothes
                            total = SaveEditor::ArraySize(gMiiSav, BL_H_CLOTH_OWN);
                            for (int i=0;i<BL_CLOTH_COUNT;i++) {
                                int idx = miiIdx * BL_SLOTS_PER_MII + BL_CLOTH_ITEMS[i].index;
                                if (idx < total) {
                                    int cc = BL_CLOTH_ITEMS[i].colorCount;
                                    uint32_t mask = (cc>=32) ? 0xFFFFFFFFu : ((1u<<cc)-1u);
                                    SaveEditor::SetUIntAt(gMiiSav, BL_H_CLOTH_OWN, idx, mask);
                                }
                            }
                            gMiiSavDirty = true;
                            gMiiStatsMsg = "All clothes unlocked"; gMiiStatsMsgCol = COL_GREEN;
                        } else if (act == 1) { // Lock All Clothes
                            total = SaveEditor::ArraySize(gMiiSav, BL_H_CLOTH_OWN);
                            for (int i=0;i<BL_SLOTS_PER_MII;i++) {
                                int idx = miiIdx * BL_SLOTS_PER_MII + i;
                                if (idx < total) SaveEditor::SetUIntAt(gMiiSav, BL_H_CLOTH_OWN, idx, 0u);
                            }
                            gMiiSavDirty = true;
                            gMiiStatsMsg = "All clothes locked"; gMiiStatsMsgCol = COL_GREEN;
                        } else if (act == 2) { // Unlock All Coords
                            total = SaveEditor::ArraySize(gMiiSav, BL_H_COORD_OWN);
                            for (int i=0;i<BL_COORD_COUNT;i++) {
                                int idx = miiIdx * BL_COORD_SLOTS_MII + BL_COORD_ITEMS[i].saveIndex;
                                if (idx < total) {
                                    int cc = BL_COORD_ITEMS[i].colorCount;
                                    uint32_t mask = (cc>=32) ? 0xFFFFFFFFu : ((1u<<cc)-1u);
                                    SaveEditor::SetUIntAt(gMiiSav, BL_H_COORD_OWN, idx, mask);
                                }
                            }
                            gMiiSavDirty = true;
                            gMiiStatsMsg = "All coordinates unlocked"; gMiiStatsMsgCol = COL_GREEN;
                        } else { // Lock All Coords
                            total = SaveEditor::ArraySize(gMiiSav, BL_H_COORD_OWN);
                            for (int i=0;i<BL_COORD_SLOTS_MII;i++) {
                                int idx = miiIdx * BL_COORD_SLOTS_MII + i;
                                if (idx < total) SaveEditor::SetUIntAt(gMiiSav, BL_H_COORD_OWN, idx, 0u);
                            }
                            gMiiSavDirty = true;
                            gMiiStatsMsg = "All coordinates locked"; gMiiStatsMsgCol = COL_GREEN;
                        }
                    }
                }
                // Edit (Habits sub-tab)
                if (gMiiStatsSubTab==MiiStatsSubTab::Habits && gMiiSav.loaded && !gMiis.empty() && gMiiStatsMiiSel<(int)gMiis.size()) {
                    EnsureHabitHashes();
                    int miiIdx = gMiis[gMiiStatsMiiSel].slot - 1;
                    std::vector<int> items = HabitsInCategory(gHabitCatSel);
                    if (gHabitItemSel >= (int)items.size()) gHabitItemSel = std::max(0,(int)items.size()-1);

                    // A: toggle active habit (radio: ensure exclusive within category)
                    if (kDown&HidNpadButton_A && !items.empty()) {
                        int chosen = items[gHabitItemSel];
                        for (int idx : items) {
                            bool active = (idx == chosen);
                            SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[idx].isChecked, miiIdx, active);
                            if (active) {
                                SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[idx].isOwn, miiIdx, true);
                                SaveEditor::SetEnumAt(gMiiSav, gHabitHashes[idx].state, miiIdx, gHabitStateOwnH);
                            }
                        }
                        gMiiSavDirty = true;
                        gMiiStatsMsg = std::string("Active: ") + HABITS[chosen].label;
                        gMiiStatsMsgCol = COL_GREEN;
                        gMiiStatsMsgFrames = 120; // ~2s at 60fps
                    }

                    // X: toggle owned (without affecting active selection unless un-owning the active one)
                    if (kDown&HidNpadButton_X && !items.empty()) {
                        int chosen = items[gHabitItemSel];
                        bool isOwn = SaveEditor::GetBoolAt(gMiiSav, gHabitHashes[chosen].isOwn, miiIdx, false);
                        if (isOwn) {
                            uint32_t st = SaveEditor::GetEnumAt(gMiiSav, gHabitHashes[chosen].state, miiIdx, gHabitStateNeverOwnedH);
                            SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[chosen].isOwn, miiIdx, false);
                            SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[chosen].isChecked, miiIdx, false);
                            if (st == gHabitStateOwnH)
                                SaveEditor::SetEnumAt(gMiiSav, gHabitHashes[chosen].state, miiIdx, gHabitStateUnownH);
                        } else {
                            SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[chosen].isOwn, miiIdx, true);
                            SaveEditor::SetEnumAt(gMiiSav, gHabitHashes[chosen].state, miiIdx, gHabitStateOwnH);
                        }
                        gMiiSavDirty = true;
                        gMiiStatsMsg = std::string(isOwn?"Removed: ":"Added: ") + HABITS[chosen].label;
                        gMiiStatsMsgCol = COL_GREEN;
                        gMiiStatsMsgFrames = 120;
                    }

                    // Minus: clear category (all habits in this category → NeverOwned)
                    if (kDown&HidNpadButton_Minus && !items.empty()) {
                        for (int idx : items) {
                            SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[idx].isChecked, miiIdx, false);
                            SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[idx].isOwn, miiIdx, false);
                            SaveEditor::SetEnumAt(gMiiSav, gHabitHashes[idx].state, miiIdx, gHabitStateNeverOwnedH);
                        }
                        gMiiSavDirty = true;
                        gMiiStatsMsg = std::string("Cleared: ") + HABIT_CAT_LABEL[gHabitCatSel];
                        gMiiStatsMsgCol = COL_GREEN;
                        gMiiStatsMsgFrames = 120;
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
                            if (kDown&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft)) {
                                if (!gMiiUndoValid[gMiiStatsFieldSel]) { gMiiUndoVal[gMiiStatsFieldSel]=(int32_t)cur; gMiiUndoValid[gMiiStatsFieldSel]=true; }
                                curIdx = (curIdx>0) ? curIdx-1 : FEELING_LABEL_COUNT-1;
                                SaveEditor::SetEnumAt(gMiiSav, fh, miiIdx, FEELING_LABELS[curIdx].hash);
                                gMiiSavDirty=true;
                            }
                            if (kDown&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight)) {
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
                                // Left/Right (D-pad or stick): navigate button focus
                                if (kNav&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft))   gMiiBtnSel = (gMiiBtnSel-1+6)%6;
                                if (kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight)) gMiiBtnSel = (gMiiBtnSel+1)%6;
                                // A fires focused button
                                if (kDown&HidNpadButton_A && !fd.isStr) {
                                    static const int MSTEPS[]={100,10,1};
                                    int mdelta = (gMiiBtnSel<3) ? -MSTEPS[gMiiBtnSel] : MSTEPS[5-gMiiBtnSel];
                                    if (fd.isUInt) adjustUInt(mdelta); else adjustInt(mdelta);
                                }
                            }
                        }
                    }
                    if (fd.isStr && gMiiNameKbdReq) {
                        gMiiNameKbdReq = false;
                        uint32_t h = SaveEditor::Hash(fd.fieldName);
                        std::string cur = SaveEditor::GetWStr32At(gMiiSav, h, miiIdx);
                        std::string nv = ShowKeyboard(fd.label, cur, 30);
                        if (nv != cur) { SaveEditor::SetWStr32At(gMiiSav, h, miiIdx, nv); gMiiSavDirty=true; }
                        gMiiStatsMsg=""; gMiiStatsMsgCol=COL_TEXT;
                    }
                    if (fd.isStr && (kDown&HidNpadButton_X)) {
                        static const uint32_t H_HOW_N = SaveEditor::Hash("Mii.Name.HowToCallName");
                        std::string cur = SaveEditor::GetWStr64At(gMiiSav, H_HOW_N, miiIdx);
                        std::string nv = ShowKeyboard("Pronunciation (optional, empty = same as name)", cur, 63);
                        if (nv != cur) { SaveEditor::SetWStr64At(gMiiSav, H_HOW_N, miiIdx, nv); gMiiSavDirty=true; }
                        gMiiStatsMsg=""; gMiiStatsMsgCol=COL_TEXT;
                    }
                    if (fd.isStr && !gMiis.empty() && gMiiStatsMiiSel<(int)gMiis.size()) {
                        if (kDown&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft))
                            gMiiLtdBtnSel = 0;
                        if (kDown&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight))
                            gMiiLtdBtnSel = 1;
                        if (kDown&HidNpadButton_A) {
                            if (gMiiLtdBtnSel == 0) TOpenImportBrowser(0);
                            else TDoMiiExport(0);
                        }
                    }
                }
                // B = back to user pick, Plus = quit app (both prompt when dirty)
                if (kDown&(HidNpadButton_B|HidNpadButton_Plus)) {
                    if (gBlPickerOpen && (kDown&HidNpadButton_B)) {
                        gBlPickerOpen = false; // B closes picker, does not navigate back
                    } else {
                        bool toUserPick = (kDown&HidNpadButton_B) != 0;
                        if (gMiiSavDirty || gUgcDirty || gWebUiDirty) {
                            gShowBackPrompt=true; gBackPromptIsMii=true; gBackPromptToUserPick=toUserPick;
                        } else if (toUserPick) {
                            FreePreview(); HttpServer::Stop(); gLog.clear();
                            gEntries.clear(); gMiis.clear();
                            gPlayerSav=SaveEditor::SavFile{}; gMiiSav=SaveEditor::SavFile{};
                            gPlayerSavDirty=false; gMiiSavDirty=false;
                            SaveMount::Unmount();
                            gScreen=Screen::UserPick;
                        } else {
                            gQuitApp=true;
                        }
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
                if (kDown&HidNpadButton_L){
                    if (!gSaveWarningAcked) {
                        gSaveWarningTarget=OnSwitchMode::Player; gShowSaveWarning=true; gSaveWarningCountdown=120; break;
                    }
                    gOnSwitchMode=OnSwitchMode::Player; gOnSwitchMsg="";
                    if (!gPlayerSav.loaded) {
                        std::string err;
                        if (!SaveEditor::Load(SAVE_PLAYER_SAV, gPlayerSav, err))
                            gPlayerMsg="Load error: "+err;
                    }
                    break;
                }
                if (kDown&HidNpadButton_R){
                    gOnSwitchMode=OnSwitchMode::UGC; gOnSwitchMsg=""; break;
                }
                if (kDown&(HidNpadButton_B|HidNpadButton_Plus)){
                    bool hasDirty = gPlayerSavDirty || gMiiSavDirty || gUgcDirty || gWebUiDirty;
                    bool toUserPick = (kDown&HidNpadButton_Plus) == 0;
                    if (hasDirty) {
                        gShowBackPrompt=true;
                        gBackPromptIsMii=gMiiSavDirty; gBackPromptToUserPick=toUserPick;
                    } else if (toUserPick) {
                        FreePreview(); HttpServer::Stop(); gLog.clear();
                        gEntries.clear(); gMiis.clear();
                        gPlayerSav=SaveEditor::SavFile{}; gMiiSav=SaveEditor::SavFile{};
                        gPlayerSavDirty=false; gMiiSavDirty=false;
                        gScreen=Screen::UserPick; SaveMount::Unmount();
                    } else { gQuitApp=true; }
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
    romfsExit();
    return 0;
}
