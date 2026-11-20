// ui_art.cpp — TomoToolNX. See ui_art.h.
#include "ui_art.h"
#include "ui_draw.h"
#include "ui_motion.h"

#include <SDL2/SDL_image.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace Art {
namespace {

const char* const kFiles[] = {
    "logo.png", "logo_tomodachishare.png",
    "pat_face.png", "pat_itembook.png", "pat_building.png", "pat_clothes.png", "pat_coordinate.png", "pat_food.png",
    "pat_goods.png", "pat_ground.png", "pat_house.png", "pat_islandedit.png", "pat_photo.png", "pat_quiz.png",
    "art_atelier.png", "art_quikbuild.png", "art_floor.png", "art_reno.png", "art_goods.png", "art_mii.png", "art_travel.png",
    "kind_clothes.png", "kind_exterior.png", "kind_floor.png", "kind_goods.png", "kind_interior.png", "kind_obj.png", "kind_food.png",
    "ico_home.png", "ico_miis.png", "ico_textures.png", "ico_player.png", "ico_map.png", "ico_backups.png", "ico_phone.png",
    "ico_settings.png", "ico_face.png", "ico_words.png", "ico_items.png", "ico_habits.png", "ico_social.png", "ico_birthday.png",
    "ico_name.png", "ico_personality.png", "ico_voice.png", "ico_look.png", "ico_language.png", "ico_pictures.png", "ico_about.png",
    "ico_trash.png", "ico_search.png", "ico_undo.png", "ico_sparkle.png", "ico_pen.png", "ico_grid.png", "ico_fullscreen.png",
    "ico_select.png", "ico_bucket.png", "ico_zoom.png", "ico_rotate.png", "ico_move.png", "ico_island.png", "ico_camera.png",
    "face_boy.png", "face_girl.png", "face_man.png", "face_woman.png", "face_old.png", "face_kid.png",
    "face_boy_hair.png", "face_girl_hair.png", "face_man_hair.png", "face_woman_hair.png", "face_old_hair.png", "face_kid_hair.png",
    "hand_point_0.png", "hand_point_1.png", "hand_point_2.png", "hand_point_3.png", "hand_point_4.png", "hand_point_5.png",
    "hand_open_0.png", "hand_open_1.png", "hand_open_2.png", "hand_open_3.png", "hand_open_4.png", "hand_open_5.png",
    "hand_close_0.png", "hand_close_1.png", "hand_close_2.png", "hand_close_3.png", "hand_close_4.png", "hand_close_5.png",
    "shop_food.png", "shop_clothes.png", "shop_goods.png", "shop_reno.png", "shop_quikbuild.png", "shop_atelier.png",
    "news_logo.png", "news_bg.jpg",
    "flag_en.png", "flag_fr.png", "flag_es.png", "flag_de.png", "flag_ru.png", "flag_ja.png", "flag_zh.png",
};
static_assert(sizeof(kFiles) / sizeof(kFiles[0]) == (size_t)Id::Count, "one file per Art::Id");

struct Slot { SDL_Texture* tex = nullptr; int w = 0, h = 0; bool tried = false; };
Slot sSlots[(int)Id::Count];
int sTone = 0;

// Wallpaper: the pattern shown, and the one fading out.
Id sWant = Id::Count, sCur = Id::Count, sPrev = Id::Count;
double sChangedAt = -10;

}  // namespace

