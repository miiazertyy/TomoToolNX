// backup.cpp – no std::filesystem (-fno-exceptions incompatible)
#include "backup.h"
#include <string>
#include <vector>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <switch.h>   // Thread, Mutex

// ─── POSIX helpers ────────────────────────────────────────────────────────────

static void MkdirP(const std::string& path) { mkdir(path.c_str(), 0777); }

[[maybe_unused]] static std::string Filename(const std::string& path) {
    size_t sl = path.find_last_of("/\\");
    return (sl == std::string::npos) ? path : path.substr(sl + 1);
}
[[maybe_unused]] static bool FileExists(const std::string& p) {
    struct stat st; return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
static bool DirExists(const std::string& p) {
    struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Returns path for a new timestamped backup folder: BACKUP_ROOT/save_DD-MM-YYYY_HH-MM
static std::string MakeTimestampedBackupPath() {
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s/save_%02d-%02d-%04d_%02d-%02d",
             BACKUP_ROOT,
             t->tm_mday, t->tm_mon + 1, t->tm_year + 1900,
             t->tm_hour, t->tm_min);
    return buf;
}

// Returns all save_* backup folders under BACKUP_ROOT, sorted oldest-first by mtime.
static std::vector<std::string> ListSaveBackups() {
    std::vector<std::pair<time_t,std::string>> entries;
    DIR* d = opendir(BACKUP_ROOT);
    if (!d) return {};
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        if (strncmp(de->d_name, "save_", 5) != 0) continue;
        std::string full = std::string(BACKUP_ROOT) + "/" + de->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            entries.push_back({st.st_mtime, full});
    }
    closedir(d);
    std::sort(entries.begin(), entries.end());
    std::vector<std::string> result;
    for (auto& e : entries) result.push_back(e.second);
    return result;
}

static void CopyFile(const std::string& src, const std::string& dst) {
    FILE* in  = fopen(src.c_str(), "rb");
    FILE* out = fopen(dst.c_str(), "wb");
    if (!in || !out) { if(in) fclose(in); if(out) fclose(out); return; }
    char buf[32768];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
    fclose(in); fclose(out);
}

// Delete a directory tree recursively
static void RmRf(const std::string& path) {
    DIR* d = opendir(path.c_str());
    if (!d) { remove(path.c_str()); return; }
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        if (!strcmp(de->d_name,".") || !strcmp(de->d_name,"..")) continue;
        std::string child = path + "/" + de->d_name;
        struct stat st;
        if (stat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            RmRf(child);
        else
            remove(child.c_str());
    }
    closedir(d);
    rmdir(path.c_str());
}

// Count regular files under a directory (for progress denominator)
static int CountFiles(const std::string& path) {
    int total = 0;
    DIR* d = opendir(path.c_str());
    if (!d) return 0;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        if (!strcmp(de->d_name,".") || !strcmp(de->d_name,"..")) continue;
        std::string child = path + "/" + de->d_name;
        struct stat st;
        if (stat(child.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) total += CountFiles(child);
        else total++;
    }
    closedir(d);
    return total;
}

// ─── Async backup state ───────────────────────────────────────────────────────

static Thread  s_thread;
static bool    s_threadActive = false;
static Mutex   s_mutex;
static float   s_progress  = 0.0f;
static bool    s_done      = false;
static std::string s_error;
static std::string s_saveRoot;
static std::string s_backupDest;

// Recursive copy — copies src tree into dst (dst must already exist)
static void CopyTree(const std::string& src, const std::string& dst,
                     int& done, int total) {
    DIR* d = opendir(src.c_str());
    if (!d) return;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        if (!strcmp(de->d_name,".") || !strcmp(de->d_name,"..")) continue;
        std::string srcChild = src + "/" + de->d_name;
        std::string dstChild = dst + "/" + de->d_name;
        struct stat st;
        if (stat(srcChild.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            MkdirP(dstChild);
            CopyTree(srcChild, dstChild, done, total);
        } else {
            CopyFile(srcChild, dstChild);
            done++;
            if (total > 0) {
                mutexLock(&s_mutex);
                s_progress = (float)done / total;
                mutexUnlock(&s_mutex);
            }
        }
    }
    closedir(d);
}

