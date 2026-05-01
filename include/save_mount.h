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

} // namespace SaveMount
