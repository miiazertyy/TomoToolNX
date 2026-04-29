// save_mount.cpp
#include "save_mount.h"
#include <cstring>
#include <cstdio>
#include <switch.h>

namespace SaveMount {

static bool s_mounted = false;

std::vector<UserInfo> GetUsers() {
    std::vector<UserInfo> users;

    Result rc = accountInitialize(AccountServiceType_Application);
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
