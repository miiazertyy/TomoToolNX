#pragma once
// save_editor.h — TomoToolNX
// Binary parser/writer for Tomodachi Life .sav files (Player.sav, Mii.sav).
// Format is identical to what the web-based save editor uses.

#include <string>
#include <vector>
#include <cstdint>

namespace SaveEditor {

// ── DataType enum (matches game's 33-type save format) ───────────────────────
enum DataType : uint32_t {
    DT_Bool=0,   DT_BoolArray=1,
    DT_Int=2,    DT_IntArray=3,
    DT_Float=4,  DT_FloatArray=5,
    DT_Enum=6,   DT_EnumArray=7,
    DT_Vec2=8,   DT_Vec2Array=9,
    DT_Vec3=10,  DT_Vec3Array=11,
    DT_Str16=12, DT_Str16Array=13,
    DT_Str32=14, DT_Str32Array=15,
    DT_Str64=16, DT_Str64Array=17,
    DT_Bin=18,   DT_BinArray=19,
    DT_UInt=20,  DT_UIntArray=21,
    DT_Int64=22, DT_Int64Array=23,
    DT_UInt64=24,DT_UInt64Array=25,
    DT_WStr16=26,DT_WStr16Array=27,
    DT_WStr32=28,DT_WStr32Array=29,
    DT_WStr64=30,DT_WStr64Array=31,
    DT_Bool64Key=32,
    DT_COUNT=33
};

struct Entry {
    uint32_t hash     = 0;
    DataType type     = DT_Bool;
    uint32_t inlineRaw = 0;           // valid for Bool/Int/UInt/Float/Enum
    std::vector<uint8_t> payload;     // valid for all heap types
};

struct SavFile {
    uint32_t version = 0;
    std::vector<Entry> entries;
    bool loaded = false;
};

// Load a .sav from disk. Returns true on success; sets err on failure.
bool Load(const std::string& path, SavFile& out, std::string& err);

// Write a .sav to disk. Returns empty string on success, error otherwise.
std::string Save(const std::string& path, const SavFile& sav);

// Murmur3 x86 32-bit hash (seed 0) — matches the game's field name hashing.
uint32_t Hash(const char* name);

// ── Scalar inline accessors (Bool/Int/UInt/Float/Enum) ───────────────────────
int32_t  GetInt  (const SavFile& s, uint32_t h, int32_t  def = 0);
uint32_t GetUInt (const SavFile& s, uint32_t h, uint32_t def = 0);
uint32_t GetEnum (const SavFile& s, uint32_t h, uint32_t def = 0);
void     SetInt  (SavFile& s, uint32_t h, int32_t  v);
void     SetUInt (SavFile& s, uint32_t h, uint32_t v);
void     SetEnum (SavFile& s, uint32_t h, uint32_t v);

// ── Array element accessors (heap types) ─────────────────────────────────────
int32_t  GetIntAt  (const SavFile& s, uint32_t h, int idx, int32_t  def = 0);
uint32_t GetUIntAt (const SavFile& s, uint32_t h, int idx, uint32_t def = 0);
uint32_t GetEnumAt (const SavFile& s, uint32_t h, int idx, uint32_t def = 0);
void     SetIntAt  (SavFile& s, uint32_t h, int idx, int32_t  v);
void     SetUIntAt (SavFile& s, uint32_t h, int idx, uint32_t v);
void     SetEnumAt (SavFile& s, uint32_t h, int idx, uint32_t v);
int      ArraySize (const SavFile& s, uint32_t h);  // element count of any array entry

// ── Wide-string accessors (UTF-8 ↔ UTF-16LE in payload) ──────────────────────
// Scalar WStr32 (DataType 28, 64-byte payload)
std::string GetWStr32   (const SavFile& s, uint32_t h);
void        SetWStr32   (SavFile& s, uint32_t h, const std::string& utf8);

// Array element: WStr32Array (DataType 29, each element 64 bytes)
std::string GetWStr32At (const SavFile& s, uint32_t h, int idx);
void        SetWStr32At (SavFile& s, uint32_t h, int idx, const std::string& utf8);

} // namespace SaveEditor
