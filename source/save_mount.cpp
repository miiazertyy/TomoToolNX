// save_mount.cpp
#include "save_mount.h"
#include <cstring>
#include <cstdio>
#include <sys/stat.h>
#include <switch.h>

namespace SaveMount {

static bool s_mounted = false;

std::vector<UserInfo> GetUsers() {
    std::vector<UserInfo> users;

    Result rc = accountInitialize(AccountServiceType_Application);
    if (R_FAILED(rc)) rc = accountInitialize(AccountServiceType_System);
    if (R_FAILED(rc)) return users;

    s32 count = 0;
    AccountUid uids[ACC_USER_LIST_SIZE];
    rc = accountListAllUsers(uids, ACC_USER_LIST_SIZE, &count);
    if (R_FAILED(rc)) { accountExit(); return users; }

    for (s32 i = 0; i < count; i++) {
        AccountProfile profile;
        if (R_FAILED(accountGetProfile(&profile, uids[i]))) continue;

        AccountProfileBase base;
        if (R_FAILED(accountProfileGet(&profile, nullptr, &base))) {
            accountProfileClose(&profile);
            continue;
        }

        UserInfo info;
        info.uid = uids[i];
        info.nickname = std::string(base.nickname);

        // Load avatar image
        u32 imgSize = 0;
        if (R_SUCCEEDED(accountProfileGetImageSize(&profile, &imgSize)) && imgSize > 0) {
            info.avatarJpeg.resize(imgSize);
            u32 actualSize = 0;
            if (R_FAILED(accountProfileLoadImage(&profile, info.avatarJpeg.data(), imgSize, &actualSize)))
                info.avatarJpeg.clear();
            else
                info.avatarJpeg.resize(actualSize);
        }

        users.push_back(info);
        accountProfileClose(&profile);
    }

    accountExit();
    return users;
}

std::string Mount(AccountUid uid) {
    if (s_mounted) Unmount();

    // Use the dedicated fsdev save data mount helper.
    // This calls fsOpenSaveDataFileSystem internally and mounts it.
    Result rc = fsdevMountSaveData(SAVE_MOUNT_NAME,
                                   (u64)TOMODACHI_TITLE_ID, uid);
    if (R_FAILED(rc)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "fsdevMountSaveData failed: 0x%08X", rc);
        return std::string(buf);
    }

    s_mounted = true;
    return "";
}

std::string Commit() {
    if (!s_mounted) return "Not mounted";
    Result rc = fsdevCommitDevice(SAVE_MOUNT_NAME);
    if (R_FAILED(rc)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "fsdevCommitDevice failed: 0x%08X", rc);
        return std::string(buf);
    }
    return "";
}

void Unmount() {
    if (!s_mounted) return;
    fsdevUnmountDevice(SAVE_MOUNT_NAME);
    s_mounted = false;
}

bool IsMounted() { return s_mounted; }

// Write `len` bytes of `data` to the file at the given fsdev path. Returns ""
// on success or a short error string. Empty input is treated as "skip".
static std::string WriteSeedFile(const std::string& path, const uint8_t* data, size_t len) {
    if (!data || len == 0) return "";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return "open " + path + " failed";
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    if (n != len) return "short write " + path;
    return "";
}

bool SaveExists(AccountUid uid) {
    // Probe-mount: if fsdevMountSaveData succeeds, a save already exists. We
    // unmount immediately so the caller can do whatever it wants next.
    if (s_mounted) Unmount();
    Result rc = fsdevMountSaveData(SAVE_MOUNT_NAME, (u64)TOMODACHI_TITLE_ID, uid);
    if (R_FAILED(rc)) return false;
    fsdevUnmountDevice(SAVE_MOUNT_NAME);
    return true;
}

