// save_editor.cpp — TomoToolNX
// Binary .sav parser/writer. Mirrors the logic in the web save editor's
// parse.ts / write.ts / hash.ts, ported to plain C++17.

#include "save_editor.h"
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <sys/stat.h>

namespace SaveEditor {

// The undo history's hooks (SetChangeHook, SetLoadHook).
static void (*sChangeHook)(SavFile&, const Entry&) = nullptr;
static void (*sLoadHook)(SavFile&) = nullptr;

// ── Murmur3 x86 32-bit (seed 0) ──────────────────────────────────────────────
static uint32_t rotl32(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }

uint32_t Hash(const char* name) {
    const uint8_t* data = (const uint8_t*)name;
    int len = (int)strlen(name);
    int nblocks = len / 4;
    uint32_t h1 = 0;
    const uint32_t c1 = 0xcc9e2d51u, c2 = 0x1b873593u;

    for (int i = 0; i < nblocks; i++) {
        uint32_t k1;
        memcpy(&k1, data + i * 4, 4);
        k1 *= c1; k1 = rotl32(k1, 15); k1 *= c2;
        h1 ^= k1; h1 = rotl32(h1, 13);
        h1 = h1 * 5u + 0xe6546b64u;
    }

    const uint8_t* tail = data + nblocks * 4;
    uint32_t k1 = 0;
    switch (len & 3) {
        case 3: k1 ^= (uint32_t)tail[2] << 16; // fallthrough
        case 2: k1 ^= (uint32_t)tail[1] << 8;  // fallthrough
        case 1: k1 ^= tail[0];
                k1 *= c1; k1 = rotl32(k1, 15); k1 *= c2; h1 ^= k1;
    }

    h1 ^= (uint32_t)len;
    h1 ^= h1 >> 16; h1 *= 0x85ebca6bu;
    h1 ^= h1 >> 13; h1 *= 0xc2b2ae35u;
    h1 ^= h1 >> 16;
    return h1;
}

// ── Little-endian helpers ─────────────────────────────────────────────────────
static uint32_t R32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static void W32(uint8_t* p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}

static bool IsInline(DataType t) {
    return t==DT_Bool || t==DT_Int || t==DT_UInt || t==DT_Float || t==DT_Enum;
}

// Returns fixed heap payload size, or 0 for variable-length types.
static size_t FixedSize(DataType t) {
    switch (t) {
        case DT_Int64:  case DT_UInt64: case DT_Vec2:  return 8;
        case DT_Vec3:                                  return 12;
        case DT_Str16:                                 return 16;
        case DT_Str32:                                 return 32;
        case DT_Str64:                                 return 64;
        case DT_WStr16:                                return 32;
        case DT_WStr32:                                return 64;
        case DT_WStr64:                                return 128;
        default:                                       return 0;
    }
}

// ── Heap payloads ─────────────────────────────────────────────────────────────
// Strict by design. A payload that cannot be read exactly is an error, never an
// empty entry: Serialize() lays the heap out from the payload sizes, so a single
// silently-empty payload shifts every entry after it and produces a file the
// game refuses to load. The reference parser (ltd-save-editor parse.ts) throws
// in the same places.
static bool ReadHeap(const uint8_t* b, size_t size, uint32_t off, DataType t,
                     std::vector<uint8_t>& out, std::string& err) {
    auto fail = [&](const char* what) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%s (type %u at 0x%X)", what, (unsigned)t, (unsigned)off);
        err = buf;
        return false;
    };
    out.clear();

    size_t fixed = FixedSize(t);
    if (fixed) {
        if ((size_t)off + fixed > size) return fail("Entry runs past the end of the file");
        out.assign(b + off, b + off + fixed);
        return true;
    }

    if ((size_t)off + 4 > size) return fail("Entry runs past the end of the file");
    const size_t count = R32(b + off);
    size_t total = 0;

