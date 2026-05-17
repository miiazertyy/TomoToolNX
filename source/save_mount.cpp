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
// on success or a short error string. A null buffer is treated as "skip", but
// a zero-length write is reported — it almost always means the caller passed
// a moved-from / empty vector by mistake.
static std::string WriteSeedFile(const std::string& path, const uint8_t* data, size_t len) {
    if (!data) return "";
    if (len == 0) return "zero-length seed for " + path;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return "open " + path + " failed";
    size_t n = fwrite(data, 1, len, f);
    int closeRc = fclose(f);
    if (n != len) return "short write " + path
        + " (" + std::to_string(n) + "/" + std::to_string(len) + ")";
    if (closeRc != 0) return "fclose " + path + " failed";
    // Verify the file landed at the expected size — fsdev sometimes reports
    // a clean fwrite + fclose + commit but loses the file if the underlying
    // savedata partition rejects it. Reading the stat back is the only
    // signal we have on-device.
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return "post-write stat " + path + " failed";
    if ((size_t)st.st_size != len) return "post-write size mismatch " + path
        + " (" + std::to_string((size_t)st.st_size) + "/" + std::to_string(len) + ")";
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
    // 8 MiB journal — every uncommitted seed file (Mii ~2.8 MiB, Player
    // ~1.3 MiB, Map ~0.2 MiB) has to fit in the journal until the next
    // fsdevCommitDevice flush; 1 MiB short-wrote the first file. The data
    // partition is still 32 MiB, comfortably above the ~4.5 MiB payload.
    FsSaveDataAttribute attr = {};
    attr.application_id  = (u64)TOMODACHI_TITLE_ID;
    attr.uid             = uid;
    attr.save_data_type  = FsSaveDataType_Account;
    attr.save_data_rank  = FsSaveDataRank_Primary;
    attr.save_data_index = 0;

    // Force-delete any existing container before creating, so the journal
    // size we set below actually takes effect. A container created by a
    // previous (failed) attempt keeps its original journal_size — that's
    // baked into the container metadata at creation and can't be resized.
    // Without this delete-first step, the user's first failed attempt with
    // a 1 MiB journal would haunt every subsequent retry: fsCreate returns
    // "already exists" (which we tolerate), we mount the old container,
    // and Mii.sav writes still short-write into the original tiny journal.
    // Every caller of CreateAndSeed has already decided to overwrite (no
    // save / partial save / bundle apply), so destroying the existing
    // container here is the intended behaviour.
    Result drc = fsDeleteSaveDataFileSystemBySaveDataAttribute(
        FsSaveDataSpaceId_User, &attr);
    // 0x7D402 = no save data exists for this (titleId, uid), which is the
    // happy path for a brand-new user. Anything else is fine to ignore too
    // (e.g. permission edge cases) — we'll surface the real error from
    // fsCreateSaveDataFileSystem if creation itself fails afterwards.
    (void)drc;

    FsSaveDataCreationInfo info = {};
    info.save_data_size     = 32 * 1024 * 1024;
    info.journal_size       = 8  * 1024 * 1024;
    info.available_size     = 0;
    info.owner_id           = (u64)TOMODACHI_TITLE_ID;
    info.flags              = 0;
    info.save_data_space_id = FsSaveDataSpaceId_User;

    FsSaveDataMetaInfo meta = {};
    meta.size = 0;
    meta.type = FsSaveDataMetaType_None;

    Result rc = fsCreateSaveDataFileSystem(&attr, &info, &meta);
    // We just deleted the container, so "already exists" tolerance is now
    // a backstop rather than the main path (some firmwares may race the
    // delete + create on the same tick).
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

    // Commit after each file so the journal doesn't accumulate the full
    // ~4.5 MiB payload in one transaction. This is belt-and-suspenders on
    // top of the bumped journal_size above — even a smaller journal would
    // recycle cleanly between writes once each commit lands.
    auto seedAndCommit = [](const char* path, const uint8_t* data, size_t len) -> std::string {
        std::string e = WriteSeedFile(path, data, len);
        if (!e.empty()) return e;
        Result r = fsdevCommitDevice(SAVE_MOUNT_NAME);
        if (R_FAILED(r)) {
            char buf[96];
            snprintf(buf, sizeof(buf), "commit after %s failed: 0x%08X", path, r);
            return std::string(buf);
        }
        return "";
    };

    std::string e;
    e = seedAndCommit(SAVE_MOUNT_NAME ":/Mii.sav",    mii,    miiLen);    if (!e.empty()) return e;
    e = seedAndCommit(SAVE_MOUNT_NAME ":/Map.sav",    map,    mapLen);    if (!e.empty()) return e;
    e = seedAndCommit(SAVE_MOUNT_NAME ":/Player.sav", player, playerLen); if (!e.empty()) return e;
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
