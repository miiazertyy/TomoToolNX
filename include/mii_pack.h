#pragma once
// mii_pack.h — TomoToolNX
// Makes the Mii parts pack (see mii_art.h) on the console itself, from the
// copy of the game installed on it: the game's files are read where they are
// (its RomFS, mounted read-only), nothing is downloaded and nothing of the
// game is in the app. The same steps as tools/mii/build_pack.py on a
// computer: the face parts are decoded from the game's textures, the heads,
// hairstyles and noses drawn from the front from its 3D models.
//
// It runs on its own thread; the pack is written next to where MiiArt reads
// it (sdmc:/switch/TomoToolNX/miiparts/) and swapped in when complete.

#include <string>

namespace MiiPack {

enum class State : int { Idle, Running, Done, Failed };

// Start making the pack when there is none (or `force`: make it again).
void Start(bool force);
State GetState();
float Progress();             // 0..1 while running
std::string Error();          // when Failed: a language key ("miipack.nogame") or a short text
// True once after a pack was made: the main thread then calls MiiArt::Reload.
bool TakeFinished();
// Is a pack there (made here or copied from a computer)?
bool Present();
// The app carries its own pack (romfs/miiparts): nothing to make.
bool BuiltIn();
// Leaving the app: stop a pack being made (it is made again next time).
void Stop();

}  // namespace MiiPack