    switch (t) {
        case DT_Bin:       total = 4 + count; break;
        case DT_BoolArray: {
            size_t byteSize = std::max((size_t)4, (count + 7) / 8);
            size_t aligned  = (byteSize + 3) & ~(size_t)3;
            total = 4 + aligned;
            break;
        }
        case DT_IntArray: case DT_UIntArray:
        case DT_FloatArray: case DT_EnumArray: total = 4 + count * 4; break;
        case DT_Int64Array: case DT_UInt64Array:
        case DT_Vec2Array:                     total = 4 + count * 8; break;
        case DT_Vec3Array:                     total = 4 + count * 12; break;
        case DT_Str16Array:                    total = 4 + count * 16; break;
        case DT_Str32Array:                    total = 4 + count * 32; break;
        case DT_Str64Array:                    total = 4 + count * 64; break;
        case DT_WStr16Array:                   total = 4 + count * 32; break;
        case DT_WStr32Array:                   total = 4 + count * 64; break;
        case DT_WStr64Array:                   total = 4 + count * 128; break;
        case DT_BinArray: {
            size_t p = (size_t)off + 4;
            for (size_t i = 0; i < count; i++) {
                if (p + 4 > size) return fail("Binary array runs past the end of the file");
                size_t sz = R32(b + p);
                p += 4 + sz;
                if (p > size) return fail("Binary array runs past the end of the file");
            }
            out.assign(b + off, b + p);
            return true;
        }
        default: return fail("Unsupported entry type");
    }

    if ((size_t)off + total > size) return fail("Entry runs past the end of the file");
    out.assign(b + off, b + off + total);
    return true;
}

static void RebuildIndex(SavFile& s) {
    s.index.clear();
    s.index.reserve(s.entries.size());
    // emplace keeps the FIRST entry for a hash, matching the old linear scan.
    for (size_t i = 0; i < s.entries.size(); i++)
        s.index.emplace(s.entries[i].hash, (uint32_t)i);
}

// ── Parse ─────────────────────────────────────────────────────────────────────
bool Parse(const uint8_t* b, size_t sz, SavFile& out, std::string& err) {
    if (!b || sz < 0x20) { err = "File too small"; return false; }
    if (b[0]!=0x04 || b[1]!=0x03 || b[2]!=0x02 || b[3]!=0x01) { err = "Bad magic"; return false; }

    SavFile sav;
    sav.version = R32(b + 4);
    const size_t saveDataOff = R32(b + 8);
    if (saveDataOff < 0x20 || saveDataOff > sz || ((saveDataOff - 0x20) & 7) != 0) {
        err = "Bad saveDataOffset"; return false;
    }

    size_t pos = 0x20;
    DataType curType = DT_Bool;

    while (pos + 8 <= saveDataOff) {
        uint32_t hash = R32(b + pos); pos += 4;
        uint32_t slot = R32(b + pos); pos += 4;
        if (hash == 0) {
            // Section marker. An index the format does not define used to be
            // read as Bool, which turned every entry after it into nonsense.
            if (slot >= (uint32_t)DT_COUNT) {
                err = "Unknown entry type " + std::to_string(slot);
                return false;
            }
            curType = (DataType)slot;
            continue;
        }
        Entry e;
        e.hash = hash; e.type = curType;
        if (IsInline(curType)) {
            e.inlineRaw = slot;
        } else if (curType == DT_Bool64Key) {
            // Offset 0 means "no table" and is the only form we can write back.
            // A real table would be dropped on save, so refuse rather than lose it.
            if (slot != 0) {
                err = "This save uses a Bool64Key table, which cannot be rewritten safely";
                return false;
            }
        } else if (!ReadHeap(b, sz, slot, curType, e.payload, err)) {
            return false;
        }
        sav.entries.push_back(std::move(e));
    }

    if (sav.entries.empty()) { err = "Save has no entries"; return false; }
    RebuildIndex(sav);
    sav.loaded = true;
    out = std::move(sav);
    return true;
}

// ── Load ──────────────────────────────────────────────────────────────────────
// On failure `out` is left exactly as it was, so a failed reload never replaces
// good in-memory data with an empty file that a later Save() could write out.
bool Load(const std::string& path, SavFile& out, std::string& err) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { err = "Cannot open: " + path; return false; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); err = "Cannot size: " + path; return false; }
    std::vector<uint8_t> b((size_t)len);
    size_t nread = b.empty() ? 0 : fread(b.data(), 1, b.size(), f);
    fclose(f);
    if (nread != b.size()) { err = "Short read: " + path; return false; }

    SavFile parsed;
    if (!Parse(b.data(), b.size(), parsed, err)) return false;
    out = std::move(parsed);
    if (sLoadHook) sLoadHook(out);
    return true;
}

