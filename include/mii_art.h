#pragma once
// mii_art.h — TomoToolNX
// Real Mii pictures, drawn from the Mii parts pack (romfs/miiparts, made by
// tools/mii/build_pack.py from the game): front views of the game's own head
// shapes, ears, hairstyles, hair parts, noses, beards and glasses, each with
// a depth picture so they overlap as in the game, and its face parts (eyes,
// eyebrows, mouths, lids, lashes, ...). Without a pack, Available() is false
// and the app keeps its drawn faces.
//
// A Mii's picture comes from its CharInfoEx, placed as the game does
// (tools/mii/compose.py is the same drawing in Python, and the reference):
//  - the head's own texture: the skin, then blush, eyeshadow, wrinkles and
//    stubble, at the game's places on it (half the face, mirrored);
//  - the face texture: eyebrows, eyes with their lids, lashes and shine, the
//    mouth (lips only with lipstick), moustache and mole, and the face paint;
//  - hair in two colours where the game's masks put the second one.

#include <SDL2/SDL.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace MiiArt {

// A face paint: its canvas, decoded (RGBA, sRGB, straight alpha).
struct FacePaint {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    uint64_t key = 0;
};

bool Available();
// The face paint whose canvas is at canvasPath (a save's
// Ugc/UgcFacePaintNNN.canvas.zs), read once and kept until Purge; nullptr
// when there is none. Any thread.
const FacePaint* LoadFacePaint(const std::string& canvasPath);
// The picture (transparent around the head), kept until Purge; nullptr when
// there is no pack or the data is too short. paintFront: the face paint goes
// over the face's parts (the save's IsFacePaintFront), else under them.
SDL_Texture* Portrait(const uint8_t* charInfoEx, size_t size, const FacePaint* paint = nullptr, bool paintFront = false);
// The same picture as RGBA bytes (n x n), for the WebUI; any thread. false
// without a pack or with too little data.
bool PortraitRgba(const uint8_t* charInfoEx, size_t size, std::vector<uint8_t>& rgba, int& n,
                  const FacePaint* paint = nullptr, bool paintFront = false);
void Purge();
// Read the pack again (a new one was just made): main thread.
void Reload();

}  // namespace MiiArt
