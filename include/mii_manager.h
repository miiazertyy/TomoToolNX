#pragma once
// mii_manager.h — port of ShareMii.py core logic
// Reads/writes Mii.sav and Player.sav directly.
// No SDL required — all binary file I/O, safe on any thread.

#include <string>
#include <vector>
#include <cstdint>

#define SAVE_MII_SAV    "tomodata:/Mii.sav"
#define SAVE_PLAYER_SAV "tomodata:/Player.sav"
#define SAVE_MAP_SAV    "tomodata:/Map.sav"
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

// Load all UGC slot names from Player.sav into an internal cache.
// Call this after mounting the save / after each UGC scan.
void LoadUgcNames();

// Return the user-visible name for a UGC stem (e.g. "UgcFood003").
// Returns empty string if the slot is unnamed or the save is unreadable.
std::string GetUgcName(const std::string& stem);

// Overwrite the in-game name for a UGC slot (e.g. "UgcFood003") with `newName`.
// `newName` is treated as UTF-8 and re-encoded to the 128-byte UTF-16LE slot
// the game expects. Returns "" on success or an error string. Caller should
// call LoadUgcNames() afterwards to refresh the in-memory cache.
std::string RenameUgc(const std::string& stem, const std::string& newName);

// Export UGC item at 1-based slot to destPath (auto-appends .ltdf/.ltdc/etc.).
// ugcKind: 0=Food,1=Cloth,2=Goods,3=Interior,4=Exterior,5=MapObject,6=MapFloor
// Returns empty string on success, error message on failure.
std::string ExportUgc(int ugcKind, int slot, const std::string& destPath);

// Import a .ltdx file into slot (1-based). Pass isAdding=true for new slots.
// Returns empty string on success, error message on failure.
std::string ImportUgc(int ugcKind, int slot, const std::string& ltdxPath, bool isAdding);

} // namespace MiiManager