// ── Serialize ─────────────────────────────────────────────────────────────────
std::string Serialize(const SavFile& sav, std::vector<uint8_t>& out) {
    out.clear();
    // An unloaded SavFile serializes to a valid-looking 296-byte file with no
    // entries. Writing that over Mii.sav / Player.sav destroys the save, and it
    // used to happen whenever a "dirty" flag was set for a file that had never
    // been opened in this session.
    if (!sav.loaded || sav.entries.empty()) return "Nothing loaded to save";

    // Group entry indices by DataType
    std::vector<std::vector<size_t>> byType((size_t)DT_COUNT);
    for (size_t i = 0; i < sav.entries.size(); i++) {
        const Entry& e = sav.entries[i];
        uint32_t t = (uint32_t)e.type;
        if (t >= (uint32_t)DT_COUNT) return "Entry has an invalid type";
        if (!IsInline(e.type) && e.type != DT_Bool64Key && e.payload.empty())
            return "Entry is missing its data";
        byType[t].push_back(i);
    }

    // Index row count: one sentinel per type + one row per entry
    size_t rowCount = (size_t)DT_COUNT + sav.entries.size();
    uint32_t saveDataOff = (uint32_t)(0x20 + rowCount * 8);

    // Compute heap offsets in type order (matches write.ts)
    std::vector<uint32_t> heapOff(sav.entries.size(), 0);
    uint64_t heapPos = saveDataOff;
    for (size_t t = 0; t < (size_t)DT_COUNT; t++) {
        for (size_t idx : byType[t]) {
            const auto& e = sav.entries[idx];
            if (IsInline(e.type)) continue;
            if (e.type == DT_Bool64Key && e.payload.empty()) { heapOff[idx] = 0; continue; }
            heapOff[idx] = (uint32_t)heapPos;
            heapPos += e.payload.size();
        }
    }
    if (heapPos > 0x7fffffffu) return "Save is too large";

    out.assign((size_t)heapPos, 0);

    // Header
    out[0]=0x04; out[1]=0x03; out[2]=0x02; out[3]=0x01;
    W32(out.data()+4, sav.version);
    W32(out.data()+8, saveDataOff);

    // Index
    size_t p = 0x20;
    for (size_t t = 0; t < (size_t)DT_COUNT; t++) {
        W32(out.data()+p, 0); p += 4;
        W32(out.data()+p, (uint32_t)t); p += 4;
        for (size_t idx : byType[t]) {
            const auto& e = sav.entries[idx];
            W32(out.data()+p, e.hash); p += 4;
            if (IsInline(e.type)) W32(out.data()+p, e.inlineRaw);
            else                  W32(out.data()+p, heapOff[idx]);
            p += 4;
        }
    }

    // Heap data
    for (size_t i = 0; i < sav.entries.size(); i++) {
        const auto& e = sav.entries[i];
        if (IsInline(e.type) || e.payload.empty()) continue;
        memcpy(out.data() + heapOff[i], e.payload.data(), e.payload.size());
    }
    return "";
}

// ── Save ──────────────────────────────────────────────────────────────────────
std::string Save(const std::string& path, const SavFile& sav) {
    std::vector<uint8_t> out;
    std::string err = Serialize(sav, out);
    if (!err.empty()) return err + ": " + path;

    // Every step is checked. A short write that went unnoticed here used to be
    // committed as-is, leaving a truncated .sav in the real save data.
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return "Cannot open for write: " + path;
    size_t n = fwrite(out.data(), 1, out.size(), f);
    int closeRc = fclose(f);
    if (n != out.size())
        return "Short write: " + path + " (" + std::to_string(n) + "/" + std::to_string(out.size()) + ")";
    if (closeRc != 0) return "Close failed: " + path;
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return "Cannot verify: " + path;
    if ((size_t)st.st_size != out.size())
        return "Size mismatch after write: " + path + " (" + std::to_string((size_t)st.st_size)
             + "/" + std::to_string(out.size()) + ")";
    return "";
}

bool HasEntry(const SavFile& s, uint32_t h) {
    for (auto& e : s.entries) if (e.hash == h) return true;
    return false;
}

