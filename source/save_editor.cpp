// save_editor.cpp — TomoToolNX
// Binary .sav parser/writer. Mirrors the logic in the web save editor's
// parse.ts / write.ts / hash.ts, ported to plain C++17.

#include "save_editor.h"
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace SaveEditor {

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

static std::vector<uint8_t> ReadHeap(const std::vector<uint8_t>& b, uint32_t off, DataType t) {
    if (t == DT_Bool64Key) return {};

    size_t fixed = FixedSize(t);
    if (fixed) {
        if (off + fixed > b.size()) return {};
        return std::vector<uint8_t>(b.begin()+off, b.begin()+off+fixed);
    }

    if (off + 4 > b.size()) return {};
    uint32_t count = R32(b.data() + off);
    size_t total = 0;

    switch (t) {
        case DT_Bin:       total = 4 + count; break;
        case DT_BoolArray: {
            size_t byteSize = std::max((size_t)4, (size_t)((count + 7) / 8));
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
            size_t p = off + 4;
            for (uint32_t i = 0; i < count; i++) {
                if (p + 4 > b.size()) return {};
                uint32_t sz = R32(b.data() + p);
                p += 4 + sz;
                if (p > b.size()) return {};
            }
            return std::vector<uint8_t>(b.begin()+off, b.begin()+p);
        }
        default: return {};
    }

    if (total == 0 || off + total > b.size()) return {};
    return std::vector<uint8_t>(b.begin()+off, b.begin()+off+total);
}

// ── Load ──────────────────────────────────────────────────────────────────────
bool Load(const std::string& path, SavFile& out, std::string& err) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { err = "Cannot open: " + path; return false; }
    fseek(f, 0, SEEK_END); size_t sz = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> b(sz);
    fread(b.data(), 1, sz, f); fclose(f);

    if (sz < 0x20) { err = "File too small"; return false; }
    if (b[0]!=0x04 || b[1]!=0x03 || b[2]!=0x02 || b[3]!=0x01) { err = "Bad magic"; return false; }

    out.version = R32(b.data() + 4);
    uint32_t saveDataOff = R32(b.data() + 8);
    if (saveDataOff < 0x20 || saveDataOff > (uint32_t)sz) { err = "Bad saveDataOffset"; return false; }

    out.entries.clear();
    size_t pos = 0x20;
    DataType curType = DT_Bool;

    while (pos + 8 <= (size_t)saveDataOff) {
        uint32_t hash = R32(b.data() + pos); pos += 4;
        uint32_t slot = R32(b.data() + pos); pos += 4;
        if (hash == 0) { curType = (DataType)(slot < (uint32_t)DT_COUNT ? slot : 0); continue; }
        Entry e;
        e.hash = hash; e.type = curType;
        if (IsInline(curType)) {
            e.inlineRaw = slot;
        } else {
            e.payload = ReadHeap(b, slot, curType);
        }
        out.entries.push_back(std::move(e));
    }

    out.loaded = true;
    return true;
}

// ── Save ──────────────────────────────────────────────────────────────────────
std::string Save(const std::string& path, const SavFile& sav) {
    // Group entry indices by DataType
    std::vector<std::vector<size_t>> byType((size_t)DT_COUNT);
    for (size_t i = 0; i < sav.entries.size(); i++) {
        uint32_t t = (uint32_t)sav.entries[i].type;
        if (t < (uint32_t)DT_COUNT) byType[t].push_back(i);
    }

    // Index row count: one sentinel per type + one row per entry
    size_t rowCount = (size_t)DT_COUNT;
    for (auto& v : byType) rowCount += v.size();
    uint32_t saveDataOff = (uint32_t)(0x20 + rowCount * 8);

    // Compute heap offsets in type order (matches write.ts)
    std::vector<uint32_t> heapOff(sav.entries.size(), 0);
    uint32_t heapPos = saveDataOff;
    for (size_t t = 0; t < (size_t)DT_COUNT; t++) {
        for (size_t idx : byType[t]) {
            const auto& e = sav.entries[idx];
            if (IsInline(e.type)) continue;
            if (e.type == DT_Bool64Key && e.payload.empty()) { heapOff[idx] = 0; continue; }
            heapOff[idx] = heapPos;
            heapPos += (uint32_t)e.payload.size();
        }
    }

    std::vector<uint8_t> out(heapPos, 0);

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
        uint32_t off = heapOff[i];
        if (off + e.payload.size() <= out.size())
            memcpy(out.data() + off, e.payload.data(), e.payload.size());
    }

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return "Cannot open for write: " + path;
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return "";
}

// ── Entry lookup ──────────────────────────────────────────────────────────────
static const Entry* Find(const SavFile& s, uint32_t h) {
    for (auto& e : s.entries) if (e.hash == h) return &e;
    return nullptr;
}
static Entry* FindMut(SavFile& s, uint32_t h) {
    for (auto& e : s.entries) if (e.hash == h) return &e;
    return nullptr;
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
void SetEnumAt(SavFile& s, uint32_t h, int idx, uint32_t v) {
    ArrayWrite(FindMut(s,h), DT_EnumArray, idx, v);
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
        else                                             { cp = '?';                                                           i += 1; }
        if (cp <= 0xFFFF) {
            buf[out*2]   = (uint8_t)(cp & 0xFF);
            buf[out*2+1] = (uint8_t)((cp >> 8) & 0xFF);
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
