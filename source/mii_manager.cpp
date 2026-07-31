// mii_manager.cpp — port of ShareMii.py
// Binary I/O only, no SDL, safe on any thread.

#include "mii_manager.h"
#include "save_editor.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <string>
#include <vector>
#include <algorithm>
#include <utility>
#include <map>
#include <dirent.h>
#include <sys/stat.h>

// ─── File I/O helpers ─────────────────────────────────────────────────────────

static bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    size_t sz = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(sz);
    bool ok = fread(out.data(), 1, sz, f) == sz;
    fclose(f);
    return ok;
}

static bool WriteFile(const std::string& path, const std::vector<uint8_t>& data) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}

static bool FileExists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

static void MkdirP(const std::string& path) { mkdir(path.c_str(), 0777); }

// ─── Offset locator (port of offsetLocator()) ─────────────────────────────────
// Finds a field by its hash in the save's index section and returns the slot value
// (the 4 bytes immediately following the hash in the index entry).
// For heap-based array fields the slot is the heap offset; callers add +4 to skip the count.

static int OffsetLocator(const std::vector<uint8_t>& data, const char* hashHex) {
    if (data.size() < 8) return -1;

    uint8_t hash[4];
    for (int i = 0; i < 4; i++) {
        unsigned int v = 0;
        sscanf(hashHex + i*2, "%02x", &v);
        hash[i] = (uint8_t)v;
    }
    uint8_t le[4] = {hash[3], hash[2], hash[1], hash[0]};

    for (size_t i = 0; i + 8 <= data.size(); i++) {
        if (memcmp(data.data() + i, le, 4) == 0) {
            uint32_t offset = 0;
            memcpy(&offset, data.data() + i + 4, 4);
            return (int)offset;
        }
    }
    return -1;
}

// Same as OffsetLocator but takes the hash as a uint32 directly (avoids
// formatting to a hex string). Used by clearBelongings which iterates a
// generated table of belongings-leaf hashes.
static int OffsetLocatorU32(const std::vector<uint8_t>& data, uint32_t hash) {
    if (data.size() < 8) return -1;
    uint8_t le[4] = { (uint8_t)(hash & 0xff), (uint8_t)((hash >> 8) & 0xff),
                      (uint8_t)((hash >> 16) & 0xff), (uint8_t)((hash >> 24) & 0xff) };
    for (size_t i = 0; i + 8 <= data.size(); i++) {
        if (memcmp(data.data() + i, le, 4) == 0) {
            uint32_t offset = 0;
            memcpy(&offset, data.data() + i + 4, 4);
            return (int)offset;
        }
    }
    return -1;
}

// ─── UTF-16LE → UTF-8 (minimal, BMP only) ────────────────────────────────────