// ── Entry lookup ──────────────────────────────────────────────────────────────
// Lookups go through the hash index built at parse time. The accessors below are
// called hundreds of times per frame by the editor screens; a linear scan over
// Player.sav's ~3800 entries for each of them was a measurable part of a frame.
// The index entry is re-checked against the vector, and anything that does not
// line up falls back to the scan, so a stale index can never return a wrong entry.
static const Entry* Find(const SavFile& s, uint32_t h) {
    auto it = s.index.find(h);
    if (it != s.index.end() && it->second < s.entries.size() && s.entries[it->second].hash == h)
        return &s.entries[it->second];
    for (auto& e : s.entries) if (e.hash == h) return &e;
    return nullptr;
}
void SetChangeHook(void (*fn)(SavFile&, const Entry&)) { sChangeHook = fn; }
void SetLoadHook(void (*fn)(SavFile&)) { sLoadHook = fn; }
void Touched(SavFile& s, const Entry& e) { if (sChangeHook) sChangeHook(s, e); }

// Only the setters ask for a mutable entry: each one is about to change it.
static Entry* FindMut(SavFile& s, uint32_t h) {
    Entry* e = const_cast<Entry*>(Find(s, h));
    if (e && sChangeHook) sChangeHook(s, *e);
    return e;
}

const uint8_t* GetBinAt(const SavFile& s, uint32_t h, int idx, size_t* size) {
    if (size) *size = 0;
    const Entry* e = Find(s, h);
    if (!e || e->type != DT_BinArray || idx < 0 || e->payload.size() < 4) return nullptr;
    const uint8_t* b = e->payload.data();
    const size_t n = e->payload.size();
    const uint32_t count = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    if ((uint32_t)idx >= count) return nullptr;
    size_t p = 4;
    for (int i = 0; ; i++) {
        if (p + 4 > n) return nullptr;
        const size_t sz = (size_t)b[p] | ((size_t)b[p + 1] << 8) | ((size_t)b[p + 2] << 16) | ((size_t)b[p + 3] << 24);
        if (p + 4 + sz > n) return nullptr;
        if (i == idx) { if (size) *size = sz; return b + p + 4; }
        p += 4 + sz;
    }
}

// ── Scalar accessors ──────────────────────────────────────────────────────────
int32_t GetInt(const SavFile& s, uint32_t h, int32_t def) {
    auto* e = Find(s,h); if (!e || e->type!=DT_Int) return def;
    return (int32_t)e->inlineRaw;
}
uint32_t GetUInt(const SavFile& s, uint32_t h, uint32_t def) {
    auto* e = Find(s,h); if (!e || e->type!=DT_UInt) return def;
    return e->inlineRaw;
}
void SetInt(SavFile& s, uint32_t h, int32_t v) {
    auto* e = FindMut(s,h); if (e && e->type==DT_Int) e->inlineRaw = (uint32_t)v;
}
void SetUInt(SavFile& s, uint32_t h, uint32_t v) {
    auto* e = FindMut(s,h); if (e && e->type==DT_UInt) e->inlineRaw = v;
}
uint32_t GetEnum(const SavFile& s, uint32_t h, uint32_t def) {
    auto* e = Find(s,h); if (!e || e->type!=DT_Enum) return def;
    return e->inlineRaw;
}
void SetEnum(SavFile& s, uint32_t h, uint32_t v) {
    auto* e = FindMut(s,h); if (e && e->type==DT_Enum) e->inlineRaw = v;
}
uint32_t GetAnyScalar(const SavFile& s, uint32_t h, uint32_t def) {
    auto* e = Find(s,h);
    if (!e) return def;
    switch(e->type) {
        case DT_Bool: case DT_Int: case DT_UInt: case DT_Float: case DT_Enum:
            return e->inlineRaw;
        default: return def;
    }
}
void SetAnyScalar(SavFile& s, uint32_t h, uint32_t v) {
    auto* e = FindMut(s,h);
    if (!e) return;
    switch(e->type) {
        case DT_Bool: case DT_Int: case DT_UInt: case DT_Float: case DT_Enum:
            e->inlineRaw = v; break;
        default: break;
    }
}
int ArraySize(const SavFile& s, uint32_t h) {
    auto* e = Find(s,h);
    if (!e || e->payload.size() < 4) return 0;
    return (int)R32(e->payload.data());
}

// ── Array helpers ─────────────────────────────────────────────────────────────
static bool ArrayRead(const Entry* e, DataType expected, int idx, uint32_t& out) {
    if (!e || e->type!=expected || e->payload.size()<4) return false;
    uint32_t cnt = R32(e->payload.data());
    if (idx<0 || (uint32_t)idx>=cnt) return false;
    size_t off = 4 + (size_t)idx * 4;
    if (off+4 > e->payload.size()) return false;
    out = R32(e->payload.data() + off);
    return true;
}
static bool ArrayWrite(Entry* e, DataType expected, int idx, uint32_t val) {
    if (!e || e->type!=expected || e->payload.size()<4) return false;
    uint32_t cnt = R32(e->payload.data());
    if (idx<0 || (uint32_t)idx>=cnt) return false;
    size_t off = 4 + (size_t)idx * 4;
    if (off+4 > e->payload.size()) return false;
    W32(e->payload.data() + off, val);
    return true;
}