SDL_Texture* Get(Id id, int* w, int* h) {
    const int i = (int)id;
    if (i < 0 || i >= (int)Id::Count) return nullptr;
    Slot& s = sSlots[i];
    if (!s.tried) {
        s.tried = true;
        const std::string path = std::string("romfs:/gfx/") + kFiles[i];
        if (SDL_Surface* surf = IMG_Load(path.c_str())) {
            s.tex = SDL_CreateTextureFromSurface(gRen, surf);
            if (s.tex) {
                s.w = surf->w; s.h = surf->h;
                SDL_SetTextureBlendMode(s.tex, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(s.tex, SDL_ScaleModeLinear);
            }
            SDL_FreeSurface(surf);
        }
    }
    if (w) *w = s.w;
    if (h) *h = s.h;
    return s.tex;
}

bool Has(Id id) { return Get(id) != nullptr; }

bool IsMask(Id id) {
    const int i = (int)id;
    return (i >= (int)Id::PatFace && i <= (int)Id::PatQuiz) || (i >= (int)Id::IcoHome && i <= (int)Id::IcoCamera) ||
           (i >= (int)Id::FaceBoyHair && i <= (int)Id::FaceKidHair) ||
           id == Id::ShopClothes || id == Id::ShopReno || id == Id::ShopAtelier || id == Id::NewsLogo;
}

void Draw(Id id, SDL_Rect box, SDL_Color tint) {
    int w = 0, h = 0;
    SDL_Texture* t = Get(id, &w, &h);
    if (!t || w <= 0 || h <= 0 || box.w <= 0 || box.h <= 0) return;
    const float k = std::min((float)box.w / w, (float)box.h / h);
    const int dw = (int)lroundf(w * k), dh = (int)lroundf(h * k);
    SDL_SetTextureColorMod(t, tint.r, tint.g, tint.b);
    DrawTexture(t, nullptr, SDL_Rect{ box.x + (box.w - dw) / 2, box.y + (box.h - dh) / 2, dw, dh }, tint.a);
    SDL_SetTextureColorMod(t, 255, 255, 255);
}

bool Icon(Id id, int cx, int cy, int size, SDL_Color c) {
    int w = 0, h = 0;
    SDL_Texture* t = Get(id, &w, &h);
    if (!t || w <= 0 || h <= 0) return false;
    // The game's icons fill their box; the drawn ones leave a margin, so these
    // are drawn a little smaller to sit at the same weight.
    const float k = size * 0.86f / (float)std::max(w, h);
    const int dw = std::max(1, (int)lroundf(w * k)), dh = std::max(1, (int)lroundf(h * k));
    SDL_SetTextureColorMod(t, c.r, c.g, c.b);
    DrawTexture(t, nullptr, SDL_Rect{ cx - dw / 2, cy - dh / 2, dw, dh }, c.a);
    SDL_SetTextureColorMod(t, 255, 255, 255);
    return true;
}

namespace {
struct ItemPic { SDL_Texture* tex = nullptr; int w = 0, h = 0; double used = 0; };
std::unordered_map<std::string, ItemPic> sItemPics;
}  // namespace

SDL_Texture* ItemPicture(const char* asset, int* w, int* h) {
    if (w) *w = 0;
    if (h) *h = 0;
    if (!asset || !*asset) return nullptr;
    auto it = sItemPics.find(asset);
    if (it == sItemPics.end()) {
        // A long list keeps loading as it scrolls: forget the pictures not
        // drawn for a while once there are many.
        if (sItemPics.size() >= 220) {
            const double now = Motion::Now();
            for (auto i = sItemPics.begin(); i != sItemPics.end();) {
                if (now - i->second.used > 2.0) { if (i->second.tex) SDL_DestroyTexture(i->second.tex); i = sItemPics.erase(i); }
                else ++i;
            }
        }
        ItemPic p;
        const std::string path = std::string("romfs:/items/") + asset + ".png";
        if (SDL_Surface* surf = IMG_Load(path.c_str())) {
            p.tex = SDL_CreateTextureFromSurface(gRen, surf);
            if (p.tex) {
                p.w = surf->w; p.h = surf->h;
                SDL_SetTextureBlendMode(p.tex, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(p.tex, SDL_ScaleModeLinear);
            }
            SDL_FreeSurface(surf);
        }
        it = sItemPics.emplace(asset, p).first;
    }
    it->second.used = Motion::Now();
    if (w) *w = it->second.w;
    if (h) *h = it->second.h;
    return it->second.tex;
}

void DrawFit(SDL_Texture* t, SDL_Rect box, uint8_t alpha) {
    int w = 0, h = 0;
    if (!t || SDL_QueryTexture(t, nullptr, nullptr, &w, &h) != 0 || w <= 0 || h <= 0 || box.w <= 0 || box.h <= 0) return;
    const float k = std::min((float)box.w / w, (float)box.h / h);
    const int dw = (int)lroundf(w * k), dh = (int)lroundf(h * k);
    DrawTexture(t, nullptr, SDL_Rect{ box.x + (box.w - dw) / 2, box.y + (box.h - dh) / 2, dw, dh }, alpha);
}

static int ClampTone(int t) { return t < 0 ? 0 : (t > 5 ? 5 : t); }
Id HandPoint(int tone) { return (Id)((int)Id::HandPoint0 + ClampTone(tone)); }
Id HandOpen(int tone)  { return (Id)((int)Id::HandOpen0 + ClampTone(tone)); }
Id HandClose(int tone) { return (Id)((int)Id::HandClose0 + ClampTone(tone)); }
Id Flag(const char* lang) {
    static const char* const kCodes[] = { "en", "fr", "es", "de", "ru", "ja", "zh" };
    if (!lang) return Id::Count;
    for (int i = 0; i < 7; i++)
        if (lang[0] == kCodes[i][0] && lang[1] == kCodes[i][1]) return (Id)((int)Id::FlagEn + i);
    return Id::Count;
}
void SetSkinTone(int tone) { sTone = ClampTone(tone); }
int  SkinTone() { return sTone; }

void SetWallpaper(Id pattern) { sWant = pattern; }

void DrawWallpaper(SDL_Rect area) {
    const double now = Motion::Now();
    if (sWant != sCur) { sPrev = sCur; sCur = sWant; sChangedAt = now; }
    const float k = Motion::Reduced() ? 1.f : std::min(1.f, (float)((now - sChangedAt) / 0.6));
    // In the theme's accent, faint; fainter still on a light page.
    const bool light = ColorLuma(gTheme.bg) >= 128;
    const SDL_Color tint = Mix(gTheme.gold, gTheme.text, light ? 0.6f : 0.25f);
    const float base = light ? 0.045f : 0.062f;
    // A slow diagonal drift, like the game's menus.
    const float t = Motion::Reduced() ? 0.f : (float)now;
    SDL_Rect oldClip{};
    const bool hadClip = SDL_RenderIsClipEnabled(gRen);
    if (hadClip) SDL_RenderGetClipRect(gRen, &oldClip);
    SDL_RenderSetClipRect(gRen, &area);
    auto layer = [&](Id id, float alpha) {
        int w = 0, h = 0;
        SDL_Texture* tex = Get(id, &w, &h);
        if (!tex || w <= 0 || h <= 0 || alpha <= 0.002f) return;
        const int ox = (int)fmodf(t * 14.f, (float)w), oy = (int)fmodf(t * 9.f, (float)h);
        SDL_SetTextureColorMod(tex, tint.r, tint.g, tint.b);
        SDL_SetTextureAlphaMod(tex, (Uint8)lroundf(255.f * alpha));
        for (int y = area.y - h + oy; y < area.y + area.h; y += h)
            for (int x = area.x - w + ox; x < area.x + area.w; x += w) {
                SDL_Rect d{ x, y, w, h };
                SDL_RenderCopy(gRen, tex, nullptr, &d);
            }
        SDL_SetTextureColorMod(tex, 255, 255, 255);
        SDL_SetTextureAlphaMod(tex, 255);
    };
    if (sPrev != Id::Count && k < 1.f) layer(sPrev, base * (1.f - k));
    if (sCur != Id::Count) layer(sCur, base * k);
    if (hadClip) SDL_RenderSetClipRect(gRen, &oldClip); else SDL_RenderSetClipRect(gRen, nullptr);
}

void Purge() {
    for (Slot& s : sSlots) {
        if (s.tex) SDL_DestroyTexture(s.tex);
        s = Slot{};
    }
    for (auto& kv : sItemPics) if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    sItemPics.clear();
}

}  // namespace Art
