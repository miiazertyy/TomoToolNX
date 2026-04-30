// mii_manager.cpp — port of ShareMii.py
// Binary I/O only, no SDL, safe on any thread.

#include "mii_manager.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
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
// Searches for the little-endian hash, returns the data offset (hash+4+4)

static int OffsetLocator(const std::vector<uint8_t>& data, const char* hashHex) {
    // Parse hex string to 4 bytes
    uint8_t hash[4];
    for (int i = 0; i < 4; i++) {
        unsigned int v = 0;
        sscanf(hashHex + i*2, "%02x", &v);
        hash[i] = (uint8_t)v;
    }
    // Reverse to little-endian
    uint8_t le[4] = {hash[3], hash[2], hash[1], hash[0]};

    // Search
    for (size_t i = 0; i + 8 <= data.size(); i++) {
        if (memcmp(data.data() + i, le, 4) == 0) {
            // Next 4 bytes are the offset value
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
static const uint8_t MAGIC_A3[4] = {0xA3,0xA3,0xA3,0xA3};
static const uint8_t MAGIC_A4[4] = {0xA4,0xA4,0xA4,0xA4};

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
        return "Invalid slot (must be 1-70)";

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
        snprintf(fpFile, sizeof(fpFile), facepaintID < 10 ? "00%d" : "0%d", facepaintID);
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
        return "Invalid slot (must be 1-70)";

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

    // Detect facepaint (ltdData[0] or ltdData[1] set, or canvas/ugctex non-empty)
    int facepaint = 0;
    if (mii[1] == 1 || mii[2] == 1) {
        facepaint = 1;
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
        // Sexuality (3 bytes at 424)
        if (persOffsetSX >= 0 && (int)mii.size() >= 427 && persOffsetSX + 27 <= (int)miisav.size()) {
            auto bits = DecodeSexuality(miisav.data() + persOffsetSX, 27);
            for (int b = 0; b < 3; b++)
                if (s*3+b < (int)bits.size()) bits[s*3+b] = mii[424+b];
            auto encoded = EncodeSexuality(bits);
            memcpy(miisav.data() + persOffsetSX, encoded.data(), std::min(encoded.size(), (size_t)27));
        }
    }

    // Facepaint handling
    if (facepaint) {
        // Find or assign a facepaint ID
        int facepaintID = -1;
        if (miiOffset2 >= 0 && miiOffset2 + 4*s < (int)miisav.size())
            facepaintID = miisav[miiOffset2 + 4*s];
        if (facepaintID == 0xFF || facepaintID < 0) {
            // Assign next available ID
            std::vector<bool> used(70, false);
            for (int x = 0; x < 70; x++) {
                if (miiOffset2 >= 0 && miiOffset2 + 4*x < (int)miisav.size()) {
                    uint8_t id = miisav[miiOffset2 + 4*x];
                    if (id != 0xFF && id < 70) used[id] = true;
                }
            }
            for (int id = 0; id < 70; id++) {
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
        snprintf(fpFile, sizeof(fpFile), facepaintID < 10 ? "00%d" : "0%d", facepaintID);
        MkdirP(SAVE_UGC_DIR);
        std::string canvasPath = std::string(SAVE_UGC_DIR) + "/UgcFacePaint" + fpFile + ".canvas.zs";
        std::string ugctexPath = std::string(SAVE_UGC_DIR) + "/UgcFacePaint" + fpFile + ".ugctex.zs";

        // Canvas: from canvasStart to ugctexStart-4
        if (canvasStart < ugctexStart - 4) {
            std::vector<uint8_t> canvas(mii.begin()+canvasStart, mii.begin()+ugctexStart-4);
            WriteFile(canvasPath, canvas);
        }
        // Ugctex: from ugctexStart to end
        if (ugctexStart < (int)mii.size()) {
            std::vector<uint8_t> ugctexData(mii.begin()+ugctexStart, mii.end());
            WriteFile(ugctexPath, ugctexData);
        }
    }

    // Write save files
    if (!WriteFile(SAVE_MII_SAV,    miisav))    return "Failed to write Mii.sav";
    if (!WriteFile(SAVE_PLAYER_SAV, playersav)) return "Failed to write Player.sav";

    return ""; // success
}

} // namespace MiiManager