int32_t GetIntAt(const SavFile& s, uint32_t h, int idx, int32_t def) {
    uint32_t raw = 0;
    return ArrayRead(Find(s,h), DT_IntArray, idx, raw) ? (int32_t)raw : def;
}
uint32_t GetUIntAt(const SavFile& s, uint32_t h, int idx, uint32_t def) {
    uint32_t raw = 0;
    return ArrayRead(Find(s,h), DT_UIntArray, idx, raw) ? raw : def;
}
uint32_t GetEnumAt(const SavFile& s, uint32_t h, int idx, uint32_t def) {
    uint32_t raw = 0;
    return ArrayRead(Find(s,h), DT_EnumArray, idx, raw) ? raw : def;
}
void SetIntAt(SavFile& s, uint32_t h, int idx, int32_t v) {
    ArrayWrite(FindMut(s,h), DT_IntArray, idx, (uint32_t)v);
}
void SetUIntAt(SavFile& s, uint32_t h, int idx, uint32_t v) {
    ArrayWrite(FindMut(s,h), DT_UIntArray, idx, v);
}
float GetFloatAt(const SavFile& s, uint32_t h, int idx, float def) {
    uint32_t raw = 0;
    if (!ArrayRead(Find(s,h), DT_FloatArray, idx, raw)) return def;
    float v;
    memcpy(&v, &raw, 4);
    return v;
}
void SetFloatAt(SavFile& s, uint32_t h, int idx, float v) {
    uint32_t raw;
    memcpy(&raw, &v, 4);
    ArrayWrite(FindMut(s,h), DT_FloatArray, idx, raw);
}

// Scalar UInt64 — DT_UInt64 stores 8 raw bytes in payload (no count
// prefix). Used by Player.sav fields like LastPenaltyTime.
uint64_t GetUInt64(const SavFile& s, uint32_t h, uint64_t def) {
    const Entry* e = Find(s, h);
    if (!e || e->type != DT_UInt64 || e->payload.size() < 8) return def;
    uint32_t lo = R32(e->payload.data());
    uint32_t hi = R32(e->payload.data() + 4);
    return (uint64_t)lo | ((uint64_t)hi << 32);
}
void SetUInt64(SavFile& s, uint32_t h, uint64_t v) {
    Entry* e = FindMut(s, h);
    if (!e || e->type != DT_UInt64) return;
    if (e->payload.size() < 8) e->payload.resize(8);
    W32(e->payload.data(),     (uint32_t)(v & 0xFFFFFFFFu));
    W32(e->payload.data() + 4, (uint32_t)(v >> 32));
}

