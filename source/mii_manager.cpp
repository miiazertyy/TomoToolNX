// mii_manager.cpp — port of ShareMii.py
// Binary I/O only, no SDL, safe on any thread.

#include "mii_manager.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <map>
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

// Find first occurrence of 4-byte pattern
static int FindMagic(const std::vector<uint8_t>& data, const uint8_t* magic, int from=0) {
    for (size_t i = from; i + 4 <= data.size(); i++)
        if (memcmp(data.data()+i, magic, 4) == 0) return (int)i;
    return -1;
}
static int FindMagicLast(const std::vector<uint8_t>& data, const uint8_t* magic) {
    int last = -1;
    for (size_t i = 0; i + 4 <= data.size(); i++)
        if (memcmp(data.data()+i, magic, 4) == 0) last = (int)i;
    return last;
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
        if (ReadFile(canvasPath, canvastex)) ltdData[0] = 1;
        if (ReadFile(ugctexPath,  ugctex))  ltdData[1] = 1;
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

    // Read .ltd
    std::vector<uint8_t> mii;
    if (!ReadFile(ltdPath, mii)) return "Cannot read .ltd file";
    if (mii.empty())             return "Empty .ltd file";
    if (mii[0] < 1 || mii[0] > 3) return "Unsupported .ltd version: " + std::to_string(mii[0]);

    // Convert older versions to v3
    if (mii[0] < 3) {
        mii.erase(mii.begin() + 4);
        if (mii[0] == 2) {
            mii.insert(mii.begin() + 427, 0);
            int cs = FindMagic(mii, MAGIC_A3);
            int us = FindMagicLast(mii, MAGIC_A3);
            if (cs >= 0 && us >= 0 && cs != us) {
                mii.insert(mii.begin() + cs + 3, 0xA3);
                mii[us+1] = 0xA4; mii[us+2] = 0xA4; mii[us+3] = 0xA4;
                mii.insert(mii.begin() + us + 3, 0xA4);
            }
        }
    }

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

    // Find sections in .ltd
    int canvasStart = FindMagic(mii, MAGIC_A3);
    int ugctexStart = FindMagicLast(mii, MAGIC_A4);
    if (canvasStart < 0 || ugctexStart < 0)
        return "Invalid .ltd file structure";
    canvasStart += 4;
    ugctexStart += 4;

    // Detect facepaint — embedded in .ltd (ltdData[0]==1 && ltdData[1]==1)
    int facepaint = 0;
    if (mii[1] == 1 && mii[2] == 1) {
        facepaint = 1;
        if (mii.size() > 47) mii[47] = 1;
    }
    // External .canvas.zs / .ugctex.zs alongside the .ltd file (facepaint == 2)
    std::string ltdBase = ltdPath;
    if (ltdBase.size() >= 4 && ltdBase.substr(ltdBase.size()-4) == ".ltd")
        ltdBase = ltdBase.substr(0, ltdBase.size()-4);
    std::string extCanvasPath = ltdBase + ".canvas.zs";
    std::string extUgcPath    = ltdBase + ".ugctex.zs";
    if (FileExists(extCanvasPath) && FileExists(extUgcPath)) {
        facepaint = 2;
        if (mii.size() > 47) mii[47] = 1;
    }

    // Write raw Mii data to Mii.sav
    if (miiindex + 156 <= (int)miisav.size())
        memcpy(miisav.data() + miiindex, mii.data() + 4, 156);

    // Personality (18 × 4 bytes starting at offset 160 in .ltd)
    if (mii[0] >= 2 && (int)mii.size() >= 160 + 18*4) {
        for (int i = 0; i < 18; i++) {
            int off = OffsetLocator(miisav, PERS_HASHES_MII[i]);
            if (off < 0) continue;
            off += 4 + s * 4;
            if (off + 4 <= (int)miisav.size())
                memcpy(miisav.data() + off, mii.data() + 160 + i*4, 4);
        }
        // Name (64 bytes at 232) + Pronunciation (128 bytes at 296)
        if ((int)mii.size() >= 232 + 64)
            memcpy(miisav.data() + miinames + s*64, mii.data() + 232, 64);
        if ((int)mii.size() >= 296 + 128)
            memcpy(miisav.data() + miiprefer + s*128, mii.data() + 296, 128);
        // Sexuality (3 bytes at 424) — only if Mii is level < 2
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
        if (canSetSexuality && persOffsetSX >= 0 && (int)mii.size() >= 427 && persOffsetSX + 27 <= (int)miisav.size()) {
            auto bits = DecodeSexuality(miisav.data() + persOffsetSX, 27);
            for (int b = 0; b < 3; b++)
                if (s*3+b < (int)bits.size()) bits[s*3+b] = mii[424+b];
            auto encoded = EncodeSexuality(bits);
            memcpy(miisav.data() + persOffsetSX, encoded.data(), std::min(encoded.size(), (size_t)27));
        }
    }

    // Read existing facepaint ID for this slot (0xFF = none)
    int facepaintID = -1;
    if (miiOffset2 >= 0 && miiOffset2 + 4*s < (int)miisav.size()) {
        uint8_t raw = miisav[miiOffset2 + 4*s];
        if (raw != 0xFF) facepaintID = raw;
    }

    // Facepaint handling
    if (facepaint) {
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

        // Write facepaint texture files
        char fpFile[8];
        snprintf(fpFile, sizeof(fpFile), "%03d", facepaintID);
        MkdirP(SAVE_UGC_DIR);
        std::string canvasPath = std::string(SAVE_UGC_DIR) + "/UgcFacePaint" + fpFile + ".canvas.zs";
        std::string ugctexPath = std::string(SAVE_UGC_DIR) + "/UgcFacePaint" + fpFile + ".ugctex.zs";

        if (facepaint == 2) {
            // External files alongside the .ltd — copy them directly
            std::vector<uint8_t> extCanvas, extUgcTex;
            if (ReadFile(extCanvasPath, extCanvas)) WriteFile(canvasPath, extCanvas);
            if (ReadFile(extUgcPath,    extUgcTex))  WriteFile(ugctexPath,  extUgcTex);
        } else {
            // Embedded in .ltd — extract from between the magic markers
            if (canvasStart < ugctexStart - 4) {
                std::vector<uint8_t> canvas(mii.begin()+canvasStart, mii.begin()+ugctexStart-4);
                WriteFile(canvasPath, canvas);
            }
            if (ugctexStart < (int)mii.size()) {
                std::vector<uint8_t> ugctexData(mii.begin()+ugctexStart, mii.end());
                WriteFile(ugctexPath, ugctexData);
            }
        }
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

    return ""; // success
}

// ─── UGC name cache ───────────────────────────────────────────────────────────

static const struct { const char* prefix; const char* hashHex; int maxSlots; } UGC_KIND_DATA[] = {
    {"Food",      "408494F5", 99},
    {"Cloth",     "40710642", 299},
    {"Goods",     "2F793EB1", 99},
    {"Interior",  "3DE2C5DD", 99},
    {"Exterior",  "27C875D6", 99},
    {"MapObject", "56F99338", 99},
    {"MapFloor",  "918875A9", 99},
};
static const int UGC_KIND_COUNT = 7;

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

    // Build vector data (12 bytes) and vector2 data (8 bytes)
    // NOTE: Python uses vOffset for both vector and vector2 reads — porting faithfully
    std::vector<uint8_t> vec(12, 0), vec2(8, 0);
    if (off.v >= 0 && off.v + s * 12 + 12 <= (int)psav.size())
        memcpy(vec.data(), psav.data() + off.v + s * 12, 12);
    if (off.v2 >= 0 && off.v >= 0 && off.v + s * 8 + 8 <= (int)psav.size())
        memcpy(vec2.data(), psav.data() + off.v + s * 8, 8);

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
    std::string ugcNum;
    if (s < 10)      ugcNum = "00" + std::to_string(s);
    else if (s < 99) ugcNum = "0"  + std::to_string(s);
    else             ugcNum = std::to_string(s);
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
    const char* ext = UGC_EXT[ugcKind];
    if (finalPath.find(ext) == std::string::npos)
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

    // Find section markers
    int nameMagic   = FindMagic(ltdx, MAGIC_A2);
    int canvasMagic = FindMagic(ltdx, MAGIC_A3);
    int ugctexMagic = FindMagic(ltdx, MAGIC_A4);
    int thumbMagic  = FindMagic(ltdx, MAGIC_A5);
    if (nameMagic < 0 || canvasMagic < 0 || ugctexMagic < 0 || thumbMagic < 0)
        return "Invalid file structure — missing section markers";
    if (nameMagic < 4)
        return "Invalid file: A2 marker too early";
    int nameStart   = nameMagic   + 4;
    int canvasStart = canvasMagic + 4;
    int ugctexStart = ugctexMagic + 4;
    int thumbStart  = thumbMagic  + 4;

    // Write texture files
    std::string ugcNum;
    if (s < 10)      ugcNum = "00" + std::to_string(s);
    else if (s < 99) ugcNum = "0"  + std::to_string(s);
    else             ugcNum = std::to_string(s);
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
    // NOTE: Python uses vOffset (not v2Offset) for both vec and vec2 writes — porting faithfully
    if (off.v >= 0 && nameStart >= 24) {
        if (nameStart - 24 + 12 <= (int)ltdx.size() && off.v + s*12 + 12 <= (int)psav.size())
            memcpy(psav.data() + off.v + s*12, ltdx.data() + nameStart - 24, 12);
    }
    if (off.v2 >= 0 && off.v >= 0 && nameStart >= 12) {
        if (nameStart - 12 + 8 <= (int)ltdx.size() && off.v + s*8 + 8 <= (int)psav.size())
            memcpy(psav.data() + off.v + s*8, ltdx.data() + nameStart - 12, 8);
    }

    if (!WriteFile(SAVE_PLAYER_SAV, psav)) return "Failed to write Player.sav";
    return "";
}

} // namespace MiiManager
