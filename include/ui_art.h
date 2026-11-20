#pragma once
// ui_art.h — TomoToolNX
// The game's own art (romfs:/gfx/, built by tools/assets/build_gfx.py): line
// icons, glossy illustrations, Mii faces, hands, shop logos and the tiling
// patterns used as wallpapers. Everything loads on first use and is drawn
// through ui_draw (so it fades, slides and scales with the rest). A missing
// file is not an error: Get() returns nullptr and callers draw their own.

#include <SDL2/SDL.h>
#include <cstdint>

namespace Art {

enum class Id : uint16_t {
    Logo, LogoShare,
    // Wallpapers (white on clear, tinted)
    PatFace, PatItemBook, PatBuilding, PatClothes, PatCoordinate, PatFood, PatGoods, PatGround,
    PatHouse, PatIslandEdit, PatPhoto, PatQuiz,
    // Illustrations
    ArtAtelier, ArtQuikBuild, ArtFloor, ArtReno, ArtGoods, ArtMii, ArtTravel,
    KindClothes, KindExterior, KindFloor, KindGoods, KindInterior, KindObj, KindFood,
    // Line icons (white masks, tinted)
    IcoHome, IcoMiis, IcoTextures, IcoPlayer, IcoMap, IcoBackups, IcoPhone, IcoSettings,
    IcoFace, IcoWords, IcoItems, IcoHabits, IcoSocial, IcoBirthday, IcoName, IcoPersonality, IcoVoice,
    IcoLook, IcoLanguage, IcoPictures, IcoAbout, IcoTrash, IcoSearch, IcoUndo, IcoSparkle, IcoPen,
    IcoGrid, IcoFullscreen, IcoSelect, IcoBucket, IcoZoom, IcoRotate, IcoMove, IcoIsland, IcoCamera,
    // Mii faces (centred on the round head, its radius 0.38 of the width), and
    // where each one's hair is (a mask, tinted with the Mii's hair colour)
    FaceBoy, FaceGirl, FaceMan, FaceWoman, FaceOld, FaceKid,
    FaceBoyHair, FaceGirlHair, FaceManHair, FaceWomanHair, FaceOldHair, FaceKidHair,
    // Hands, one per skin tone (the six of the Player page)
    HandPoint0, HandPoint1, HandPoint2, HandPoint3, HandPoint4, HandPoint5,
    HandOpen0, HandOpen1, HandOpen2, HandOpen3, HandOpen4, HandOpen5,
    HandClose0, HandClose1, HandClose2, HandClose3, HandClose4, HandClose5,
    // Logos and backdrops
    ShopFood, ShopClothes, ShopGoods, ShopReno, ShopQuikBuild, ShopAtelier, NewsLogo, NewsBg,
    // Little flags for the languages (tools/assets/make_flags.py)
    FlagEn, FlagFr, FlagEs, FlagDe, FlagRu, FlagJa, FlagZh,
    Count
};

// The texture (loaded on first use), and its size. nullptr when the file is
// not in the build.
SDL_Texture* Get(Id id, int* w = nullptr, int* h = nullptr);
bool Has(Id id);
// A white mask that takes a colour (patterns, line icons, white logos), as
// opposed to a picture drawn as it is.
bool IsMask(Id id);
// Drawn to fit inside `box`, keeping its shape, centred. White masks take
// `tint`; illustrations are drawn as they are (tint white).
void Draw(Id id, SDL_Rect box, SDL_Color tint = SDL_Color{ 255, 255, 255, 255 });
// A line icon centred on (cx, cy) at about `size` px, in colour `c`. Returns
// false when the icon is missing (the caller draws its own).
bool Icon(Id id, int cx, int cy, int size, SDL_Color c);
// One of the six hands, for a skin tone 0..5.
Id HandPoint(int tone);
Id HandOpen(int tone);
Id HandClose(int tone);
// The flag for a language code ("en", "fr", ...); Id::Count when there is none.
Id Flag(const char* lang);
// A Mii item's picture, romfs:/items/<asset>.png (tools/assets/build_items.py),
// loaded on first use and kept while it is used; nullptr when there is none.
SDL_Texture* ItemPicture(const char* asset, int* w = nullptr, int* h = nullptr);
// Drawn to fit inside `box`, keeping its shape, centred.
void DrawFit(SDL_Texture* t, SDL_Rect box, uint8_t alpha = 255);
// The skin tone the Player page has chosen (0..5); the pages set it.
void SetSkinTone(int tone);
int  SkinTone();

// The wallpaper: a tiling pattern drifting slowly across `area`, in the
// theme's colour. A page asks for its pattern every frame; a change fades
// over. Id::Count means none.
void SetWallpaper(Id pattern);
void DrawWallpaper(SDL_Rect area);

void Purge();   // free every texture (on exit)

}  // namespace Art