uint64_t GetUInt64At(const SavFile& s, uint32_t h, int idx, uint64_t def) {
    const Entry* e = Find(s, h);
    if (!e || e->type != DT_UInt64Array || e->payload.size() < 4) return def;
    uint32_t cnt = R32(e->payload.data());
    if (idx < 0 || (uint32_t)idx >= cnt) return def;
    size_t off = 4 + (size_t)idx * 8;
    if (off + 8 > e->payload.size()) return def;
    uint32_t lo = R32(e->payload.data() + off);
    uint32_t hi = R32(e->payload.data() + off + 4);
    return (uint64_t)lo | ((uint64_t)hi << 32);
}
void SetUInt64At(SavFile& s, uint32_t h, int idx, uint64_t v) {
    Entry* e = FindMut(s, h);
    if (!e || e->type != DT_UInt64Array || e->payload.size() < 4) return;
    uint32_t cnt = R32(e->payload.data());
    if (idx < 0 || (uint32_t)idx >= cnt) return;
    size_t off = 4 + (size_t)idx * 8;
    if (off + 8 > e->payload.size()) return;
    W32(e->payload.data() + off,     (uint32_t)(v & 0xFFFFFFFFu));
    W32(e->payload.data() + off + 4, (uint32_t)(v >> 32));
}
void SetEnumAt(SavFile& s, uint32_t h, int idx, uint32_t v) {
    ArrayWrite(FindMut(s,h), DT_EnumArray, idx, v);
}
bool GetBoolAt(const SavFile& s, uint32_t h, int idx, bool def) {
    const Entry* e = Find(s, h);
    if (!e || e->type != DT_BoolArray || e->payload.size() < 4) return def;
    uint32_t cnt = R32(e->payload.data());
    if (idx < 0 || (uint32_t)idx >= cnt) return def;
    size_t byteOff = 4 + (size_t)(idx >> 3);
    if (byteOff >= e->payload.size()) return def;
    return ((e->payload[byteOff] >> (idx & 7)) & 1) != 0;
}
void SetBoolAt(SavFile& s, uint32_t h, int idx, bool v) {
    Entry* e = FindMut(s, h);
    if (!e || e->type != DT_BoolArray || e->payload.size() < 4) return;
    uint32_t cnt = R32(e->payload.data());
    if (idx < 0 || (uint32_t)idx >= cnt) return;
    size_t byteOff = 4 + (size_t)(idx >> 3);
    if (byteOff >= e->payload.size()) return;
    uint8_t mask = (uint8_t)(1u << (idx & 7));
    if (v) e->payload[byteOff] |= mask;
    else   e->payload[byteOff] &= (uint8_t)~mask;
}
// Reads from EnumArray or UIntArray (identical binary layout — game uses either).
uint32_t GetAnyEnumAt(const SavFile& s, uint32_t h, int idx, uint32_t def) {
    const Entry* e = Find(s, h);
    if (!e || e->payload.size() < 4) return def;
    if (e->type != DT_EnumArray && e->type != DT_UIntArray) return def;
    uint32_t cnt = R32(e->payload.data());
    if (idx < 0 || (uint32_t)idx >= cnt) return def;
    size_t off = 4 + (size_t)idx * 4;
    if (off + 4 > e->payload.size()) return def;
    return R32(e->payload.data() + off);
}
void SetAnyEnumAt(SavFile& s, uint32_t h, int idx, uint32_t v) {
    Entry* e = FindMut(s, h);
    if (!e || e->payload.size() < 4) return;
    if (e->type != DT_EnumArray && e->type != DT_UIntArray) return;
    uint32_t cnt = R32(e->payload.data());
    if (idx < 0 || (uint32_t)idx >= cnt) return;
    size_t off = 4 + (size_t)idx * 4;
    if (off + 4 > e->payload.size()) return;
    W32(e->payload.data() + off, v);
}

// ── UTF-16LE ↔ UTF-8 ──────────────────────────────────────────────────────────
static std::string Utf16LeToUtf8(const uint8_t* buf, size_t maxBytes) {
    std::string out;
    for (size_t i = 0; i + 1 < maxBytes; i += 2) {
        uint16_t cp = (uint16_t)buf[i] | ((uint16_t)buf[i+1] << 8);
        if (cp == 0) break;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < maxBytes) {
            uint16_t lo = (uint16_t)buf[i+2] | ((uint16_t)buf[i+3] << 8);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                uint32_t full = 0x10000u + (((uint32_t)cp - 0xD800u) << 10) + ((uint32_t)lo - 0xDC00u);
                out += (char)(0xF0 | (full >> 18));
                out += (char)(0x80 | ((full >> 12) & 0x3F));
                out += (char)(0x80 | ((full >> 6) & 0x3F));
                out += (char)(0x80 | (full & 0x3F));
                i += 2;
                continue;
            }
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) { out += '?'; continue; }   // lone surrogate
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// maxChars = max UTF-16 code units including null terminator
static void Utf8ToUtf16Le(const std::string& utf8, uint8_t* buf, size_t maxChars) {
    memset(buf, 0, maxChars * 2);
    size_t out = 0;
    for (size_t i = 0; i < utf8.size() && out < maxChars - 1; ) {
        uint8_t c = (uint8_t)utf8[i];
        uint32_t cp;
        if (c < 0x80)                                    { cp = c;                                                              i += 1; }
        else if ((c & 0xE0)==0xC0 && i+1<utf8.size())   { cp = ((c&0x1F)<<6)   | ((uint8_t)utf8[i+1]&0x3F);                  i += 2; }
        else if ((c & 0xF0)==0xE0 && i+2<utf8.size())   { cp = ((c&0x0F)<<12)  | (((uint8_t)utf8[i+1]&0x3F)<<6)
                                                              | ((uint8_t)utf8[i+2]&0x3F);                                    i += 3; }
        else if ((c & 0xF8)==0xF0 && i+3<utf8.size())   { cp = ((c&0x07)<<18)  | (((uint8_t)utf8[i+1]&0x3F)<<12)
                                                              | (((uint8_t)utf8[i+2]&0x3F)<<6)
                                                              | ((uint8_t)utf8[i+3]&0x3F);                                    i += 4; }
        else                                             { cp = '?';                                                           i += 1; }
        if (cp <= 0xFFFF) {
            buf[out*2]   = (uint8_t)(cp & 0xFF);
            buf[out*2+1] = (uint8_t)((cp >> 8) & 0xFF);
            out++;
        } else if (cp <= 0x10FFFF && out + 1 < maxChars - 1) {
            // Astral character: a surrogate pair, only if both halves fit.
            uint32_t v  = cp - 0x10000u;
            uint16_t hi = (uint16_t)(0xD800u + (v >> 10));
            uint16_t lo = (uint16_t)(0xDC00u + (v & 0x3FFu));
            buf[out*2]   = (uint8_t)(hi & 0xFF);
            buf[out*2+1] = (uint8_t)(hi >> 8);
            out++;
            buf[out*2]   = (uint8_t)(lo & 0xFF);
            buf[out*2+1] = (uint8_t)(lo >> 8);
            out++;
        }
    }
}