std::string CreateAndSeed(AccountUid uid,
                          const uint8_t* mii,    size_t miiLen,
                          const uint8_t* map,    size_t mapLen,
                          const uint8_t* player, size_t playerLen) {
    if (s_mounted) Unmount();

    // Create the save container. Tomodachi Life's account save: 32 MiB data,
    // 1 MiB journal — comfortably above the ~4.5 MiB of real-save payload and
    // safe for future patches that grow the format.
    FsSaveDataAttribute attr = {};
    attr.application_id  = (u64)TOMODACHI_TITLE_ID;
    attr.uid             = uid;
    attr.save_data_type  = FsSaveDataType_Account;
    attr.save_data_rank  = FsSaveDataRank_Primary;
    attr.save_data_index = 0;

    FsSaveDataCreationInfo info = {};
    info.save_data_size     = 32 * 1024 * 1024;
    info.journal_size       = 1  * 1024 * 1024;
    info.available_size     = 0;
    info.owner_id           = (u64)TOMODACHI_TITLE_ID;
    info.flags              = 0;
    info.save_data_space_id = FsSaveDataSpaceId_User;

    FsSaveDataMetaInfo meta = {};
    meta.size = 0;
    meta.type = FsSaveDataMetaType_None;

    Result rc = fsCreateSaveDataFileSystem(&attr, &info, &meta);
    // Tolerate "save data already exists" — caller verifies emptiness or
    // accepts overwrite. Two different result codes show up in practice
    // depending on firmware/path:
    //   0x4ce02 — MAKERESULT(FS, 615), classic FsError_PathAlreadyExists
    //   0x00402 — MAKERESULT(FS, 2),   newer firmwares emit this when the
    //             save container for (titleId, uid) already exists. Without
    //             tolerating it the "overwrite an existing user's save"
    //             flow fails with "fsCreateSaveDataFileSystem failed:
    //             0x00000402".
    if (R_FAILED(rc)
        && (u32)rc != 0x4ce02
        && (u32)rc != 0x00402) {
        char buf[80];
        snprintf(buf, sizeof(buf), "fsCreateSaveDataFileSystem failed: 0x%08X", rc);
        return std::string(buf);
    }

    rc = fsdevMountSaveData(SAVE_MOUNT_NAME, (u64)TOMODACHI_TITLE_ID, uid);
    if (R_FAILED(rc)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "post-create mount failed: 0x%08X", rc);
        return std::string(buf);
    }
    s_mounted = true;

    // Tomodachi Life expects Ugc/ and PhotoStudio/ directories alongside the
    // .sav files; create them even when seed contents are empty.
    mkdir(SAVE_MOUNT_NAME ":/Ugc", 0777);
    mkdir(SAVE_MOUNT_NAME ":/PhotoStudio", 0777);

    std::string e;
    e = WriteSeedFile(SAVE_MOUNT_NAME ":/Mii.sav",    mii,    miiLen);    if (!e.empty()) return e;
    e = WriteSeedFile(SAVE_MOUNT_NAME ":/Map.sav",    map,    mapLen);    if (!e.empty()) return e;
    e = WriteSeedFile(SAVE_MOUNT_NAME ":/Player.sav", player, playerLen); if (!e.empty()) return e;

    Result crc = fsdevCommitDevice(SAVE_MOUNT_NAME);
    if (R_FAILED(crc)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "commit after seed failed: 0x%08X", crc);
        return std::string(buf);
    }
    return "";
}

std::string FormatUid(const AccountUid& uid) {
    char buf[40];
    snprintf(buf, sizeof(buf), "%016llx%016llx",
             (unsigned long long)uid.uid[0],
             (unsigned long long)uid.uid[1]);
    return std::string(buf);
}

std::string SafeUserFolder(const UserInfo& user) {
    // Sanitize nickname → ASCII subset that every FAT/exFAT/NTFS filesystem
    // accepts. Walk one UTF-8 byte at a time; bytes >= 0x80 (any non-ASCII)
    // get replaced with '_' so we never split a multi-byte codepoint and
    // emit half of it. Forbidden ASCII chars (slash, colon, asterisk, etc.)
    // and control chars also become '_'.
    std::string clean;
    clean.reserve(user.nickname.size());
    bool lastWasUnder = false;
    for (unsigned char c : user.nickname) {
        bool ok =
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == ' ';
        if (ok) {
            clean.push_back((char)c);
            lastWasUnder = false;
        } else {
            if (!lastWasUnder) {
                clean.push_back('_');
                lastWasUnder = true;
            }
        }
    }
    // Trim leading/trailing spaces and underscores so we never produce
    // hidden-looking names like "  _Bob" or "Bob_  ".
    auto isTrim = [](char c) { return c == ' ' || c == '_'; };
    while (!clean.empty() && isTrim(clean.front())) clean.erase(clean.begin());
    while (!clean.empty() && isTrim(clean.back()))  clean.pop_back();
    if (clean.empty()) clean = "User";

    // Append a short UID suffix so two accounts with the same nickname (or
    // the same account after a rename) never share a folder. 8 hex chars
    // = 32 bits, more than enough to disambiguate the handful of accounts
    // that fit on one Switch.
    std::string uidHex = FormatUid(user.uid);
    std::string shortHex = (uidHex.size() >= 8)
        ? uidHex.substr(uidHex.size() - 8) : uidHex;
    return clean + "_" + shortHex;
}

std::string GetLocalIP() {
    // NOTE: nifmInitialize is called once by main() before SDL init.
    // Do NOT call nifmInitialize/nifmExit here – it would tear down the
    // service that main() owns, breaking subsequent socket operations.
    u32 ip = 0;
    Result rc = nifmGetCurrentIpAddress(&ip);
    if (R_FAILED(rc) || ip == 0) return "";
    char buf[32];
    snprintf(buf, sizeof(buf), "%d.%d.%d.%d",
             (ip >> 0) & 0xFF,
             (ip >> 8) & 0xFF,
             (ip >> 16) & 0xFF,
             (ip >> 24) & 0xFF);
    return std::string(buf);
}

} // namespace SaveMount
