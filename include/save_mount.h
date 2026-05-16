#pragma once
// save_mount.h
// Mounts Tomodachi Life save data directly via libnx fsdev API.
// Title ID: 010051F0207B2000

#include <string>
#include <vector>
#include <switch.h>

#define TOMODACHI_TITLE_ID 0x010051F0207B2000ULL
#define SAVE_MOUNT_NAME    "tomodata"
#define SAVE_UGC_PATH      "tomodata:/Ugc"

namespace SaveMount {

struct UserInfo {
    AccountUid uid;
    std::string nickname;
    std::vector<uint8_t> avatarJpeg; // raw JPEG bytes of profile image, may be empty
};

// Get all user accounts on the system
std::vector<UserInfo> GetUsers();

// Mount the save for a given user. Returns empty string on success, error on failure.
std::string Mount(AccountUid uid);

// Commit (write) any pending changes to the save filesystem
std::string Commit();

// Unmount the save filesystem
void Unmount();

// Returns true if currently mounted
bool IsMounted();

// Get the Switch's IP address as a string (e.g. "192.168.1.42"), empty if no wifi
std::string GetLocalIP();

// Create a fresh Tomodachi Life save container for `uid` via
// fsCreateSaveDataFileSystem, mount it, and write the three .sav blobs.
// `mii`/`map`/`player` are flat byte spans (any may be empty to skip writing
// that file). On success the save stays mounted and Commit()/Unmount() must be
// called as usual; the function returns "" on success or an error string.
std::string CreateAndSeed(AccountUid uid,
                          const uint8_t* mii,    size_t miiLen,
                          const uint8_t* map,    size_t mapLen,
                          const uint8_t* player, size_t playerLen);

// Return true if a Tomodachi Life save already exists for `uid`. Reads the
// save-data list without mounting; used to gate the "+" / bundle assignment
// flows so an existing save is never silently overwritten.
bool SaveExists(AccountUid uid);

// Format an AccountUid as a stable 32-char lowercase hex string. Used to
// scope on-disk artifacts (e.g. per-user backup folders) so each Switch
// account keeps its own isolated history.
std::string FormatUid(const AccountUid& uid);

// Filesystem-safe folder name for a user, used to scope backups on the SD
// card so the user can see "MyNickname_<short>" instead of a 32-char hex
// string. Nickname chars outside [A-Za-z0-9-_ ] are replaced with `_`,
// runs of `_` are collapsed, leading/trailing `_` are trimmed; if the
// nickname is empty after sanitization, falls back to "User". A short
// suffix from the UID is appended so two users with the same nickname
// (or the same user after renaming) never collide.
std::string SafeUserFolder(const UserInfo& user);

} // namespace SaveMount