// ── Scalar WStr32 ─────────────────────────────────────────────────────────────
std::string GetWStr32(const SavFile& s, uint32_t h) {
    auto* e = Find(s,h);
    if (!e || e->type!=DT_WStr32 || e->payload.size()<64) return "";
    return Utf16LeToUtf8(e->payload.data(), 64);
}
void SetWStr32(SavFile& s, uint32_t h, const std::string& utf8) {
    auto* e = FindMut(s,h);
    if (!e || e->type!=DT_WStr32) return;
    if (e->payload.size() < 64) e->payload.resize(64, 0);
    Utf8ToUtf16Le(utf8, e->payload.data(), 32); // 32 UTF-16 chars
}

// ── Array WStr32 ──────────────────────────────────────────────────────────────
std::string GetWStr32At(const SavFile& s, uint32_t h, int idx) {
    auto* e = Find(s,h);
    if (!e || e->type!=DT_WStr32Array || e->payload.size()<4) return "";
    uint32_t cnt = R32(e->payload.data());
    if (idx<0 || (uint32_t)idx>=cnt) return "";
    size_t off = 4 + (size_t)idx * 64;
    if (off + 64 > e->payload.size()) return "";
    return Utf16LeToUtf8(e->payload.data() + off, 64);
}
void SetWStr32At(SavFile& s, uint32_t h, int idx, const std::string& utf8) {
    auto* e = FindMut(s,h);
    if (!e || e->type!=DT_WStr32Array || e->payload.size()<4) return;
    uint32_t cnt = R32(e->payload.data());
    if (idx<0 || (uint32_t)idx>=cnt) return;
    size_t off = 4 + (size_t)idx * 64;
    if (off + 64 > e->payload.size()) return;
    Utf8ToUtf16Le(utf8, e->payload.data() + off, 32);
}

// ── Array WStr64 ──────────────────────────────────────────────────────────────
std::string GetWStr64At(const SavFile& s, uint32_t h, int idx) {
    auto* e = Find(s,h);
    if (!e || e->type!=DT_WStr64Array || e->payload.size()<4) return "";
    uint32_t cnt = R32(e->payload.data());
    if (idx<0 || (uint32_t)idx>=cnt) return "";
    size_t off = 4 + (size_t)idx * 128;
    if (off + 128 > e->payload.size()) return "";
    return Utf16LeToUtf8(e->payload.data() + off, 128);
}
void SetWStr64At(SavFile& s, uint32_t h, int idx, const std::string& utf8) {
    auto* e = FindMut(s,h);
    if (!e || e->type!=DT_WStr64Array || e->payload.size()<4) return;
    uint32_t cnt = R32(e->payload.data());
    if (idx<0 || (uint32_t)idx>=cnt) return;
    size_t off = 4 + (size_t)idx * 128;
    if (off + 128 > e->payload.size()) return;
    Utf8ToUtf16Le(utf8, e->payload.data() + off, 64); // 64 UTF-16 code units incl. null
}

} // namespace SaveEditor
