#pragma once
// mii_manager.h — port of ShareMii.py core logic
// Reads/writes Mii.sav and Player.sav directly.
// No SDL required — all binary file I/O, safe on any thread.

#include <string>
#include <vector>
#include <cstdint>

#define SAVE_MII_SAV    "tomodata:/Mii.sav"
#define SAVE_PLAYER_SAV "tomodata:/Player.sav"
#define SAVE_UGC_DIR    "tomodata:/Ugc"
#define MII_LTD_VERSION 3
#define MII_MAX_SLOTS   70

namespace MiiManager {

struct MiiSlot {
    int    slot;       // 1-based
    std::string name;  // UTF-8 decoded name
    bool   hasFacepaint;
    bool   isEmpty;
};

// List all filled Mii slots. Returns empty vector on error.
std::vector<MiiSlot> ListMiis();

// Export Mii at slot (1-based) to a .ltd file at destPath.
// Returns empty string on success, error message on failure.
std::string ExportMii(int slot, const std::string& destPath);

// Import a .ltd file into slot (1-based).
// Returns empty string on success, error message on failure.
std::string ImportMii(int slot, const std::string& ltdPath);

} // namespace MiiManager
