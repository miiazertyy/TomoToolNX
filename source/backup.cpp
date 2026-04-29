// backup.cpp – no std::filesystem (-fno-exceptions incompatible)
#include "backup.h"
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <switch.h>   // Thread, Mutex

// ─── POSIX helpers ────────────────────────────────────────────────────────────

static void MkdirP(const std::string& path) { mkdir(path.c_str(), 0777); }

static std::string Filename(const std::string& path) {
    size_t sl = path.find_last_of("/\\");
    return (sl == std::string::npos) ? path : path.substr(sl + 1);
}
static bool FileExists(const std::string& p) {
    struct stat st; return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
static bool DirExists(const std::string& p) {
    struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
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

// Recursive copy — copies src tree into dst (dst must already exist)
static void CopyTree(const std::string& src, const std::string& dst,
                     int& done, int total, float& progressOut) {
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
            CopyTree(srcChild, dstChild, done, total, progressOut);
        } else {
            CopyFile(srcChild, dstChild);
            done++;
            if (total > 0) progressOut = (float)done / total;
        }
    }
    closedir(d);
}

// ─── Async backup state ───────────────────────────────────────────────────────

static Thread  s_thread;
static Mutex   s_mutex;
static float   s_progress  = 0.0f;
static bool    s_done      = false;
static std::string s_error;
static std::string s_saveRoot;

static void BackupThreadFunc(void*) {
    std::string root = s_saveRoot;
    std::string dest = BACKUP_SAVE;

    MkdirP(BACKUP_ROOT);
    MkdirP(dest);

    // Count files for progress
    int total = CountFiles(root);
    int done  = 0;
    float prog = 0.0f;

    CopyTree(root, dest, done, total, prog);

    mutexLock(&s_mutex);
    s_progress = 1.0f;
    s_done     = true;
    s_error    = "";
    mutexUnlock(&s_mutex);
}

namespace BackupService {

void StartFullBackup(const std::string& saveMountRoot) {
    mutexInit(&s_mutex);
    s_progress = 0.0f;
    s_done     = false;
    s_error    = "";
    s_saveRoot = saveMountRoot;
    threadCreate(&s_thread, BackupThreadFunc, nullptr, nullptr, 256*1024, 0x2C, -2);
    threadStart(&s_thread);
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

bool HasExistingBackup() { return DirExists(BACKUP_SAVE); }

void DeleteBackup() { RmRf(BACKUP_ROOT); }

// ─── Per-import UGC entry backup ──────────────────────────────────────────────

static std::string GetNextNumberedFolder(const std::string& root) {
    int highest = 0, width = 3;
    DIR* dir = opendir(root.c_str());
    if (dir) {
        struct dirent* de;
        while ((de = readdir(dir)) != nullptr) {
            char* end = nullptr;
            long n = strtol(de->d_name, &end, 10);
            if (end != de->d_name && *end == '\0' && n > 0) {
                if (n > highest) highest = (int)n;
                int w = (int)strlen(de->d_name);
                if (w > width) width = w;
            }
        }
        closedir(dir);
    }
    int next = highest + 1;
    std::string s = std::to_string(next);
    while ((int)s.size() < width) s = "0" + s;
    return root + "/" + s;
}

std::string BackupEntry(const UgcTextureEntry& entry) {
    MkdirP(BACKUP_ROOT);
    MkdirP(BACKUP_IMPORTS);
    std::string dest = GetNextNumberedFolder(BACKUP_IMPORTS);
    MkdirP(dest);
    auto copyIfExists = [&](const std::string& src) {
        if (src.empty() || !FileExists(src)) return;
        CopyFile(src, dest + "/" + Filename(src));
    };
    copyIfExists(entry.ugctexPath);
    copyIfExists(entry.thumbPath);
    copyIfExists(entry.canvasPath);
    return dest;
}

} // namespace BackupService