static void BackupThreadFunc(void*) {
    std::string root = s_saveRoot;
    std::string dest = s_backupDest;

    MkdirP(BACKUP_ROOT);
    MkdirP(dest);

    int total = CountFiles(root);
    int done  = 0;

    CopyTree(root, dest, done, total);

    mutexLock(&s_mutex);
    s_progress = 1.0f;
    s_done     = true;
    s_error    = "";
    mutexUnlock(&s_mutex);
}

namespace BackupService {

void StartFullBackup(const std::string& saveMountRoot) {
    mutexInit(&s_mutex);
    s_progress   = 0.0f;
    s_done       = false;
    s_error      = "";
    s_saveRoot   = saveMountRoot;
    s_backupDest = MakeTimestampedBackupPath();
    threadCreate(&s_thread, BackupThreadFunc, nullptr, nullptr, 256*1024, 0x2C, -2);
    threadStart(&s_thread);
    s_threadActive = true;
}

void Cleanup() {
    if (!s_threadActive) return;
    threadWaitForExit(&s_thread);
    threadClose(&s_thread);
    s_threadActive = false;
}

float BackupProgress() {
    mutexLock(&s_mutex);
    float v = s_progress;
    mutexUnlock(&s_mutex);
    return v;
}

bool BackupDone() {
    mutexLock(&s_mutex);
    bool v = s_done;
    mutexUnlock(&s_mutex);
    return v;
}

std::string BackupError() {
    mutexLock(&s_mutex);
    std::string v = s_error;
    mutexUnlock(&s_mutex);
    return v;
}

bool HasExistingBackup() { return !ListSaveBackups().empty(); }

int CountBackups() { return (int)ListSaveBackups().size(); }

void DeleteOldestBackup() {
    auto backups = ListSaveBackups();
    if (!backups.empty()) RmRf(backups[0]);
}

bool DeleteBackupAt(const std::string& backupPath) {
    if (backupPath.empty()) return false;
    // Safety: refuse to touch anything outside our own backup root.
    const std::string root = BACKUP_ROOT;
    if (backupPath.size() <= root.size() + 1) return false;
    if (backupPath.compare(0, root.size(), root) != 0) return false;
    if (backupPath[root.size()] != '/') return false;
    // Refuse "../" escapes in the trailing path.
    if (backupPath.find("/..") != std::string::npos) return false;
    RmRf(backupPath);
    return true;
}

void DeleteBackup() { RmRf(BACKUP_ROOT); }


static std::string RestoreCopyTree(const std::string& src, const std::string& dst) {
    DIR* d = opendir(src.c_str());
    if (!d) return "Cannot read: " + src;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        if (!strcmp(de->d_name,".") || !strcmp(de->d_name,"..")) continue;
        std::string sc = src + "/" + de->d_name;
        std::string dc = dst + "/" + de->d_name;
        struct stat st;
        if (stat(sc.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            MkdirP(dc);
            std::string err = RestoreCopyTree(sc, dc);
            if (!err.empty()) { closedir(d); return err; }
        } else {
            CopyFile(sc, dc);
        }
    }
    closedir(d);
    return "";
}

std::vector<std::string> ListBackups() {
    auto v = ListSaveBackups();
    std::reverse(v.begin(), v.end()); // newest first
    return v;
}

std::string RestoreBackup(const std::string& backupPath, const std::string& saveMountRoot) {
    if (!DirExists(backupPath)) return "Backup folder not found";
    return RestoreCopyTree(backupPath, saveMountRoot);
}

} // namespace BackupService
