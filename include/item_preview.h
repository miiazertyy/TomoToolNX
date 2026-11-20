#pragma once
// item_preview.h — TomoToolNX
// A texture on its item, in 3D: the game's own model for the slot's type
// (clothes: UGC.Cloth.UgcClothType, goods: UGC.Goods.UgcGoodsType, objects:
// UGC.MapObject.UgcMapObjectType), found through the game's tables
// (RSDB/UgcClothParam, UgcObj, UgcTypeParam), with the texture laid on the
// part that takes it, drawn turning by the software rasterizer.
//
// The models come from the game installed on the console (GameData::Root).

#include <SDL2/SDL.h>
#include <cstdint>
#include <string>
#include <vector>

namespace ItemPreview {

// Is there a model for this kind (UgcKindOfStem: 1 clothes, 2 goods, 5
// objects) and type? Reads the game's tables the first time.
bool Has(int kind, uint32_t type);
// The item with the texture at `texPath` on it, turned by `yaw` (radians),
// drawn into an n x n texture kept here (nullptr when it cannot be drawn).
// `key` names the texture (its stem and version): a new key loads it again.
SDL_Texture* Draw(SDL_Renderer* ren, const std::string& key, int kind, uint32_t type, const std::string& texPath, int n, float yaw);
void Clear();
// The game's files this view reads (paths inside the game's RomFS: the three
// tables, then every model they name).
std::vector<std::string> GameFiles();

}  // namespace ItemPreview