static std::string Utf16leToUtf8(const uint8_t* data, size_t maxBytes) {
    std::string out;
    for (size_t i = 0; i + 1 < maxBytes; i += 2) {
        uint16_t cp = (uint16_t)data[i] | ((uint16_t)data[i+1] << 8);
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

// Strip null bytes and find end of UTF-16LE string
static size_t Utf16leLen(const uint8_t* data, size_t maxBytes) {
    for (size_t i = 0; i + 2 <= maxBytes; i += 2) {
        if (data[i] == 0 && data[i+1] == 0) return i;
    }
    return maxBytes;
}

// ─── Sexuality bit encode/decode ──────────────────────────────────────────────

static std::vector<int> DecodeSexuality(const uint8_t* data, size_t len) {
    std::vector<int> bits;
    for (size_t b = 0; b < len; b++)
        for (int bit = 0; bit < 8; bit++)
            bits.push_back((data[b] >> bit) & 1);
    return bits;
}

static std::vector<uint8_t> EncodeSexuality(const std::vector<int>& bits) {
    std::vector<uint8_t> result;
    for (size_t i = 0; i < bits.size(); i += 8) {
        uint8_t byte = 0;
        for (int b = 0; b < 8 && i+b < bits.size(); b++)
            byte |= (bits[i+b] & 1) << b;
        result.push_back(byte);
    }
    return result;
}

// ─── Unique file path ─────────────────────────────────────────────────────────

static std::string UniquePath(std::string path) {
    if (!FileExists(path)) return path;
    std::string base = path, ext;
    size_t dot = path.rfind('.');
    if (dot != std::string::npos) { base = path.substr(0, dot); ext = path.substr(dot); }
    for (int i = 1; i < 1000; i++) {
        std::string candidate = base + "_(" + std::to_string(i) + ")" + ext;
        if (!FileExists(candidate)) return candidate;
    }
    return path;
}

// ─── Personality offset table ─────────────────────────────────────────────────

static const char* PERS_HASHES_MII[18] = {
    "43CD364F", // P1 Sociability
    "CD8DBAF8", // P2 Audaciousness
    "25B48224", // P3 Activeness
    "607BA160", // P4 Commonsense
    "68E1134E", // P5 Gaiety
    "4913AE1A", // V1 Formants
    "141EE086", // V2 Speed
    "07B9D175", // V3 Intonation
    "81CF470A", // V4 Pitch
    "4D78E262", // V5 Tension
    "FBC3FFB0", // V6 PresetType
    "236E2D73", // S1 Gender
    "F3C3DE59", // S2 PronounType
    "660C5247", // S3 ClothStyle
    "5D7D3F45", // B1 Year
    "AB8AE08B", // B2 Day
    "2545E583", // B3 DirectAge
    "6CF484F4", // B4 Month
};

// Facepaint offsets in Player.sav
static const char* FP_HASHES[5] = {
    "4C9819E4", // fpOffset1 Price
    "DECC8954", // fpOffset2 TextureSourceType
    "23135BC5", // fpOffset3 State
    "FFC750B6", // fpOffset4 Unknown
    "A56E42EC", // fpOffset5 Hash
};

// Magic section markers
static const uint8_t MAGIC_A2[4] = {0xA2,0xA2,0xA2,0xA2};
static const uint8_t MAGIC_A3[4] = {0xA3,0xA3,0xA3,0xA3};
static const uint8_t MAGIC_A4[4] = {0xA4,0xA4,0xA4,0xA4};
static const uint8_t MAGIC_A5[4] = {0xA5,0xA5,0xA5,0xA5};

// Find first occurrence of 4-byte pattern at or after `from`
static int FindMagic(const std::vector<uint8_t>& data, const uint8_t* magic, int from=0) {
    if (from < 0) from = 0;
    for (int i = from; i + 4 <= (int)data.size(); i++)
        if (memcmp(data.data()+i, magic, 4) == 0) return i;
    return -1;
}
// Same, for markers narrower than 4 bytes (the v1/v2 .ltd layouts used 3).
static int FindMagicN(const std::vector<uint8_t>& data, const uint8_t* magic,
                      int len, int from=0) {
    if (from < 0) from = 0;
    for (int i = from; i + len <= (int)data.size(); i++)
        if (memcmp(data.data()+i, magic, len) == 0) return i;
    return -1;
}
static int FindMagicLastN(const std::vector<uint8_t>& data, const uint8_t* magic, int len) {
    for (int i = (int)data.size() - len; i >= 0; i--)
        if (memcmp(data.data()+i, magic, len) == 0) return i;
    return -1;
}

// ─── .ltd Mii share-file parser ───────────────────────────────────────────────
// Port of ltd-save-editor packages/ltd-sharemii/src/codec.ts (decodeLtdMii /
// upgradeToV3).
//
// The previous inline parser scanned the entire file for the section markers —
// the FIRST A3A3A3A3 anywhere and the LAST A4A4A4A4 anywhere. Both payloads are
// compressed blobs, so a marker-shaped byte run inside one of them split the
// canvas/ugctex sections in the wrong place and wrote two corrupt .zs files into
// the save. The searches are now bounded exactly like the reference: the canvas
// marker at or after the end of the fixed-size header, and the ugctex marker at
// or after the canvas marker (first match, never last).
//
// Requiring the full 428-byte header up front also fixes the unchecked reads
// that followed: the old code copied 156 bytes from offset 4 (and personality /
// name / pronunciation from their fixed offsets) after validating nothing but
// mii[0], so a truncated upload copied heap garbage straight into Mii.sav.

static const int LTD_MII_BLOCK_LEN   = 156;
static const int LTD_PERSONALITY_LEN = 18 * 4;
static const int LTD_NAME_LEN        = 64;
static const int LTD_PRONOUNCE_LEN   = 128;
static const int LTD_SEXUALITY_LEN   = 4;

static const int LTD_OFF_MII_BLOCK   = 4;
static const int LTD_OFF_PERSONALITY = LTD_OFF_MII_BLOCK   + LTD_MII_BLOCK_LEN;   // 160
static const int LTD_OFF_NAME        = LTD_OFF_PERSONALITY + LTD_PERSONALITY_LEN; // 232
static const int LTD_OFF_PRONOUNCE   = LTD_OFF_NAME        + LTD_NAME_LEN;        // 296
static const int LTD_OFF_SEXUALITY   = LTD_OFF_PRONOUNCE   + LTD_PRONOUNCE_LEN;   // 424
static const int LTD_HEADER_LEN      = LTD_OFF_SEXUALITY   + LTD_SEXUALITY_LEN;   // 428

struct LtdMii {
    int  originalVersion;             // pre-upgrade version; v1 carries no personality
    bool hasCanvas;
    bool hasUgcTex;
    std::vector<uint8_t> buf;         // whole file, normalised to the v3 layout
    std::vector<uint8_t> canvasTex;
    std::vector<uint8_t> ugcTex;
    LtdMii() : originalVersion(0), hasCanvas(false), hasUgcTex(false) {}
};

// Rewrite a v1/v2 payload in place into the v3 layout.
static std::string UpgradeLtdToV3(std::vector<uint8_t>& mii) {
    int origVer = mii[0];
    if (mii.size() < 5) return "Invalid .ltd file (truncated header)";
    mii.erase(mii.begin() + 4);
    if (origVer == 2) {
        static const uint8_t M3[3] = {0xA3, 0xA3, 0xA3};
        if ((int)mii.size() < 427) return "Invalid .ltd file (truncated v2 payload)";
        mii.insert(mii.begin() + 427, 0);
        // v2 used 3-byte section markers; widen both to the 4-byte v3 form.
        int cm = FindMagicN(mii, M3, 3, 0);
        if (cm < 0) return "Invalid .ltd file (missing v2 canvas marker)";
        int um = FindMagicLastN(mii, M3, 3);
        if (um <= cm) return "Invalid .ltd file (missing v2 ugctex marker)";
        mii.insert(mii.begin() + cm + 3, 0xA3);
        // The insert above shifted everything from cm+3 on by one byte, so the
        // trailing marker now occupies um+1 .. um+3.
        if (um + 4 > (int)mii.size()) return "Invalid .ltd file (truncated v2 payload)";
        mii[um + 1] = 0xA4; mii[um + 2] = 0xA4; mii[um + 3] = 0xA4;
        mii.insert(mii.begin() + um + 3, 0xA4);
    }
    mii[0] = 3;
    return "";
}

static std::string DecodeLtdMii(std::vector<uint8_t> raw, LtdMii& out) {
    if (raw.size() < 4) return "Invalid .ltd file (too small)";
    int origVer = raw[0];
    if (origVer < 1 || origVer > 3)
        return "Unsupported .ltd version: " + std::to_string(origVer);
    if (origVer < 3) {
        std::string err = UpgradeLtdToV3(raw);
        if (!err.empty()) return err;
    }
    if ((int)raw.size() < LTD_HEADER_LEN)
        return "Invalid .ltd file (truncated: " + std::to_string(raw.size())
             + " bytes, expected at least " + std::to_string(LTD_HEADER_LEN) + ")";

    int canvasMagic = FindMagic(raw, MAGIC_A3, LTD_HEADER_LEN);
    if (canvasMagic < 0) return "Invalid .ltd file (missing canvas section marker)";
    int ugcMagic = FindMagic(raw, MAGIC_A4, canvasMagic + 4);
    if (ugcMagic < 0) return "Invalid .ltd file (missing ugctex section marker)";

    out.originalVersion = origVer;
    out.hasCanvas = raw[1] != 0;
    out.hasUgcTex = raw[2] != 0;
    out.canvasTex.assign(raw.begin() + canvasMagic + 4, raw.begin() + ugcMagic);
    out.ugcTex.assign(raw.begin() + ugcMagic + 4, raw.end());
    out.buf.swap(raw);
    return "";
}

// Load facepaint textures that ship next to a .ltd instead of inside it.
// Two naming conventions are accepted:
//   1. "<stem>.canvas.zs" / "<stem>.ugctex.zs" matching the .ltd's own stem —
//      what TomoToolNX's own exporter and the WebUI upload path produce.
//   2. "UgcFacePaintNNN.canvas.zs" / "UgcFacePaintNNN.ugctex.zs" — what
//      ltd-save-editor's ShareMii export writes, named after the *source*
//      save's facepaint id, which the .ltd itself never records. Those can only
//      be paired positionally, so this is accepted only when the directory
//      holds exactly one such pair and exactly one .ltd — otherwise we would be
//      guessing which Mii the textures belong to.
// Before this, convention 2 was not recognised at all: importing a Mii exported
// from the web editor found no facepaint and *cleared* the target slot's
// existing one.
static bool EndsWithLower(const std::string& low, const char* suffix) {
    size_t n = strlen(suffix);
    return low.size() > n && low.compare(low.size() - n, n, suffix) == 0;
}

static bool LoadFacepaintSidecar(const std::string& ltdPath,
                                 std::vector<uint8_t>& canvas,
                                 std::vector<uint8_t>& ugctex) {
    auto tryPair = [&](const std::string& stem) -> bool {
        canvas.clear(); ugctex.clear();
        if (!ReadFile(stem + ".canvas.zs", canvas)) return false;
        if (!ReadFile(stem + ".ugctex.zs", ugctex)) { canvas.clear(); return false; }
        if (canvas.empty() || ugctex.empty()) { canvas.clear(); ugctex.clear(); return false; }
        return true;
    };

    std::string base = ltdPath;
    if (base.size() >= 4 && base.compare(base.size() - 4, 4, ".ltd") == 0)
        base = base.substr(0, base.size() - 4);
    if (tryPair(base)) return true;

    size_t slash = ltdPath.find_last_of("/\\");
    std::string dir = (slash == std::string::npos) ? std::string(".")
                                                   : ltdPath.substr(0, slash);
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    std::string fpStem;
    int fpPairs = 0, ltdFiles = 0;
    for (struct dirent* e = readdir(d); e != nullptr; e = readdir(d)) {
        std::string name = e->d_name;
        std::string low  = name;
        for (auto& c : low) c = (char)tolower((unsigned char)c);
        if (EndsWithLower(low, ".ltd")) { ltdFiles++; continue; }
        if (EndsWithLower(low, ".canvas.zs") &&
            low.size() > 12 && low.compare(0, 12, "ugcfacepaint") == 0) {
            fpPairs++;
            fpStem = name.substr(0, name.size() - 10); // strip ".canvas.zs"
        }
    }
    closedir(d);
    if (fpPairs != 1 || ltdFiles != 1) return false;
    return tryPair(dir + "/" + fpStem);
}

// ─── UGC kind tables (mirrors ShareUGCTemplate.py) ───────────────────────────

static const char* UGC_EXT[7]  = {".ltdf",".ltdc",".ltdg",".ltdi",".ltde",".ltdo",".ltdl"};
static const char* UGC_PFX[7]  = {"Food","Cloth","Goods","Interior","Exterior","MapObject","MapFloor"};
static const int   UGC_NIGHT_IDX[7]  = {5, 6, 13, 4, 7, 9, 4};
static const int   UGC_NIGHT_SZ[7]   = {13,38, 13,13,13,13,13};
static const int   UGC_MAX_SL[7]     = {99,299,99, 99,99, 99, 99};

static const char* UGC_ENABLE_H[7] = {"F4A39965","AF129C33","1A9C00FE","A39744E9","F4BEADC2","5951050B","A1126D32"};
static const char* UGC_TEX_H[7]    = {"3558B77F","59BFA9D3","70D10A48","E7F9D439","16227C50","A9C5CFB8","06A7A14C"};
static const char* UGC_HASH_H[7]   = {"6D48F8E2","89F25CAC","56202100","7FEF7F7D","38D72795","1B28B170","816D50A3"};
static const uint8_t UGC_HASH_IDX[7] = {1,3,2,6,7,4,5};
static const uint8_t UGC_TEX_DATA[28] = {
    0x41,0x49,0x93,0x56, 0xE3,0xC2,0x2F,0xB4,
    0x41,0x49,0x93,0x56, 0xE3,0xC2,0x2F,0xB4,
    0xE3,0xC2,0x2F,0xB4, 0xE3,0xC2,0x2F,0xB4,
    0xE3,0xC2,0x2F,0xB4,
};

// Per-kind ugcOffset hashes
static const char* UGC_F_H0[10] = {"307FEEFA","6F93FFBD","5CA9336E","F768620A","5AF04BEB","2DB168C5","634800AE","DD8D6C5A","AF1186CF","58E6AAD3"};
static const char* UGC_F_H1[10] = {"C81545FE","2FB9146D","7A31EF97","7EEC35E9","5E32FD3F","0DBABE27","71621C98","2D271339","CDF31EB5","2823DBD3"};
static const char* UGC_F_H2[17] = {"3FAA2222","823F8297","7ECC8A60","88DC1D43","8896DDD6","BFF29472","5D965762","78D39208","53C762B0","40D2C6FE","C0A6C046","AE373B0D","7D5FFBB7","9E978F5E","F6349929","9038CDD0","9A59F58A"};
static const char* UGC_F_H3[8]  = {"A9116402","835114C1","EC65E2E4","0A7CF2C5","662CD807","01B3661E","5AF4A09F","41FF2201"};
static const char* UGC_F_H4[11] = {"ED95CF0F","43F509BA","A7A0773C","A7A0773C","34BA6119","5E6E9F8C","2907C040","97865D6B","609F197D","47A50525","71EA7734"};
static const char* UGC_F_H5[13] = {"274659D1","DCE826FC","E04E1E6B","056F2F20","BC7D7E30","3C2BC52F","CFFECCC2","5C15E339","5EFF5E0E","9838264B","48778DE6","62AD5137","D1B3B197"};
static const char* UGC_F_H6[8]  = {"21D582D9","DE7CB924","E8BD8C89","C35B8B0F","60E280FB","7EC3836A","F209E2F9","6D842ACC"};
static const int   UGC_NUM_F[7] = {10,10,17,8,11,13,8};
static const char** UGC_F_H[7]  = {(const char**)UGC_F_H0,(const char**)UGC_F_H1,(const char**)UGC_F_H2,(const char**)UGC_F_H3,(const char**)UGC_F_H4,(const char**)UGC_F_H5,(const char**)UGC_F_H6};

// Per-kind name hashes
static const char* UGC_N_H0[2] = {"408494F5","BA0F4BAF"};
static const char* UGC_N_H1[2] = {"40710642","CF9A13EA"};
static const char* UGC_N_H2[4] = {"2F793EB1","F655B33A","F36A5A0B","A66367EB"};
static const char* UGC_N_H3[2] = {"3DE2C5DD","85A37B90"};
static const char* UGC_N_H4[2] = {"27C875D6","0E15E3F8"};
static const char* UGC_N_H5[2] = {"56F99338","EE921AE2"};
static const char* UGC_N_H6[2] = {"918875A9","503490E0"};
static const int   UGC_NUM_N[7] = {2,2,4,2,2,2,2};
// bytes-per-slot for each name field (name[0..3])
static const int   UGC_N_SZ[7][4] = {
    {128,128,  0,  0}, // Food
    {128,128,  0,  0}, // Cloth
    {128,128, 64,128}, // Goods (extra GoodsText+HowToCallGoodsText)
    {128,128,  0,  0}, // Interior
    {128,128,  0,  0}, // Exterior
    {128,128,  0,  0}, // MapObject
    {128,128,  0,  0}, // MapFloor
};
static const char** UGC_N_H[7] = {(const char**)UGC_N_H0,(const char**)UGC_N_H1,(const char**)UGC_N_H2,(const char**)UGC_N_H3,(const char**)UGC_N_H4,(const char**)UGC_N_H5,(const char**)UGC_N_H6};

// vOffset/v2Offset hashes (nullptr = not used for this kind)
static const char* UGC_V_H[7]  = {nullptr,nullptr,"F36C4E28",nullptr,"3C14025E","27F2ECDE",nullptr};
static const char* UGC_V2_H[7] = {nullptr,nullptr,nullptr,   nullptr,"B9D21B4F","2F96203B",nullptr};

struct UgcOffsets {
    int f[17]; // ugcOffsets: per-slot field data (absolute byte pos in Player.sav, already +4)
    int n[4];  // name offsets (absolute byte pos)
    int v;     // vOffset (-1 if unused)
    int v2;    // v2Offset (-1 if unused)
    int enable, tex, hash; // slot-enable / texture-kind / hash-index arrays
};

static bool BuildUgcOffsets(const std::vector<uint8_t>& psav, int kind, UgcOffsets& out) {
    for (int i = 0; i < UGC_NUM_F[kind]; i++) {
        out.f[i] = OffsetLocator(psav, UGC_F_H[kind][i]);
        if (out.f[i] < 0) return false;
        out.f[i] += 4;
    }
    for (int i = 0; i < UGC_NUM_N[kind]; i++) {
        out.n[i] = OffsetLocator(psav, UGC_N_H[kind][i]);
        if (out.n[i] < 0) return false;
        out.n[i] += 4;
    }
    auto locate = [&](const char* h) -> int {
        if (!h) return -1;
        int r = OffsetLocator(psav, h);
        return r >= 0 ? r + 4 : -1;
    };
    out.v      = locate(UGC_V_H[kind]);
    out.v2     = locate(UGC_V2_H[kind]);
    out.enable = locate(UGC_ENABLE_H[kind]);
    out.tex    = locate(UGC_TEX_H[kind]);
    out.hash   = locate(UGC_HASH_H[kind]);
    return true;
}

// ─── Public API ───────────────────────────────────────────────────────────────

namespace MiiManager {

std::vector<MiiSlot> ListMiis() {
    std::vector<MiiSlot> result;
    std::vector<uint8_t> miisav;
    if (!ReadFile(SAVE_MII_SAV, miisav)) return result;

    int names_hash = OffsetLocator(miisav, "2499BFDA");
    int data_hash  = OffsetLocator(miisav, "881CA27A");
    int fp_hash    = OffsetLocator(miisav, "5E32ADF4");
    if (names_hash < 0 || data_hash < 0) return result;

    int miinames = names_hash + 4;
    int miidata  = data_hash  + 4;
    int miiOffset2 = fp_hash >= 0 ? fp_hash + 4 : -1;

    for (int x = 0; x < MII_MAX_SLOTS; x++) {
        int miiindex = miidata + 156 * x;
        if (miiindex + 156 > (int)miisav.size()) break;

        // Check if slot is initialized (empty slot sums to 152)
        int sum = 0;
        for (int i = 0; i < 156; i++) sum += miisav[miiindex + i];

        MiiSlot s;
        s.slot  = x + 1;
        s.isEmpty = (sum == 152);

        if (!s.isEmpty) {
            const uint8_t* namePtr = miisav.data() + miinames + x * 64;
            size_t nameLen = Utf16leLen(namePtr, 64);
            // Empty name → treat as an empty slot. The sum-based check
            // above misses freshly-deleted slots because it strides 156
            // bytes (4 of which belong to the next field and stay
            // non-zero across a delete). Empty name is the reliable
            // signal: in-game roster lookups also gate on it.
            if (nameLen == 0) continue;
            s.name = Utf16leToUtf8(namePtr, nameLen);
            s.hasFacepaint = (miiOffset2 >= 0 &&
                              miiOffset2 + 4*x < (int)miisav.size() &&
                              miisav[miiOffset2 + 4*x] != 0xFF);
            result.push_back(s);
        }
    }
    return result;
}

std::string ExportMii(int slot, const std::string& destPath) {
    if (slot < 1 || slot > MII_MAX_SLOTS)
        return "Invalid slot (must be 1-" + std::to_string(MII_MAX_SLOTS) + ")";

    std::vector<uint8_t> miisav, playersav;
    if (!ReadFile(SAVE_MII_SAV,    miisav))    return "Cannot read Mii.sav";
    if (!ReadFile(SAVE_PLAYER_SAV, playersav)) return "Cannot read Player.sav";

    int names_hash  = OffsetLocator(miisav, "2499BFDA");
    int data_hash   = OffsetLocator(miisav, "881CA27A");
    int prefer_hash = OffsetLocator(miisav, "3A5EDA05");
    int fp2_hash    = OffsetLocator(miisav, "5E32ADF4");
    int sxHash      = OffsetLocator(miisav, "DFC82223");
    if (names_hash < 0 || data_hash < 0 || prefer_hash < 0)
        return "Could not locate Mii data offsets";

    int miinames   = names_hash  + 4;
    int miidata    = data_hash   + 4;
    int miiprefer  = prefer_hash + 4;
    int miiOffset2 = fp2_hash >= 0 ? fp2_hash + 4 : -1;
    int persOffsetSX = sxHash >= 0 ? sxHash + 4 : -1;

    int s = slot - 1; // 0-based
    int miiindex = miidata + 156 * s;
    if (miiindex + 156 > (int)miisav.size())
        return "Slot out of bounds";

    // Check slot is initialized
    int sum = 0;
    for (int i = 0; i < 156; i++) sum += miisav[miiindex + i];
    if (sum == 152) return "Mii slot is empty";

    // Personality
    std::vector<uint8_t> personality;
    for (int i = 0; i < 18; i++) {
        int off = OffsetLocator(miisav, PERS_HASHES_MII[i]);
        if (off < 0) { personality.insert(personality.end(), 4, 0); continue; }
        off += 4 + s * 4;
        for (int b = 0; b < 4; b++)
            personality.push_back(off + b < (int)miisav.size() ? miisav[off+b] : 0);
    }

    // Name (64 bytes) + Pronunciation (128 bytes)
    std::vector<uint8_t> name(miisav.begin() + miinames + s*64,
                               miisav.begin() + miinames + s*64 + 64);
    std::vector<uint8_t> pronounce(miisav.begin() + miiprefer + s*128,
                                    miisav.begin() + miiprefer + s*128 + 128);

    // Sexuality (3 bits per slot, stored packed across 27 bytes)
    std::vector<uint8_t> sexualityBytes(3, 0);
    if (persOffsetSX >= 0 && persOffsetSX + 27 <= (int)miisav.size()) {
        auto bits = DecodeSexuality(miisav.data() + persOffsetSX, 27);
        for (int b = 0; b < 3 && s*3+b < (int)bits.size(); b++)
            sexualityBytes[b] = (uint8_t)bits[s*3+b];
    }
    sexualityBytes.push_back(0); // padding

    // Facepaint
    bool hasFacepaint = false;
    int facepaintID = -1;
    std::vector<uint8_t> canvastex, ugctex;
    uint8_t ltdData[3] = {0,0,0};

    if (miiOffset2 >= 0 && miiOffset2 + 4*s < (int)miisav.size()) {
        uint8_t fpID = miisav[miiOffset2 + 4*s];
        if (fpID != 0xFF) {
            hasFacepaint = true;
            facepaintID  = fpID;
        }
    }

    if (hasFacepaint) {
        char fpFile[8];
        snprintf(fpFile, sizeof(fpFile), "%03d", facepaintID);
        std::string canvasPath = std::string(SAVE_UGC_DIR) + "/UgcFacePaint" + fpFile + ".canvas.zs";
        std::string ugctexPath = std::string(SAVE_UGC_DIR) + "/UgcFacePaint" + fpFile + ".ugctex.zs";
        // Both blobs must actually have content. ReadFile() succeeds on a
        // zero-byte file, and flagging an empty payload as present made the
        // importer commit facepaint state with no texture to back it.
        if (!ReadFile(canvasPath, canvastex)) canvastex.clear();
        if (!ReadFile(ugctexPath, ugctex))    ugctex.clear();
        if (canvastex.empty() || ugctex.empty()) {
            canvastex.clear();
            ugctex.clear();
        } else {
            ltdData[0] = 1;
            ltdData[1] = 1;
        }
    }

    // Build .ltd file
    // Format: [version=3][ltdData(3)][rawMii(156)][personality(72)][name(64)][pronounce(128)][sexuality(4)][A3A3A3A3][canvas][A4A4A4A4][ugctex]
    std::vector<uint8_t> output;
    output.push_back(MII_LTD_VERSION);
    output.insert(output.end(), ltdData, ltdData+3);
    output.insert(output.end(), miisav.begin()+miiindex, miisav.begin()+miiindex+156);
    output.insert(output.end(), personality.begin(), personality.end());
    output.insert(output.end(), name.begin(), name.end());
    output.insert(output.end(), pronounce.begin(), pronounce.end());
    output.insert(output.end(), sexualityBytes.begin(), sexualityBytes.end());
    output.insert(output.end(), MAGIC_A3, MAGIC_A3+4);
    output.insert(output.end(), canvastex.begin(), canvastex.end());
    output.insert(output.end(), MAGIC_A4, MAGIC_A4+4);
    output.insert(output.end(), ugctex.begin(), ugctex.end());

    // Build final path
    std::string finalPath = destPath;
    if (finalPath.size() < 4 || finalPath.substr(finalPath.size()-4) != ".ltd")
        finalPath += ".ltd";
    finalPath = UniquePath(finalPath);

    if (!WriteFile(finalPath, output))
        return "Failed to write " + finalPath;

    return ""; // success
}

std::string ImportMii(int slot, const std::string& ltdPath) {
    if (slot < 1 || slot > MII_MAX_SLOTS)
        return "Invalid slot (must be 1-" + std::to_string(MII_MAX_SLOTS) + ")";

    // Read + fully validate the .ltd before anything else. DecodeLtdMii
    // guarantees a v3 layout with at least LTD_HEADER_LEN bytes, so every
    // fixed-offset read below is in bounds.
    std::vector<uint8_t> rawLtd;
    if (!ReadFile(ltdPath, rawLtd)) return "Cannot read .ltd file";

    LtdMii ltd;
    {
        std::string err = DecodeLtdMii(std::move(rawLtd), ltd);
        if (!err.empty()) return err;
    }
    const std::vector<uint8_t>& mii = ltd.buf;

    // Read save files
    std::vector<uint8_t> miisav, playersav;
    if (!ReadFile(SAVE_MII_SAV,    miisav))    return "Cannot read Mii.sav";
    if (!ReadFile(SAVE_PLAYER_SAV, playersav)) return "Cannot read Player.sav";

    // Locate offsets
    int names_hash  = OffsetLocator(miisav, "2499BFDA");
    int data_hash   = OffsetLocator(miisav, "881CA27A");
    int prefer_hash = OffsetLocator(miisav, "3A5EDA05");
    int fp2_hash    = OffsetLocator(miisav, "5E32ADF4");
    int sxHash      = OffsetLocator(miisav, "DFC82223");
    if (names_hash < 0 || data_hash < 0 || prefer_hash < 0)
        return "Could not locate Mii data offsets in Mii.sav";

    // Player.sav facepaint offsets
    int fpOff[5];
    for (int i = 0; i < 5; i++) {
        fpOff[i] = OffsetLocator(playersav, FP_HASHES[i]);
        if (fpOff[i] < 0) return std::string("Missing facepaint offset: ") + FP_HASHES[i];
        fpOff[i] += 4;
    }

    int miinames   = names_hash  + 4;
    int miidata    = data_hash   + 4;
    int miiprefer  = prefer_hash + 4;
    int miiOffset2 = fp2_hash >= 0 ? fp2_hash + 4 : -1;
    int persOffsetSX = sxHash >= 0 ? sxHash + 4 : -1;

    int s = slot - 1; // 0-based
    int miiindex = miidata + 156 * s;
    if (miiindex + 156 > (int)miisav.size())
        return "Slot out of bounds";

    // Check slot is initialized
    int sum = 0;
    for (int i = 0; i < 156; i++) sum += miisav[miiindex + i];
    if (sum == 152) return "Mii slot not initialized — create a Mii there first";

    // ── Resolve the facepaint payload BEFORE touching any save state ─────────
    // Both blobs must exist and be non-empty. The old code committed the
    // Mii.sav facepaint index and the Player.sav slot state first and wrote the
    // .zs files afterwards under separate conditions, so an empty payload or a
    // failed write left the resident pointing at a facepaint id whose textures
    // still belonged to a *previous* resident — one Mii wearing another Mii's
    // makeup. Nothing below mutates the saves until both blobs are in hand.
    std::vector<uint8_t> fpCanvas, fpUgcTex;
    bool haveFacepaint = false;
    if (ltd.hasCanvas && ltd.hasUgcTex &&
        !ltd.canvasTex.empty() && !ltd.ugcTex.empty()) {
        fpCanvas = ltd.canvasTex;
        fpUgcTex = ltd.ugcTex;
        haveFacepaint = true;
    }
    if (!haveFacepaint)
        haveFacepaint = LoadFacepaintSidecar(ltdPath, fpCanvas, fpUgcTex);

    // Write raw Mii data to Mii.sav. NOTE: the .ltd block is copied verbatim.
    // A previous version poked mii[47] = 1 here whenever the Mii carried a
    // facepaint; that byte is CharInfoEx offset 39 — the Mii's *gender* — so
    // every facepainted Mii was silently turned female on import. The game
    // learns about facepaint from Mii.FacePaintIndex, which is set below.
    memcpy(miisav.data() + miiindex, mii.data() + LTD_OFF_MII_BLOCK, LTD_MII_BLOCK_LEN);

    // Personality / name / pronunciation / sexuality (v2+ only)
    if (ltd.originalVersion >= 2) {
        for (int i = 0; i < 18; i++) {
            int off = OffsetLocator(miisav, PERS_HASHES_MII[i]);
            if (off < 0) continue;
            off += 4 + s * 4;
            if (off + 4 <= (int)miisav.size())
                memcpy(miisav.data() + off, mii.data() + LTD_OFF_PERSONALITY + i*4, 4);
        }
        if (miinames + s*64 + LTD_NAME_LEN <= (int)miisav.size())
            memcpy(miisav.data() + miinames + s*64, mii.data() + LTD_OFF_NAME, LTD_NAME_LEN);
        if (miiprefer + s*128 + LTD_PRONOUNCE_LEN <= (int)miisav.size())
            memcpy(miisav.data() + miiprefer + s*128, mii.data() + LTD_OFF_PRONOUNCE, LTD_PRONOUNCE_LEN);
        // Sexuality — only if Mii is level < 2
        int levelHash = OffsetLocator(miisav, "9999B7D9");
        bool canSetSexuality = true;
        if (levelHash >= 0) {
            int levelOff = levelHash + 4 + s * 4;
            if (levelOff + 4 <= (int)miisav.size()) {
                uint32_t level = 0;
                memcpy(&level, miisav.data() + levelOff, 4);
                canSetSexuality = (level < 2);
            }
        }
        if (canSetSexuality && persOffsetSX >= 0 && persOffsetSX + 27 <= (int)miisav.size()) {
            auto bits = DecodeSexuality(miisav.data() + persOffsetSX, 27);
            for (int b = 0; b < 3; b++)
                if (s*3+b < (int)bits.size())
                    bits[s*3+b] = mii[LTD_OFF_SEXUALITY + b] & 1;
            auto encoded = EncodeSexuality(bits);
            memcpy(miisav.data() + persOffsetSX, encoded.data(), std::min(encoded.size(), (size_t)27));
        }
    }

    // Read existing facepaint ID for this slot (0xFF = none)
    int facepaintID = -1;
    if (miiOffset2 >= 0 && miiOffset2 + 4*s < (int)miisav.size()) {
        uint8_t rawId = miisav[miiOffset2 + 4*s];
        if (rawId != 0xFF) facepaintID = rawId;
    }

    // Facepaint handling. `staleFpStem` records textures to delete once the
    // saves have actually been committed — removing them earlier would strand
    // a still-referenced facepaint if a .sav write then failed.
    std::string staleFpStem;
    if (haveFacepaint) {
        // Assign next available ID if this slot had no facepaint
        if (facepaintID < 0) {
            std::vector<bool> used(MII_MAX_SLOTS, false);
            for (int x = 0; x < MII_MAX_SLOTS; x++) {
                if (miiOffset2 >= 0 && miiOffset2 + 4*x < (int)miisav.size()) {
                    uint8_t id = miisav[miiOffset2 + 4*x];
                    if (id != 0xFF && id < MII_MAX_SLOTS) used[id] = true;
                }
            }
            for (int id = 0; id < MII_MAX_SLOTS; id++) {
                if (!used[id]) { facepaintID = id; break; }
            }
        }
        if (facepaintID < 0) return "No free facepaint slots";

        // Write the textures FIRST. If either write fails we return before
        // Mii.sav / Player.sav are modified at all, so the save on disk stays
        // exactly as it was rather than referencing a facepaint that isn't there.
        char fpFile[8];
        snprintf(fpFile, sizeof(fpFile), "%03d", facepaintID);
        MkdirP(SAVE_UGC_DIR);
        std::string canvasPath = std::string(SAVE_UGC_DIR) + "/UgcFacePaint" + fpFile + ".canvas.zs";
        std::string ugctexPath = std::string(SAVE_UGC_DIR) + "/UgcFacePaint" + fpFile + ".ugctex.zs";
        if (!WriteFile(canvasPath, fpCanvas)) return "Failed to write " + canvasPath;
        if (!WriteFile(ugctexPath, fpUgcTex)) return "Failed to write " + ugctexPath;

        // Update Mii.sav facepaint index
        if (miiOffset2 >= 0 && miiOffset2 + 4*s + 4 <= (int)miisav.size()) {
            miisav[miiOffset2 + 4*s]   = (uint8_t)facepaintID;
            miisav[miiOffset2 + 4*s+1] = 0;
            miisav[miiOffset2 + 4*s+2] = 0;
            miisav[miiOffset2 + 4*s+3] = 0;
        }

        // Update Player.sav facepaint state
        static const uint8_t FP1VAL[4] = {0xF4,0x01,0x00,0x00};
        static const uint8_t FP2VAL[4] = {0x41,0x49,0x93,0x56};
        static const uint8_t FP3VAL[4] = {0xF4,0xAD,0x7F,0x1D};
        static const uint8_t FP4VAL[4] = {0x00,0x80,0x00,0x00};
        if (fpOff[0] + facepaintID*4 + 4 <= (int)playersav.size())
            memcpy(playersav.data() + fpOff[0] + facepaintID*4, FP1VAL, 4);
        if (fpOff[1] + facepaintID*4 + 4 <= (int)playersav.size())
            memcpy(playersav.data() + fpOff[1] + facepaintID*4, FP2VAL, 4);
        if (fpOff[2] + facepaintID*4 + 4 <= (int)playersav.size())
            memcpy(playersav.data() + fpOff[2] + facepaintID*4, FP3VAL, 4);
        if (fpOff[3] + facepaintID*4 + 4 <= (int)playersav.size())
            memcpy(playersav.data() + fpOff[3] + facepaintID*4, FP4VAL, 4);
        uint8_t fp5val[4] = {(uint8_t)facepaintID, 0, 8, 0};
        if (fpOff[4] + facepaintID*4 + 4 <= (int)playersav.size())
            memcpy(playersav.data() + fpOff[4] + facepaintID*4, fp5val, 4);
    } else {
        // New Mii has no facepaint — clear old facepaint data if slot previously had one
        if (facepaintID >= 0) {
            if (miiOffset2 >= 0 && miiOffset2 + 4*s + 4 <= (int)miisav.size()) {
                miisav[miiOffset2 + 4*s]   = 0xFF;
                miisav[miiOffset2 + 4*s+1] = 0xFF;
                miisav[miiOffset2 + 4*s+2] = 0xFF;
                miisav[miiOffset2 + 4*s+3] = 0xFF;
            }
            static const uint8_t CLR1[4] = {0x00,0x00,0x00,0x00};
            static const uint8_t CLR2[4] = {0x09,0xDE,0xEE,0xB6};
            static const uint8_t CLR3[4] = {0xA5,0x8A,0xFF,0xAF};
            static const uint8_t CLR4[4] = {0x00,0x00,0x00,0x00};
            static const uint8_t CLR5[4] = {0x00,0x00,0x00,0x00};
            if (fpOff[0] + facepaintID*4 + 4 <= (int)playersav.size())
                memcpy(playersav.data() + fpOff[0] + facepaintID*4, CLR1, 4);
            if (fpOff[1] + facepaintID*4 + 4 <= (int)playersav.size())
                memcpy(playersav.data() + fpOff[1] + facepaintID*4, CLR2, 4);
            if (fpOff[2] + facepaintID*4 + 4 <= (int)playersav.size())
                memcpy(playersav.data() + fpOff[2] + facepaintID*4, CLR3, 4);
            if (fpOff[3] + facepaintID*4 + 4 <= (int)playersav.size())
                memcpy(playersav.data() + fpOff[3] + facepaintID*4, CLR4, 4);
            if (fpOff[4] + facepaintID*4 + 4 <= (int)playersav.size())
                memcpy(playersav.data() + fpOff[4] + facepaintID*4, CLR5, 4);

            // Drop the textures too. Leaving them behind meant the next Mii to
            // be allocated this now-"free" id inherited the old resident's
            // artwork whenever its own texture write didn't land.
            char fpFile[8];
            snprintf(fpFile, sizeof(fpFile), "%03d", facepaintID);
            staleFpStem = std::string(SAVE_UGC_DIR) + "/UgcFacePaint" + fpFile;
        }
    }

    // Reset UgcInfo flags so the imported Mii can be sent via local network.
    //   IsCopy[slot]              → false  (treat as native mii, not a received copy)
    //   IsEdit[slot]              → false  (clean state)
    //   NetworkDeliverCount[slot] → 0      (reset per-mii delivery counter)
    {
        // BoolArray patcher: skip 4-byte count, then bit-clear bit at index s.
        auto clearBoolAt = [&](const char* hashHex) {
            int o = OffsetLocator(miisav, hashHex);
            if (o < 0) return;
            size_t off = (size_t)o + 4 + (size_t)(s >> 3);
            if (off < miisav.size()) miisav[off] &= (uint8_t)~(1u << (s & 7));
        };
        clearBoolAt("DBDB59FA"); // Mii.MiiMisc.UgcInfo.IsCopy
        clearBoolAt("7F9B5D34"); // Mii.MiiMisc.UgcInfo.IsEdit

        // UIntArray patcher: skip 4-byte count, write zero at slot index.
        int ndcOff = OffsetLocator(miisav, "62AAAA12"); // Mii.MiiMisc.UgcInfo.NetworkDeliverCount
        if (ndcOff >= 0) {
            size_t off = (size_t)ndcOff + 4 + (size_t)s * 4;
            if (off + 4 <= miisav.size()) {
                miisav[off]   = 0; miisav[off+1] = 0;
                miisav[off+2] = 0; miisav[off+3] = 0;
            }
        }
    }

    // Write save files
    if (!WriteFile(SAVE_MII_SAV,    miisav))    return "Failed to write Mii.sav";
    if (!WriteFile(SAVE_PLAYER_SAV, playersav)) return "Failed to write Player.sav";

    if (!staleFpStem.empty()) {
        remove((staleFpStem + ".canvas.zs").c_str());
        remove((staleFpStem + ".ugctex.zs").c_str());
    }

    return ""; // success
}

// ─── UGC name cache ───────────────────────────────────────────────────────────

// Fields per UGC kind:
//   prefix    — stem prefix (Ugc{prefix}NNN)
//   hashHex   — name array hash (WStr64Array)
//   maxSlots  — capacity
//   enableHex — enum array hash for the "slot enabled" flag; must be
//               set to ENABLE_USED (0x1d7fadf4) so the game shows the item.
//   textureHex— enum array hash for the texture-source identifier.
//   hashIdHex — UInt array hash for the per-slot content-id, packed as
//               (slotIdx & 0xff) | ((kindTag & 0xff) << 16).
//   textureValue — value written into the texture-source array for
//               newly-created blank slots (from 2.13.0's UGC_TEX_DATA).
//   kindTag   — high byte of the hashId (from 2.13.0's UGC_HASH_INDICES).
// Values ported from ltd-save-editor-2.13.0/packages/ltd-sharemii/
// src/ugcKinds.ts — without setting all three of enable/texture/hashId
// the game ignores the new slot entirely (name-only writes get treated
// as leftover garbage and don't render in the shop / catalog UI).
static const struct {
    const char* prefix;
    const char* hashHex;
    int         maxSlots;
    const char* enableHex;
    const char* textureHex;
    const char* hashIdHex;
    uint32_t    textureValue;
    uint32_t    kindTag;
} UGC_KIND_DATA[] = {
    {"Food",      "408494F5", 99,  "F4A39965", "3558B77F", "6D48F8E2", 0x56934941u, 1u},
    {"Cloth",     "40710642", 299, "AF129C33", "59BFA9D3", "89F25CAC", 0xb42fc2e3u, 3u},
    {"Goods",     "2F793EB1", 99,  "1A9C00FE", "70D10A48", "56202100", 0x56934941u, 2u},
    {"Interior",  "3DE2C5DD", 99,  "A39744E9", "E7F9D439", "7FEF7F7D", 0xb42fc2e3u, 6u},
    {"Exterior",  "27C875D6", 99,  "F4BEADC2", "16227C50", "38D72795", 0xb42fc2e3u, 7u},
    {"MapObject", "56F99338", 99,  "5951050B", "A9C5CFB8", "1B28B170", 0xb42fc2e3u, 4u},
    {"MapFloor",  "918875A9", 99,  "A1126D32", "06A7A14C", "816D50A3", 0xb42fc2e3u, 5u},
};
static const int UGC_KIND_COUNT = 7;
// Enum value the game writes to the "slot enabled" field for an active
// UGC slot. Blank creation sets it to make the slot visible.
static const uint32_t UGC_ENABLE_USED = 0x1d7fadf4u;

static std::map<std::string, std::string> s_ugcNames; // "UgcFood003" → UTF-8 name

void LoadUgcNames() {
    s_ugcNames.clear();

    std::vector<uint8_t> playersav;
    if (!ReadFile(SAVE_PLAYER_SAV, playersav)) return;

    for (int k = 0; k < UGC_KIND_COUNT; k++) {
        const auto& ki = UGC_KIND_DATA[k];
        int offset = OffsetLocator(playersav, ki.hashHex);
        if (offset < 0) continue;

        // Payload layout: 4-byte array header, then maxSlots * 128 bytes (UTF-16LE name each)
        int base = offset + 4;
        for (int i = 0; i < ki.maxSlots; i++) {
            int nameOff = base + i * 128;
            if (nameOff + 128 > (int)playersav.size()) break;

            std::string name = Utf16leToUtf8(playersav.data() + nameOff, 128);
            if (name.empty()) continue;

            char stem[32];
            snprintf(stem, sizeof(stem), "Ugc%s%03d", ki.prefix, i);
            s_ugcNames[stem] = name;
        }
    }
}

static std::string FormatUgcStem(const std::string& stem) {
    static const struct { const char* key; const char* label; } MAP[] = {
        {"ugcfood",      "Food"},
        {"ugccloth",     "Cloth"},
        {"ugcgoods",     "Goods"},
        {"ugcinterior",  "Interior"},
        {"ugcexterior",  "Exterior"},
        {"ugcmapobject", "Map Object"},
        {"ugcmapfloor",  "Map Floor"},
        {"ugcfacepaint", "Face Paint"},
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

std::string GetUgcName(const std::string& stem) {
    auto it = s_ugcNames.find(stem);
    if (it != s_ugcNames.end()) return it->second;
    return FormatUgcStem(stem);
}

// Re-encode `utf8` into a fixed-size UTF-16LE buffer. The buffer is zero-filled
// first, so unused tail bytes act as the terminating null pair the game
// expects. Anything past `maxCodeUnits-1` is truncated.
static void WriteUtf16LeFixed(const std::string& utf8, uint8_t* buf,
                              size_t maxCodeUnits) {
    memset(buf, 0, maxCodeUnits * 2);
    if (maxCodeUnits == 0) return;
    size_t out = 0;
    for (size_t i = 0; i < utf8.size() && out + 1 < maxCodeUnits; ) {
        uint8_t c = (uint8_t)utf8[i];
        uint32_t cp;
        if (c < 0x80) {
            cp = c; i += 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
            cp = ((c & 0x1F) << 6) | ((uint8_t)utf8[i+1] & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
            cp = ((c & 0x0F) << 12) | (((uint8_t)utf8[i+1] & 0x3F) << 6)
               | ((uint8_t)utf8[i+2] & 0x3F);
            i += 3;
        } else {
            cp = '?'; i += 1;
        }
        if (cp <= 0xFFFF) {
            buf[out*2]     = (uint8_t)(cp & 0xFF);
            buf[out*2 + 1] = (uint8_t)((cp >> 8) & 0xFF);
            out++;
        }
    }
}

// Parse a stem like "UgcFood003" into (kind index, slot index). Returns false
// on any prefix/number mismatch.
static bool ParseUgcStem(const std::string& stem, int& outKind, int& outSlot) {
    if (stem.size() < 4 || stem.compare(0, 3, "Ugc") != 0) return false;
    for (int k = 0; k < UGC_KIND_COUNT; k++) {
        const char* prefix = UGC_KIND_DATA[k].prefix;
        size_t pl = strlen(prefix);
        if (stem.size() < 3 + pl) continue;
        if (stem.compare(3, pl, prefix) != 0) continue;
        std::string tail = stem.substr(3 + pl);
        if (tail.empty()) continue;
        for (char c : tail) if (c < '0' || c > '9') return false;
        int n = atoi(tail.c_str());
        if (n < 0 || n >= UGC_KIND_DATA[k].maxSlots) return false;
        outKind = k;
        outSlot = n;
        return true;
    }
    return false;
}

std::string RenameUgc(const std::string& stem, const std::string& newName) {
    int kind = -1, slot = -1;
    if (!ParseUgcStem(stem, kind, slot))
        return "Unrecognised UGC stem: " + stem;

    std::vector<uint8_t> psav;
    if (!ReadFile(SAVE_PLAYER_SAV, psav)) return "Cannot read Player.sav";

    int offset = OffsetLocator(psav, UGC_KIND_DATA[kind].hashHex);
    if (offset < 0) return "UGC name table not found in Player.sav";

    // 4-byte array header, then maxSlots * 128 bytes (64 UTF-16LE code units).
    int nameOff = offset + 4 + slot * 128;
    if (nameOff + 128 > (int)psav.size()) return "UGC name slot out of bounds";

    WriteUtf16LeFixed(newName, psav.data() + nameOff, 64);

    if (!WriteFile(SAVE_PLAYER_SAV, psav)) return "Failed to write Player.sav";
    // Keep the in-memory cache aligned with what's now on disk so callers
    // that don't immediately re-scan still see the new name.
    char canonicalStem[32];
    snprintf(canonicalStem, sizeof(canonicalStem), "Ugc%s%03d",
             UGC_KIND_DATA[kind].prefix, slot);
    if (newName.empty()) s_ugcNames.erase(canonicalStem);
    else                 s_ugcNames[canonicalStem] = newName;
    return "";
}

std::string ExportUgc(int ugcKind, int slot, const std::string& destPath) {
    if (ugcKind < 0 || ugcKind >= 7)
        return "Invalid UGC kind";
    if (slot < 1 || slot > UGC_MAX_SL[ugcKind])
        return "Invalid slot (1-" + std::to_string(UGC_MAX_SL[ugcKind]) + ")";

    std::vector<uint8_t> psav;
    if (!ReadFile(SAVE_PLAYER_SAV, psav)) return "Cannot read Player.sav";

    UgcOffsets off;
    if (!BuildUgcOffsets(psav, ugcKind, off))
        return "Could not locate UGC offsets in Player.sav";

    int s = slot - 1;
    int nf       = UGC_NUM_F[ugcKind];
    int nightIdx = UGC_NIGHT_IDX[ugcKind];
    int nightSz  = UGC_NIGHT_SZ[ugcKind];

    // Build ugcData (nf * 4 bytes)
    std::vector<uint8_t> ugcData;
    for (int x = 0; x < nf; x++) {
        uint8_t val[4] = {0,0,0,0};
        if (x == nightIdx) {
            // Extract single bit from the packed bit array
            if (off.f[x] + nightSz <= (int)psav.size()) {
                auto bits = DecodeSexuality(psav.data() + off.f[x], nightSz);
                val[0] = (s < (int)bits.size()) ? (uint8_t)bits[s] : 0;
            }
        } else {
            int fieldOff = off.f[x] + s * 4;
            if (fieldOff + 4 <= (int)psav.size())
                memcpy(val, psav.data() + fieldOff, 4);
        }
        ugcData.insert(ugcData.end(), val, val + 4);
    }

    // Build name section
    int nn = UGC_NUM_N[ugcKind];
    std::vector<uint8_t> nameData;
    for (int i = 0; i < nn; i++) {
        int sz = UGC_N_SZ[ugcKind][i];
        int nameOff = off.n[i] + s * sz;
        if (nameOff + sz <= (int)psav.size())
            nameData.insert(nameData.end(), psav.data() + nameOff, psav.data() + nameOff + sz);
        else
            nameData.insert(nameData.end(), sz, 0);
    }

    // Build vector data (Vector3Array, 12 B/slot) and vector2 (Vector2Array,
    // 8 B/slot). vector2 lives in its OWN array — the old code indexed both
    // through off.v, so it read 8 bytes from the middle of some other slot's
    // Vector3 record (and ImportUgc wrote them back there, corrupting
    // neighbouring Exterior / MapObject entries).
    std::vector<uint8_t> vec(12, 0), vec2(8, 0);
    if (off.v >= 0 && off.v + s * 12 + 12 <= (int)psav.size())
        memcpy(vec.data(), psav.data() + off.v + s * 12, 12);
    if (off.v2 >= 0 && off.v2 + s * 8 + 8 <= (int)psav.size())
        memcpy(vec2.data(), psav.data() + off.v2 + s * 8, 8);

    // Sanitize name for filename
    std::string utf8name = nameData.size() >= 2 ? Utf16leToUtf8(nameData.data(), std::min((int)nameData.size(), 128)) : "";
    std::string sanitName;
    for (unsigned char c : utf8name) {
        if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='-'||c=='_')
            sanitName += (char)c;
        else
            sanitName += '_';
    }
    if (sanitName.empty()) sanitName = "ugc";

    // Build UGC file stem (e.g. "Food005")
    // Stems are zero-padded to three digits (UgcCloth099, UgcCloth100).
    // The old ladder used `s < 99` for the two-digit case, so slot index 99
    // alone produced "Cloth99" and read/wrote the wrong file.
    // 16 bytes so the compiler can see the %03d can never truncate; `s` is
    // already range-checked against UGC_MAX_SL above, but that isn't visible
    // to -Wformat-truncation here.
    char ugcNum[16];
    snprintf(ugcNum, sizeof(ugcNum), "%03d", s);
    std::string ugcStem = std::string(UGC_PFX[ugcKind]) + ugcNum;

    // Read texture files
    std::vector<uint8_t> canvastex, ugctex, thumbtex;
    std::string canvasPath = std::string(SAVE_UGC_DIR) + "/Ugc" + ugcStem + ".canvas.zs";
    std::string ugctexPath = std::string(SAVE_UGC_DIR) + "/Ugc" + ugcStem + ".ugctex.zs";
    std::string thumbPath  = std::string(SAVE_UGC_DIR) + "/Ugc" + ugcStem + "_Thumb.ugctex.zs";
    if (!ReadFile(canvasPath, canvastex)) return "Cannot read canvas: " + canvasPath;
    if (!ReadFile(ugctexPath, ugctex))   return "Cannot read ugctex: " + ugctexPath;
    if (!ReadFile(thumbPath,  thumbtex)) return "Cannot read thumb: "  + thumbPath;

    // Assemble: [kind(1)][0][0][0][ugcData][vec12][vec28][A2A2A2A2][names][A3A3A3A3][canvas][A4A4A4A4][ugctex][A5A5A5A5][thumb]
    std::vector<uint8_t> output;
    output.push_back((uint8_t)ugcKind);
    output.push_back(0); output.push_back(0); output.push_back(0);
    output.insert(output.end(), ugcData.begin(),  ugcData.end());
    output.insert(output.end(), vec.begin(),      vec.end());
    output.insert(output.end(), vec2.begin(),     vec2.end());
    output.insert(output.end(), MAGIC_A2,         MAGIC_A2 + 4);
    output.insert(output.end(), nameData.begin(), nameData.end());
    output.insert(output.end(), MAGIC_A3,         MAGIC_A3 + 4);
    output.insert(output.end(), canvastex.begin(),canvastex.end());
    output.insert(output.end(), MAGIC_A4,         MAGIC_A4 + 4);
    output.insert(output.end(), ugctex.begin(),   ugctex.end());
    output.insert(output.end(), MAGIC_A5,         MAGIC_A5 + 4);
    output.insert(output.end(), thumbtex.begin(), thumbtex.end());

    // Build final path
    std::string finalPath = destPath;
    if (finalPath.size() >= 4 && finalPath.substr(finalPath.size() - 4) == "auto")
        finalPath = finalPath.substr(0, finalPath.size() - 4) + sanitName;
    // Append the extension unless the path already ENDS with it. `find` matched
    // anywhere, so a destination folder containing e.g. ".ltdf" suppressed it
    // and the export landed without an extension.
    const char* ext = UGC_EXT[ugcKind];
    size_t extLen = strlen(ext);
    if (finalPath.size() < extLen ||
        finalPath.compare(finalPath.size() - extLen, extLen, ext) != 0)
        finalPath += ext;
    finalPath = UniquePath(finalPath);

    if (!WriteFile(finalPath, output))
        return "Failed to write " + finalPath;
    return "";
}

std::string ImportUgc(int ugcKind, int slot, const std::string& ltdxPath, bool isAdding) {
    if (ugcKind < 0 || ugcKind >= 7)
        return "Invalid UGC kind";
    if (slot < 1 || slot > UGC_MAX_SL[ugcKind])
        return "Invalid slot (1-" + std::to_string(UGC_MAX_SL[ugcKind]) + ")";

    std::vector<uint8_t> ltdx;
    if (!ReadFile(ltdxPath, ltdx)) return "Cannot read file: " + ltdxPath;
    if (ltdx.empty())              return "Empty file";
    if (ltdx[0] != (uint8_t)ugcKind)
        return std::string("Incorrect UGC type in file. Expected ") + UGC_PFX[ugcKind];

    // Exterior cannot be replaced, only added
    if (ugcKind == 4 && !isAdding)
        return "You can only add Exterior and Objects, not replace. Please use the add slot.";

    std::vector<uint8_t> psav;
    if (!ReadFile(SAVE_PLAYER_SAV, psav)) return "Cannot read Player.sav";

    UgcOffsets off;
    if (!BuildUgcOffsets(psav, ugcKind, off))
        return "Could not locate UGC offsets in Player.sav";

    int s = slot - 1;

    // Cloth: when replacing, subtype (ugcOffsets[0]) must match
    if (ugcKind == 1 && !isAdding && (int)ltdx.size() >= 8) {
        if (off.f[0] + s * 4 + 4 <= (int)psav.size()) {
            if (memcmp(psav.data() + off.f[0] + s * 4, ltdx.data() + 4, 4) != 0)
                return "This item is not the same subtype as what you're importing! Find the same type or add the item.";
        }
    }

    // Find section markers. Each search starts just past the previous marker
    // (mirrors decodeLtdUgc in ltd-sharemii/src/codec.ts). Searching all four
    // independently from offset 0 meant a marker-shaped byte run inside the
    // name block or the canvas payload could be picked up as a later section's
    // marker, silently slicing the textures apart at the wrong offsets.
    int nameMagic = FindMagic(ltdx, MAGIC_A2, 4);
    if (nameMagic < 0) return "Invalid file structure — missing name marker";
    int canvasMagic = FindMagic(ltdx, MAGIC_A3, nameMagic + 4);
    if (canvasMagic < 0) return "Invalid file structure — missing canvas marker";
    int ugctexMagic = FindMagic(ltdx, MAGIC_A4, canvasMagic + 4);
    if (ugctexMagic < 0) return "Invalid file structure — missing ugctex marker";
    int thumbMagic  = FindMagic(ltdx, MAGIC_A5, ugctexMagic + 4);
    if (thumbMagic < 0) return "Invalid file structure — missing thumb marker";
    int nameStart   = nameMagic   + 4;
    int canvasStart = canvasMagic + 4;
    int ugctexStart = ugctexMagic + 4;
    int thumbStart  = thumbMagic  + 4;

    // Write texture files
    // Stems are zero-padded to three digits (UgcCloth099, UgcCloth100).
    // The old ladder used `s < 99` for the two-digit case, so slot index 99
    // alone produced "Cloth99" and read/wrote the wrong file.
    // 16 bytes so the compiler can see the %03d can never truncate; `s` is
    // already range-checked against UGC_MAX_SL above, but that isn't visible
    // to -Wformat-truncation here.
    char ugcNum[16];
    snprintf(ugcNum, sizeof(ugcNum), "%03d", s);
    std::string ugcStem = std::string(UGC_PFX[ugcKind]) + ugcNum;
    MkdirP(SAVE_UGC_DIR);
    std::string canvasOut = std::string(SAVE_UGC_DIR) + "/Ugc" + ugcStem + ".canvas.zs";
    std::string ugctexOut = std::string(SAVE_UGC_DIR) + "/Ugc" + ugcStem + ".ugctex.zs";
    std::string thumbOut  = std::string(SAVE_UGC_DIR) + "/Ugc" + ugcStem + "_Thumb.ugctex.zs";

    // canvas: [canvasStart .. ugctexMagic)
    if (canvasStart <= ugctexMagic) {
        std::vector<uint8_t> canvas(ltdx.begin() + canvasStart, ltdx.begin() + ugctexMagic);
        WriteFile(canvasOut, canvas);
    }
    // ugctex: [ugctexStart .. thumbMagic)
    if (ugctexStart <= thumbMagic) {
        std::vector<uint8_t> ugctex(ltdx.begin() + ugctexStart, ltdx.begin() + thumbMagic);
        WriteFile(ugctexOut, ugctex);
    }
    // thumb: [thumbStart .. end)
    if (thumbStart <= (int)ltdx.size()) {
        std::vector<uint8_t> thumb(ltdx.begin() + thumbStart, ltdx.end());
        WriteFile(thumbOut, thumb);
    }

    // Apply UGC field data to Player.sav
    int nf       = UGC_NUM_F[ugcKind];
    int nightIdx = UGC_NIGHT_IDX[ugcKind];
    int nightSz  = UGC_NIGHT_SZ[ugcKind];
    for (int x = 0; x < nf; x++) {
        int fileOff = 4 + x * 4;
        if (fileOff + 4 > (int)ltdx.size()) break;
        if (x == nightIdx) {
            // Write one bit into the packed bit array
            if (off.f[x] + nightSz <= (int)psav.size()) {
                auto bits = DecodeSexuality(psav.data() + off.f[x], nightSz);
                if (s < (int)bits.size()) {
                    uint8_t val = ltdx[fileOff];
                    bits[s] = (val == 0 || val == 1) ? val : 0;
                }
                auto encoded = EncodeSexuality(bits);
                memcpy(psav.data() + off.f[x], encoded.data(),
                       std::min(encoded.size(), (size_t)nightSz));
            }
        } else {
            int fieldOff = off.f[x] + s * 4;
            if (fieldOff + 4 <= (int)psav.size())
                memcpy(psav.data() + fieldOff, ltdx.data() + fileOff, 4);
        }
    }

    // If adding, mark slot as enabled and set tex/hash metadata
    if (isAdding) {
        static const uint8_t ENABLE_VAL[4] = {0xF4,0xAD,0x7F,0x1D};
        if (off.enable >= 0 && off.enable + s*4 + 4 <= (int)psav.size())
            memcpy(psav.data() + off.enable + s*4, ENABLE_VAL, 4);
        if (off.tex >= 0 && off.tex + s*4 + 4 <= (int)psav.size())
            memcpy(psav.data() + off.tex + s*4, UGC_TEX_DATA + ugcKind * 4, 4);
        if (off.hash >= 0 && off.hash + s*4 + 4 <= (int)psav.size()) {
            uint8_t hv[4] = {(uint8_t)s, 0, UGC_HASH_IDX[ugcKind], 0};
            memcpy(psav.data() + off.hash + s*4, hv, 4);
        }
    }

    // Write name fields
    int nn = UGC_NUM_N[ugcKind];
    int nameReadOff = nameStart;
    for (int i = 0; i < nn; i++) {
        int sz = UGC_N_SZ[ugcKind][i];
        int destOff = off.n[i] + s * sz;
        if (destOff + sz <= (int)psav.size() && nameReadOff + sz <= (int)ltdx.size())
            memcpy(psav.data() + destOff, ltdx.data() + nameReadOff, sz);
        nameReadOff += sz;
    }

    // Write vector data embedded in the file before the A2 marker
    // Layout: [... vec(12) vec2(8) A2A2A2A2 names ...]
    // vector2 goes into off.v2, not off.v — see the matching note in ExportUgc.
    if (off.v >= 0 && nameStart >= 24) {
        if (nameStart - 24 + 12 <= (int)ltdx.size() && off.v + s*12 + 12 <= (int)psav.size())
            memcpy(psav.data() + off.v + s*12, ltdx.data() + nameStart - 24, 12);
    }
    if (off.v2 >= 0 && nameStart >= 12) {
        if (nameStart - 12 + 8 <= (int)ltdx.size() && off.v2 + s*8 + 8 <= (int)psav.size())
            memcpy(psav.data() + off.v2 + s*8, ltdx.data() + nameStart - 12, 8);
    }

    if (!WriteFile(SAVE_PLAYER_SAV, psav)) return "Failed to write Player.sav";
    return "";
}

// ─── Blank Mii / blank UGC (ported from ltd-save-editor-2.13.0) ──────────────

} // namespace MiiManager

#include "default_char_info_ex.h"
#include "belongings_leaves.h"
#include "per_mii_arrays.h"

namespace MiiManager {

int FirstEmptyMiiSlot() {
    std::vector<uint8_t> miisav;
    if (!ReadFile(SAVE_MII_SAV, miisav)) return -1;
    int names_hash = OffsetLocator(miisav, "2499BFDA");
    int data_hash  = OffsetLocator(miisav, "881CA27A");
    if (names_hash < 0 || data_hash < 0) return -1;
    int miinames = names_hash + 4;
    int miidata  = data_hash  + 4;
    for (int x = 0; x < MII_MAX_SLOTS; x++) {
        int miiindex = miidata + 156 * x;
        if (miiindex + 156 > (int)miisav.size()) break;
        // Empty-slot signature used everywhere else in this file:
        // "uninitialized CharInfoEx" has 152 bytes summing to 152
        // (a specific stride of single 0x01s).
        int sum = 0;
        for (int i = 0; i < 156; i++) sum += miisav[miiindex + i];
        if (sum != 152) continue;
        // Also confirm name slot is empty so we don't trample a half-deleted
        // resident whose CharInfoEx happens to match.
        const uint8_t* namePtr = miisav.data() + miinames + x * 64;
        uint16_t firstWchar = (uint16_t)(namePtr[0] | (namePtr[1] << 8));
        if (firstWchar != 0) continue;
        return x + 1;
    }
    return -1;
}

std::string CreateBlankMii(int slot) {
    if (slot < 1 || slot > MII_MAX_SLOTS)
        return "Invalid slot (1-" + std::to_string(MII_MAX_SLOTS) + ")";
    int s = slot - 1;

    // Pure raw in-place byte edits — the SAME mechanism ImportMii uses
    // (OffsetLocator + memcpy, no SaveEditor re-serialization). Import
    // is the proven-working Mii-writing path, so create now mirrors it
    // exactly. Earlier versions round-tripped through SaveEditor::Save,
    // which is what corrupted the save. Raw edits never touch the file
    // structure — they only overwrite existing field values in place —
    // so they can't produce a structurally-invalid save.
    std::vector<uint8_t> miisav;
    if (!ReadFile(SAVE_MII_SAV, miisav)) return "Cannot read Mii.sav";

    int names_hash  = OffsetLocator(miisav, "2499BFDA"); // Mii.Name.Name
    int data_hash   = OffsetLocator(miisav, "881CA27A"); // Mii.CharInfoEx
    int prefer_hash = OffsetLocator(miisav, "3A5EDA05"); // HowToCallName
    if (names_hash < 0 || data_hash < 0)
        return "Could not locate Mii offsets in Mii.sav";
    int miinames  = names_hash  + 4;
    int miidata   = data_hash   + 4;
    int miiprefer = prefer_hash >= 0 ? prefer_hash + 4 : -1;

    // BinArray record layout per slot: [size:4][data:152] = 156 bytes.
    int miiindex = miidata + 156 * s;
    if (miiindex + 156 > (int)miisav.size()) return "Slot out of bounds";

    // Refuse to clobber a populated slot. Empty slot sums to 152 (size
    // prefix 152 + all-zero data).
    int sum = 0;
    for (int i = 0; i < 156; i++) sum += miisav[miiindex + i];
    if (sum != 152)
        return "Slot " + std::to_string(slot) + " is not empty";

    const int fileSize = (int)miisav.size();

    // ── Raw field writers ───────────────────────────────────────────
    // fieldBase returns the payload-data offset for a hash's array
    // (heap offset + 4, skipping the count prefix), or -1 if missing.
    auto fieldBase = [&](const char* hex) -> int {
        int o = OffsetLocator(miisav, hex);
        return o < 0 ? -1 : o + 4;
    };
    auto wU32 = [&](const char* hex, int idx, uint32_t v) {
        int b = fieldBase(hex); if (b < 0) return;
        int off = b + idx * 4; if (off + 4 > fileSize) return;
        miisav[off+0] = (uint8_t)(v & 0xff);
        miisav[off+1] = (uint8_t)((v >> 8) & 0xff);
        miisav[off+2] = (uint8_t)((v >> 16) & 0xff);
        miisav[off+3] = (uint8_t)((v >> 24) & 0xff);
    };
    auto wI32 = [&](const char* hex, int idx, int32_t v) {
        wU32(hex, idx, (uint32_t)v);
    };
    auto wU64 = [&](const char* hex, int idx, uint64_t v) {
        int b = fieldBase(hex); if (b < 0) return;
        int off = b + idx * 8; if (off + 8 > fileSize) return;
        for (int k = 0; k < 8; k++) miisav[off+k] = (uint8_t)((v >> (k*8)) & 0xff);
    };
    auto wBool = [&](const char* hex, int bitIdx, bool v) {
        int b = fieldBase(hex); if (b < 0) return;
        int byteOff = b + (bitIdx / 8); if (byteOff >= fileSize) return;
        uint8_t mask = (uint8_t)(1u << (bitIdx & 7));
        if (v) miisav[byteOff] |= mask; else miisav[byteOff] &= (uint8_t)~mask;
    };

    // Capacity = name-array element count (== Mii slot count, 70).
    int capacity = 0;
    if (names_hash >= 0 && names_hash + 4 <= fileSize) {
        uint32_t c = 0; memcpy(&c, miisav.data() + names_hash, 4);
        capacity = (int)c;
    }
    if (capacity <= 0) return "Bad Mii.sav (zero capacity)";

    // Find a SOURCE slot to clone: the first populated Mii (non-empty
    // name) that isn't the target.
    int srcSlot = -1;
    for (int i = 0; i < capacity; i++) {
        int nOff = miinames + i * 64;
        if (nOff + 2 > fileSize) break;
        uint16_t firstWchar = (uint16_t)(miisav[nOff] | (miisav[nOff+1] << 8));
        if (firstWchar == 0) continue;      // empty slot
        if (i != s) { srcSlot = i; break; }
    }
    if (srcSlot < 0)
        return "Need at least one existing Mii to base a blank one on";

    // ── CLONE per-Mii data from the source slot ─────────────────────
    // This is the key fix. A valid Mii has ~130 per-slot fields set to
    // specific "empty" game defaults (habit ownership states, word
    // kinds, region languages, feeling history, etc.) that addMii()
    // never writes — because addMii assumes a slot the *game* already
    // initialised. Our deleted/empty slots don't have those defaults,
    // so a from-scratch build left dozens of fields at 0/garbage and
    // the game rejected the save. Cloning a real Mii's full per-slot
    // byte-range guarantees every field is a valid game value; the
    // identity fields (name, appearance, personality, housing,
    // SortIndex) are then overwritten below. Relation.* arrays are
    // deliberately NOT in PER_MII_ARRAYS (they're per-pair, cleared
    // separately) so the clone never copies relationships.
    for (int ai = 0; ai < BlankDefaults::PER_MII_ARRAY_COUNT; ai++) {
        const auto& a = BlankDefaults::PER_MII_ARRAYS[ai];
        int lo = OffsetLocatorU32(miisav, a.hash);
        if (lo < 0 || lo + 4 > fileSize) continue;
        uint32_t count = 0; memcpy(&count, miisav.data() + lo, 4);
        if (count == 0 || (int)(count % (uint32_t)capacity) != 0) continue;
        int stride = (int)(count / (uint32_t)capacity);
        int base = lo + 4;
        if (a.type == 1) {
            // BoolArray: stride bits per Mii. Copy bit-by-bit.
            for (int bit = 0; bit < stride; bit++) {
                int sbit = srcSlot * stride + bit;
                int tbit = s       * stride + bit;
                int sByte = base + (sbit / 8), tByte = base + (tbit / 8);
                if (sByte >= fileSize || tByte >= fileSize) break;
                bool v = (miisav[sByte] >> (sbit & 7)) & 1;
                uint8_t m = (uint8_t)(1u << (tbit & 7));
                if (v) miisav[tByte] |= m; else miisav[tByte] &= (uint8_t)~m;
            }
        } else {
            // Fixed-size element arrays. Byte sizes are the per-element
            // heap sizes from the reference parser (forclaude parse.ts /
            // GamedataLib.NET): note W-strings are 2 bytes/code-unit, so
            // e.g. WStr16Array is 32 (16 UTF-16 units), not 16.
            int esz;
            switch (a.type) {
                case 3: case 5: case 7: case 21: esz = 4;   break; // Int/Float/Enum/UInt
                case 9: case 23: case 25:        esz = 8;   break; // Vec2/Int64/UInt64
                case 11:                         esz = 12;  break; // Vec3
                case 13:                         esz = 16;  break; // Str16Array
                case 15: case 27:                esz = 32;  break; // Str32Array / WStr16Array
                case 17: case 29:                esz = 64;  break; // Str64Array / WStr32Array
                case 31:                         esz = 128; break; // WStr64Array
                default:                         esz = 0;   break;
            }
            if (esz == 0) continue;
            int bytes = stride * esz;
            int sOff = base + (srcSlot * stride) * esz;
            int tOff = base + (s       * stride) * esz;
            if (sOff + bytes > fileSize || tOff + bytes > fileSize) continue;
            memmove(miisav.data() + tOff, miisav.data() + sOff, bytes);
        }
    }

    // ── Link the new Mii to every existing Mii (THE fix) ────────────
    // Relations are stored per unordered pair (a<b): 2415 pair slots for
    // a 70-Mii roster, each with two directional records. A VALID save
    // has a complete record for EVERY pair of populated Miis — canonical
    // Id_a/Id_b plus valid directional enum fields (verified against a
    // real save: 0 pairs ever hold Id -1). Our old code CLEARED the
    // reused slot's pairs (Id_a/Id_b = -1, meter 0, type Invalid). But
    // the slot is now populated, so the game saw 70 Miis while the
    // relation matrix claimed one of them didn't exist → contradiction →
    // the game rejected the save. THAT (not the CharInfoEx, SortIndex, or
    // housing) is what was corrupting every blank Mii. The reference
    // addMii LINKS instead; we mirror it exactly, filling each pair and
    // both directions with the same defaults a fresh in-game Mii uses.
    //
    // Pair index for a<b: slot = a*(2*cap-a-1)/2 + (b-a-1). Directional
    // entries live at 2*slot and 2*slot+1.
    {
        // Resolve every field's payload base once (fieldBase = heap
        // offset + 4), so we don't rescan the 2.9 MB file per link.
        int B_IDA  = fieldBase("F7420AFB"); // RelationId.Id_a   Int  (per pair)
        int B_IDB  = fieldBase("4071F71C"); // RelationId.Id_b   Int  (per pair)
        int B_TST  = fieldBase("1A892E50"); // TypeSetTime       U64  (per pair)
        int B_FGT  = fieldBase("C2DB3A72"); // IsFight           Bool (per pair)
        int B_NOT  = fieldBase("6E00CE64"); // IsNotifiedBloodTypeCouple Bool
        int B_MTR  = fieldBase("42C2FC2F"); // Meter             Int  (per dir)
        int B_BAS  = fieldBase("8B41897E"); // BaseRelationType  Enum (per dir)
        int B_BLD  = fieldBase("77D6AFCE"); // BloodType         Enum (per dir)
        int B_BIT  = fieldBase("24433FBE"); // BitFlag           UInt (per dir)
        int B_NCK  = fieldBase("A0058500"); // NickNameToOtherType Enum
        int B_SEC  = fieldBase("96AA92D5"); // SecondPersonRegionLanguageID
        int B_THR  = fieldBase("2B87527F"); // ThirdPersonRegionLanguageID
        int B_UTR  = fieldBase("5B9181EB"); // UpdateTimeRanking U64  (per dir)
        int B_UNK  = fieldBase("FA21F70E"); // Unknown           Bool (per dir)
        const uint32_t OTHER = 0x0784a8dcu;        // enum "Other"
        const uint32_t SECONDP = 0xa31978ddu;      // enum "SecondPerson"
        const uint32_t JPJA = 0xf618046eu;         // enum "JPja"
        auto w32 = [&](int base, int idx, uint32_t v) {
            if (base < 0) return; int o = base + idx*4;
            if (o + 4 > fileSize) return; memcpy(miisav.data() + o, &v, 4);
        };
        auto w64 = [&](int base, int idx, uint64_t v) {
            if (base < 0) return; int o = base + idx*8;
            if (o + 8 > fileSize) return; memcpy(miisav.data() + o, &v, 8);
        };
        auto wbit = [&](int base, int bitIdx, bool v) {
            if (base < 0) return; int o = base + bitIdx/8;
            if (o >= fileSize) return; uint8_t m = (uint8_t)(1u << (bitIdx & 7));
            if (v) miisav[o] |= m; else miisav[o] &= (uint8_t)~m;
        };
        for (int j = 0; j < capacity; j++) {
            if (j == s) continue;
            int nOff = miinames + j * 64;
            if (nOff + 2 > fileSize) break;
            uint16_t fw = (uint16_t)(miisav[nOff] | (miisav[nOff+1] << 8));
            if (fw == 0) continue;                       // only populated Miis
            int a  = j < s ? j : s;
            int bb = j < s ? s : j;
            int slot = a * (2*capacity - a - 1) / 2 + (bb - a - 1);
            // Pair-level record.
            w32(B_IDA, slot, (uint32_t)a);
            w32(B_IDB, slot, (uint32_t)bb);
            w64(B_TST, slot, 0);                          // game-local time; 0 = fresh
            wbit(B_FGT, slot, false);
            wbit(B_NOT, slot, false);
            // Both directional records.
            for (int d = 0; d < 2; d++) {
                int dir = slot*2 + d;
                w32(B_MTR, dir, 100);                     // meter (same as a real save)
                w32(B_BAS, dir, OTHER);
                w32(B_BLD, dir, OTHER);
                w32(B_BIT, dir, 0);
                w32(B_NCK, dir, SECONDP);
                w32(B_SEC, dir, JPJA);
                w32(B_THR, dir, JPJA);
                w64(B_UTR, dir, 0);
                wbit(B_UNK, dir, false);
            }
        }
    }

    // 1) CharInfoEx — write 152-byte default at data offset (keep the
    //    size prefix, already 152 for an empty slot).
    memcpy(miisav.data() + miiindex + 4,
           BlankDefaults::DEFAULT_CHAR_INFO_EX, 152);

    // 2) Name (WStr32, 64 bytes) + clear HowToCallName (128 bytes).
    {
        char nameBuf[16];
        snprintf(nameBuf, sizeof(nameBuf), "Mii %d", slot);
        uint8_t* nameDst = miisav.data() + miinames + s * 64;
        memset(nameDst, 0, 64);
        for (size_t i = 0; nameBuf[i] != 0 && i < 31; i++) {
            nameDst[i*2 + 0] = (uint8_t)nameBuf[i];
            nameDst[i*2 + 1] = 0;
        }
    }
    if (miiprefer >= 0 && miiprefer + s*128 + 128 <= fileSize)
        memset(miisav.data() + miiprefer + s * 128, 0, 128);

    // Enum member value hashes (murmur3 x86 32 of the member name).
    const uint32_t HASH_MALE = SaveEditor::Hash("Male");
    const uint32_t HASH_HE   = SaveEditor::Hash("He");
    const uint32_t HASH_BOY  = SaveEditor::Hash("Boy");
    const uint32_t HASH_USEN = SaveEditor::Hash("USen");
    const uint32_t HASH_JPJA = SaveEditor::Hash("JPja");

    (void)HASH_HE; (void)HASH_BOY; (void)HASH_USEN; (void)HASH_JPJA; (void)HASH_MALE;
    // ── Blank-identity overrides ONLY ───────────────────────────────
    // The clone already gave every field a VALID game value (copied from
    // a real Mii). We only overwrite the small set that defines a fresh
    // blank male Mii. Everything else (eat food-item IDs, region
    // languages, birthday, feeling, timers, satisfaction) is left as the
    // cloned value — overwriting those with hard-coded numbers was the
    // corruption: e.g. addMii's eat BestId/UltraBestId are region-
    // specific and simply don't exist in this save's food catalogue, so
    // writing them created a dangling reference the game rejects.
    //
    // Personality — 5 sliders, mid-value 4.
    wI32("25B48224", s, 4); // Activeness
    wI32("CD8DBAF8", s, 4); // Audaciousness
    wI32("607BA160", s, 4); // Commonsense
    wI32("68E1134E", s, 4); // Gaiety
    wI32("43CD364F", s, 4); // Sociability
    // Voice — "Boy" preset.
    wI32("4913AE1A", s, 28); // Formant
    wI32("81CF470A", s, 28); // Pitch
    wI32("141EE086", s, 25); // Speed
    wI32("4D78E262", s,  5); // Tension
    wU32("FBC3FFB0", s, HASH_BOY); // PresetType
    // Face + cloth — consistent with the neutral male DEFAULT appearance.
    wU32("236E2D73", s, HASH_MALE); // FaceInfo.Gender
    wI32("AFED53D6", s, 0);         // HairChangeOrdered
    wU32("660C5247", s, HASH_MALE); // ClothInfo.ClothStyle
    wU32("F3C3DE59", s, HASH_HE);   // PronounType
    // IsLoveGender BoolArray: middle of the 3-slot group = true.
    wBool("DFC82223", s * 3 + 1, true);
    // Bond meter.
    wI32("D0882636", s, 30);
    // Housing — unhoused, mirroring the reference addMii. A new Mii is
    // left with no house (HouseMapId / CacheMapId = -1); on next launch
    // the game shows the "place the house" prompt once per added Mii
    // (confirmed by the tool author). RoomIndex -1 keeps it out of any
    // room. Note: this is NOT what was corrupting the save — leaving the
    // deleted Mii's house behind is tolerated (a delete-only save loads
    // fine). The real bug was the relation matrix; see the link step above.
    wI32("B8AB3CAC", s, -1); // HouseMapId
    wI32("FC296601", s, -1); // RoomIndex
    wI32("3CBD9978", s, -1); // HomeLiveTime.CacheMapId
    wI32("D7070113", s, 0);  // HomeLiveTime.CacheOwnerNum
    wU64("34190CDF", s, 0);  // StartLiveTogetherTime
    // Starter clothing / coordinate ownership. The clone copied the
    // SOURCE Mii's owned items into this slot; reset the ownership
    // strides to zero, then flag only the four starter cloth subindices
    // + the starter coordinate, matching addMii(). ClothOwnInfo /
    // CoordinateOwnInfo are UIntArrays where 0 = "not owned", so zeroing
    // is the correct empty value.
    {
        static const int CLOTH_OWN_STRIDE = 1200;
        static const int COORD_OWN_STRIDE = 400;
        static const int CLOTH_SUBS[] = { 77, 246, 339, 382 };
        int clothBase = fieldBase("84824827");
        if (clothBase >= 0)
            for (int i = 0; i < CLOTH_OWN_STRIDE; i++) {
                int off = clothBase + (s * CLOTH_OWN_STRIDE + i) * 4;
                if (off + 4 <= fileSize) memset(miisav.data() + off, 0, 4);
            }
        int coordBase = fieldBase("89CE9150");
        if (coordBase >= 0)
            for (int i = 0; i < COORD_OWN_STRIDE; i++) {
                int off = coordBase + (s * COORD_OWN_STRIDE + i) * 4;
                if (off + 4 <= fileSize) memset(miisav.data() + off, 0, 4);
            }
        for (int sub : CLOTH_SUBS)
            wU32("84824827", s * CLOTH_OWN_STRIDE + sub, 1);
        wU32("89CE9150", s * COORD_OWN_STRIDE, 1);
    }

    // ── Renumber SortIndex to a dense 0..N-1 permutation ────────────
    // The game requires SortIndex to be exactly a permutation of
    // 0..(populatedCount-1) — a gap or an out-of-range value (the clone
    // left the target sharing the source's index; a naive max+1 would
    // write 70 which is out of range) corrupts the roster. Collect all
    // populated slots, sort by their current SortIndex to preserve the
    // existing display order, then assign dense ranks. The new Mii,
    // carrying the source's index, slots in adjacent to the source and
    // gets a unique rank like everyone else.
    {
        int siBase = fieldBase("9CCF22B7");
        if (siBase >= 0) {
            std::vector<std::pair<int32_t,int>> pop; // (currentSortIndex, slot)
            for (int i = 0; i < capacity; i++) {
                int nOff = miinames + i * 64;
                if (nOff + 2 > fileSize) break;
                uint16_t fw = (uint16_t)(miisav[nOff] | (miisav[nOff+1] << 8));
                if (fw == 0) continue;
                if (siBase + (i+1)*4 > fileSize) continue;
                int32_t si; memcpy(&si, miisav.data() + siBase + i*4, 4);
                pop.push_back({si, i});
            }
            std::stable_sort(pop.begin(), pop.end(),
                [](const std::pair<int32_t,int>& a, const std::pair<int32_t,int>& b){
                    return a.first < b.first;
                });
            for (size_t r = 0; r < pop.size(); r++) {
                int32_t v = (int32_t)r;
                memcpy(miisav.data() + siBase + pop[r].second*4, &v, 4);
            }
        }
    }

    if (!WriteFile(SAVE_MII_SAV, miisav)) return "Failed to write Mii.sav";
    return "";
}

int FirstEmptyUgcSlot(int ugcKind) {
    if (ugcKind < 0 || ugcKind >= UGC_KIND_COUNT) return -1;
    std::vector<uint8_t> psav;
    if (!ReadFile(SAVE_PLAYER_SAV, psav)) return -1;
    int offset = OffsetLocator(psav, UGC_KIND_DATA[ugcKind].hashHex);
    if (offset < 0) return -1;
    int maxSlots = UGC_KIND_DATA[ugcKind].maxSlots;
    // Layout matches LoadUgcNames (line ~706): payload starts at offset+4,
    // then maxSlots * 128 UTF-16LE bytes. The earlier `+4+4` skip walked
    // 4 bytes past every name slot and read garbage.
    int base = offset + 4;
    for (int x = 0; x < maxSlots; x++) {
        int nameOff = base + x * 128;
        if (nameOff + 128 > (int)psav.size()) break;
        uint16_t firstWchar = (uint16_t)(psav[nameOff] | (psav[nameOff+1] << 8));
        if (firstWchar == 0) return x + 1;
    }
    return -1;
}

// Empty out a UGC slot. Two-step:
//   1. clear the slot's name via RenameUgc(stem, "") — that also removes
//      the s_ugcNames cache entry, so the WebUI + on-Switch name lookups
//      immediately see the blank.
//   2. remove the on-disk texture files (.canvas.zs, .ugctex.zs and the
//      _Thumb_ugctex.zs) — this is what the UGC scanner uses to build
//      the list, so without this step the "deleted" slot would still
//      show up with a placeholder white square.
// Missing files are tolerated: a partial slot (e.g. one where only the
// canvas is on disk) still gets what remains removed.
std::string DeleteUgc(int ugcKind, int slot) {
    if (ugcKind < 0 || ugcKind >= UGC_KIND_COUNT)
        return "Invalid UGC kind";
    int maxSlots = UGC_KIND_DATA[ugcKind].maxSlots;
    if (slot < 1 || slot > maxSlots)
        return "Invalid slot (1-" + std::to_string(maxSlots) + ")";
    const auto& ki = UGC_KIND_DATA[ugcKind];
    int slotIdx = slot - 1;
    char stem[32];
    snprintf(stem, sizeof(stem), "Ugc%s%03d", ki.prefix, slotIdx);
    // Blank the name; ignore errors so a stale/missing name table
    // doesn't block us from at least deleting the files below.
    RenameUgc(stem, "");
    // Also zero the enable/texture/hashId metadata written by
    // CreateBlankUgc — otherwise the game keeps the slot flagged as
    // active with stale texture-source and content-id values and can
    // render a broken item entry after the files are gone.
    {
        std::vector<uint8_t> psav;
        if (ReadFile(SAVE_PLAYER_SAV, psav)) {
            auto zeroU32Arr = [&](const char* hashHex) {
                int offset = OffsetLocator(psav, hashHex);
                if (offset < 0) return;
                int off = offset + 4 + slotIdx * 4;
                if (off + 4 > (int)psav.size()) return;
                psav[off + 0] = 0; psav[off + 1] = 0;
                psav[off + 2] = 0; psav[off + 3] = 0;
            };
            zeroU32Arr(ki.enableHex);
            zeroU32Arr(ki.textureHex);
            zeroU32Arr(ki.hashIdHex);
            WriteFile(SAVE_PLAYER_SAV, psav);
        }
    }
    LoadUgcNames();
    // Sweep the on-disk texture files. remove() returns non-zero on
    // failure; missing is fine, permission issues surface as an error
    // string once we've tried all three.
    // Both thumbnail spellings are in the wild — ugc_scanner.cpp accepts either,
    // and ExportUgc / ImportUgc / texture_processor.cpp all write the
    // "_Thumb.ugctex.zs" form. Deleting only "_Thumb_ugctex.zs" left the
    // thumbnail behind, so the scanner kept listing the deleted slot.
    std::string base = std::string(SAVE_UGC_DIR) + "/" + stem;
    const char* suffixes[] = { ".canvas.zs", ".ugctex.zs",
                               "_Thumb.ugctex.zs", "_Thumb_ugctex.zs" };
    std::string err;
    for (const char* suf : suffixes) {
        std::string path = base + suf;
        if (remove(path.c_str()) != 0 && errno != ENOENT) {
            if (!err.empty()) err += "; ";
            err += "failed to delete " + std::string(suf);
        }
    }
    return err;
}

std::string CreateBlankUgc(int ugcKind, int slot) {
    if (ugcKind < 0 || ugcKind >= UGC_KIND_COUNT)
        return "Invalid UGC kind";
    int maxSlots = UGC_KIND_DATA[ugcKind].maxSlots;
    if (slot < 1 || slot > maxSlots)
        return "Invalid slot (1-" + std::to_string(maxSlots) + ")";
    const auto& ki = UGC_KIND_DATA[ugcKind];
    int slotIdx = slot - 1;
    char stem[32];
    snprintf(stem, sizeof(stem), "Ugc%s%03d", ki.prefix, slotIdx);
    char defaultName[32];
    snprintf(defaultName, sizeof(defaultName), "Blank %s %d",
             ki.prefix, slot);
    // 1) Name (via existing helper — reads/writes Player.sav on its own).
    std::string err = RenameUgc(stem, defaultName);
    if (!err.empty()) return err;
    // 2) Enable + texture + hashId metadata. Without these three the
    //    game treats the slot as garbage even when the name and the
    //    on-disk .canvas.zs / .ugctex.zs / thumb files exist. Mirrors
    //    initAddedSlotMeta() from 2.13.0's applyUgc.ts.
    std::vector<uint8_t> psav;
    if (!ReadFile(SAVE_PLAYER_SAV, psav)) return "Cannot read Player.sav";
    auto writeU32Arr = [&](const char* hashHex, int idx, uint32_t val,
                           const char* label) -> std::string {
        int offset = OffsetLocator(psav, hashHex);
        if (offset < 0) return std::string(label) + " field not found";
        // Array payload: [count:4][element:4]*count
        int off = offset + 4 + idx * 4;
        if (off + 4 > (int)psav.size())
            return std::string(label) + " slot out of bounds";
        psav[off + 0] = (uint8_t)( val        & 0xff);
        psav[off + 1] = (uint8_t)((val >>  8) & 0xff);
        psav[off + 2] = (uint8_t)((val >> 16) & 0xff);
        psav[off + 3] = (uint8_t)((val >> 24) & 0xff);
        return "";
    };
    err = writeU32Arr(ki.enableHex,  slotIdx, UGC_ENABLE_USED, "enable");
    if (!err.empty()) return err;
    err = writeU32Arr(ki.textureHex, slotIdx, ki.textureValue,  "texture");
    if (!err.empty()) return err;
    uint32_t hashIdVal = ((uint32_t)slotIdx & 0xffu)
                       | ((ki.kindTag & 0xffu) << 16);
    err = writeU32Arr(ki.hashIdHex,  slotIdx, hashIdVal,        "hashId");
    if (!err.empty()) return err;
    if (!WriteFile(SAVE_PLAYER_SAV, psav)) return "Failed to write Player.sav";
    LoadUgcNames(); // refresh the in-memory name cache
    return "";
}

} // namespace MiiManager
