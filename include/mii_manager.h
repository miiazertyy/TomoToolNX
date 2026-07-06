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

// Find the first 1-based slot index whose name is empty AND whose CharInfoEx
// is all zeros (matching ltd-save-editor-2.13.0's firstEmptyMiiSlot). Returns
// -1 if every slot is occupied.
int FirstEmptyMiiSlot();

// Allocate a fresh "name name" male Mii in the given empty slot — mirrors
// the addMii() flow from ltd-save-editor-2.13.0/src/lib/mii/ownership/addMii.ts:
// writes the bundled DEFAULT_CHAR_INFO_EX blob, sets default character/voice
// stats, names the slot "Mii N", and reads back as an editable resident.
// Caller must commit Mii.sav afterwards. Returns "" on success or an error
// string (e.g. "slot N is not empty", "CharInfoEx array missing").
std::string CreateBlankMii(int slot);

// Find the first 1-based slot of UGC kind `ugcKind` whose name is empty.
// Returns -1 if all 100 slots are full.
int FirstEmptyUgcSlot(int ugcKind);

// Generate a white 256×256 placeholder UGC at slot `slot` of kind `ugcKind`,
// writing the {stem}.canvas.zs / {stem}.ugctex.zs / {stem}.thumb files via
// our normal texture_processor pipeline. Also sets the UGC name. Returns ""
// on success or an error string.
std::string CreateBlankUgc(int ugcKind, int slot);

// Empty out the slot: clears the name in Player.sav (so the game shows
// the slot as blank) and removes the on-disk .canvas.zs, .ugctex.zs and
// _Thumb_ugctex.zs files so the scanner drops the entry. Returns ""
// on success or an error string. Non-fatal if some files are already
// missing (a partial slot is still cleaned up).
std::string DeleteUgc(int ugcKind, int slot);

} // namespace MiiManager
