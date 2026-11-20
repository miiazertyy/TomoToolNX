#pragma once
// game_data.h — TomoToolNX
// Reading the game's own files: where the app keeps the few it carries,
// and readers for the formats
// the Mii parts and the 3D item previews need: zstd, SARC, BYML, BNTX (the
// first mip, decoded) and BFRES models (shapes with positions, normals, UVs
// and indices; bones). Ports of tools/mii/nxfmt.py.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace GameData {

// The game's files the app reads (the 3D item view's tables and models): the
// copy it carries, "romfs:/game/" (tools/assets/build_game_files.py), or for
// tests "sdmc:/switch/TomoToolNX/miiparts_src/"; "" when neither is there.
// Any thread.
std::string Root();
void Close();                                   // leaving the app
// Why Root() found nothing ("nofile"), "" when it did.
std::string Why();

using Buf = std::vector<uint8_t>;
Buf ReadFile(const std::string& path);           // the whole file, unzstd'ed when it is .zs
// The first file in `dir` whose name starts with `prefix` ("" when none).
std::string FindFile(const std::string& dir, const std::string& prefix);

// SARC: (name, (offset, size)) of each file inside.
std::vector<std::pair<std::string, std::pair<size_t, size_t>>> Sarc(const Buf& d);

// BYML, as a tree of values.
struct Val {
    enum T : uint8_t { Null, Bool, Int, Float, Str, Arr, Dict } t = Null;
    int64_t i = 0;
    double f = 0;
    std::string s;
    std::vector<Val> a;
    std::vector<std::pair<std::string, Val>> m;
    const Val* Get(const char* k) const { for (auto& kv : m) if (kv.first == k) return &kv.second; return nullptr; }
    int64_t AsInt(int64_t def = 0) const { return t == Int || t == Bool ? i : t == Float ? (int64_t)f : def; }
    bool AsBool() const { return (t == Bool || t == Int) ? i != 0 : false; }
    std::string AsStr() const { return t == Str ? s : std::string(); }
};
bool ParseByml(const uint8_t* d, size_t n, Val& out);

// BNTX: each texture's first mip; DecodeTex gives it as RGBA (BC4: the value
// in red, as texture2ddecoder does).
struct BntxTex { std::string name; uint32_t fmt = 0; int w = 0, h = 0, tile = 0, bhLog2 = 0; size_t off = 0, size = 0; };
std::vector<BntxTex> Bntx(const Buf& d);
bool DecodeTex(const Buf& d, const BntxTex& t, std::vector<uint8_t>& rgba);

// BFRES: one model (by name, or the first): shapes and bones.
struct Shape { std::string name; std::vector<float> p, n, uv; std::vector<uint32_t> idx; };
struct Bone { std::string name; int parent = -1; float t[3] = { 0, 0, 0 }; };
struct Model { std::string name; std::vector<Shape> shapes; std::vector<Bone> bones; bool ok = false; };
Model Bfres(const Buf& d, const std::string& modelName = std::string());
// A bone's place in the model (the translations up its parents added).
void BoneWorld(const Model& m, const char* name, float out[3]);

}  // namespace GameData
