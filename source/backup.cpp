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

// True recursive `mkdir -p`: walks every `/`-separated component and
// creates it if missing. The old single-call version assumed every
// ancestor of `path` already existed, which is true on real Switch SDs
// (the homebrew menu pre-creates /switch/), but not in Ryujinx fresh
// SD emulation — so a first-run write into /switch/TomoToolNX/Bundles
// silently failed with the parent never having been created, and the
// subsequent fopen for Mii.sav produced "open … failed".
static void MkdirP(const std::string& path) {
    if (path.empty()) return;
    // Skip a leading slash so we don't loop over /<empty>.
    size_t i = (path[0] == '/') ? 1 : 0;
    while (i <= path.size()) {
        if (i == path.size() || path[i] == '/') {
            std::string sub = path.substr(0, i);
            if (!sub.empty()) mkdir(sub.c_str(), 0777);
        }
        i++;
    }
}

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

// Root directory holding backups for a particular user (or the legacy global
// root when uidTag is empty). Callers are expected to MkdirP this before use.
static std::string BackupRootFor(const std::string& uidTag) {
    if (uidTag.empty()) return BACKUP_ROOT;
    return std::string(BACKUP_ROOT) + "/" + uidTag;
}

// Returns path for a new timestamped backup folder under the per-user root.
static std::string MakeTimestampedBackupPath(const std::string& uidTag) {
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char buf[96];
    snprintf(buf, sizeof(buf), "%s/save_%02d-%02d-%04d_%02d-%02d",
             BackupRootFor(uidTag).c_str(),
             t->tm_mday, t->tm_mon + 1, t->tm_year + 1900,
             t->tm_hour, t->tm_min);
    return buf;
}

// Returns all save_* backup folders inside the per-user root, sorted oldest-first by mtime.
static std::vector<std::string> ListSaveBackups(const std::string& uidTag) {
    std::vector<std::pair<time_t,std::string>> entries;
    std::string root = BackupRootFor(uidTag);
    DIR* d = opendir(root.c_str());
    if (!d) return {};
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        if (strncmp(de->d_name, "save_", 5) != 0) continue;
        std::string full = root + "/" + de->d_name;
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

// Returns "" on success, error string on any failure. Previously void;
// silent failures here meant a bundle apply / restore could quietly
// truncate a savedata file to zero bytes and the caller would never know.
//
// If `commitDev` is non-null, fsdevCommitDevice is called after the file
// is fully closed — never mid-file. We tried a chunked close/commit/reopen
// pattern to stay under Tomodachi Life's 1 MiB savedata journal cap; in
// practice fsdevCommitDevice returned non-Result-shaped errors (0xE2F27202,
// 0xE3746402) on real hardware whenever it was called between chunks of
// the same file. The bundle-apply path was reworked to route through
// SaveMount::CreateAndSeed instead (see FinishBundleApply in main.cpp);
// this function stays simple for SD-to-SD copies and post-close commits.
static std::string CopyFile(const std::string& src, const std::string& dst,
                            const char* commitDev = nullptr) {
    FILE* in  = fopen(src.c_str(), "rb");
    if (!in) return "open src " + src + " failed";
    FILE* out = fopen(dst.c_str(), "wb");
    if (!out) { fclose(in); return "open dst " + dst + " failed"; }
    char buf[32768];
    size_t n;
    size_t totalRead = 0, totalWritten = 0;
    bool writeErr = false;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        totalRead += n;
        size_t w = fwrite(buf, 1, n, out);
        totalWritten += w;
        if (w != n) { writeErr = true; break; }
    }
    bool readErr = ferror(in) != 0;
    int closeOut = fclose(out);
    fclose(in);
    if (readErr)  return "read " + src + " failed at " + std::to_string(totalRead);
    if (writeErr) return "short write " + dst
        + " (" + std::to_string(totalWritten) + "/" + std::to_string(totalRead) + ")";
    if (closeOut != 0) return "fclose " + dst + " failed";
    if (commitDev) {
        Result rc = fsdevCommitDevice(commitDev);
        if (R_FAILED(rc)) {
            char ebuf[96];
            snprintf(ebuf, sizeof(ebuf), "commit after %s failed: 0x%08X", dst.c_str(), rc);
            return std::string(ebuf);
        }
    }
    return "";
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

// Recursive copy — copies src tree into dst (dst must already exist).
// First CopyFile error is recorded into s_error; subsequent files still
// attempt to copy so the backup is as complete as possible.
static void CopyTree(const std::string& src, const std::string& dst,
                     int& done, int total) {
    DIR* d = opendir(src.c_str());
    if (!d) return;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        if (!strcmp(de->d_name,".") || !strcmp(de->d_name,"..")) continue;
        // Plain concat — `src`/`dst` for backups are SD-card paths that
        // don't end in '/', so adding "/" is safe. For savedata-mount
        // destinations see RestoreCopyTree's JoinPath helper.
        std::string srcChild = src + "/" + de->d_name;
        std::string dstChild = dst + "/" + de->d_name;
        struct stat st;
        if (stat(srcChild.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            MkdirP(dstChild);
            CopyTree(srcChild, dstChild, done, total);
        } else {
            std::string cerr = CopyFile(srcChild, dstChild);
            if (!cerr.empty()) {
                mutexLock(&s_mutex);
                if (s_error.empty()) s_error = cerr;
                mutexUnlock(&s_mutex);
            }
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

static std::string s_backupParent;

static void BackupThreadFunc(void*) {
    std::string root = s_saveRoot;
    std::string dest = s_backupDest;

    MkdirP(BACKUP_ROOT);
    if (!s_backupParent.empty() && s_backupParent != BACKUP_ROOT)
        MkdirP(s_backupParent);
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

void StartFullBackup(const std::string& saveMountRoot,
                     const std::string& uidTag) {
    mutexInit(&s_mutex);
    s_progress     = 0.0f;
    s_done         = false;
    s_error        = "";
    s_saveRoot     = saveMountRoot;
    s_backupParent = BackupRootFor(uidTag);
    s_backupDest   = MakeTimestampedBackupPath(uidTag);
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

bool HasExistingBackup(const std::string& uidTag) {
    return !ListSaveBackups(uidTag).empty();
}

int CountBackups(const std::string& uidTag) {
    return (int)ListSaveBackups(uidTag).size();
}

void DeleteOldestBackup(const std::string& uidTag) {
    auto backups = ListSaveBackups(uidTag);
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


// Path-safe join — avoids `tomodata://Mii.sav` (double slash) when the
// destination root already ends in `/`. fsdev appears to register such
// paths in a way that later confuses fsdevCommitDevice ("mid-copy commit
// failed: 0xE3746402"), so we normalize before passing into the filesystem.
static std::string JoinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

// `commitDev` (e.g. "tomodata") is forwarded into CopyFile so writes into a
// savedata mount drain the journal mid-file. Pass nullptr for plain SD->SD
// or savedata->SD copies (e.g. restoring a backup to a non-savedata path).
static std::string RestoreCopyTree(const std::string& src, const std::string& dst,
                                   const char* commitDev = nullptr) {
    DIR* d = opendir(src.c_str());
    if (!d) return "Cannot read: " + src;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        if (!strcmp(de->d_name,".") || !strcmp(de->d_name,"..")) continue;
        std::string sc = JoinPath(src, de->d_name);
        std::string dc = JoinPath(dst, de->d_name);
        struct stat st;
        if (stat(sc.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            MkdirP(dc);
            std::string err = RestoreCopyTree(sc, dc, commitDev);
            if (!err.empty()) { closedir(d); return err; }
        } else {
            std::string err = CopyFile(sc, dc, commitDev);
            if (!err.empty()) { closedir(d); return err; }
        }
    }
    closedir(d);
    return "";
}

std::vector<std::string> ListBackups(const std::string& uidTag) {
    auto v = ListSaveBackups(uidTag);
    std::reverse(v.begin(), v.end()); // newest first
    return v;
}

std::string RestoreBackup(const std::string& backupPath, const std::string& saveMountRoot) {
    if (!DirExists(backupPath)) return "Backup folder not found";
    // saveMountRoot is always "tomodata:/" in practice — the savedata mount
    // shared with SaveMount::SAVE_MOUNT_NAME. Forwarding the device name
    // lets CopyFile drain the journal mid-file so large savs don't
    // short-write into Tomodachi's 1 MiB journal cap.
    return RestoreCopyTree(backupPath, saveMountRoot, "tomodata");
}

// ── Save bundles ────────────────────────────────────────────────────────────

static bool BundleNameSafe(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    if (name.find("..") != std::string::npos) return false;
    for (char c : name) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
              || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return false;
    }
    return true;
}

static std::string WriteBlob(const std::string& path, const uint8_t* data, size_t len) {
    if (!data || len < 1024) return ""; // skip — caller may pass placeholder
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return "open " + path + " failed";
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    if (n != len) return "short write " + path;
    return "";
}

std::string WriteBundleFromSeed(const std::string& name,
                                const uint8_t* mii,    size_t miiLen,
                                const uint8_t* map,    size_t mapLen,
                                const uint8_t* player, size_t playerLen) {
    if (!BundleNameSafe(name)) return "Invalid bundle name";
    MkdirP(BUNDLE_ROOT);
    std::string root = std::string(BUNDLE_ROOT) + "/" + name;
    MkdirP(root);
    MkdirP(root + "/Ugc");
    MkdirP(root + "/PhotoStudio");
    std::string e;
    e = WriteBlob(root + "/Mii.sav",    mii,    miiLen);    if (!e.empty()) return e;
    e = WriteBlob(root + "/Map.sav",    map,    mapLen);    if (!e.empty()) return e;
    e = WriteBlob(root + "/Player.sav", player, playerLen); if (!e.empty()) return e;
    return "";
}

std::vector<std::string> ListBundles() {
    std::vector<std::pair<time_t, std::string>> entries;
    DIR* d = opendir(BUNDLE_ROOT);
    if (!d) return {};
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        std::string full = std::string(BUNDLE_ROOT) + "/" + de->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            entries.push_back({ st.st_mtime, std::string(de->d_name) });
    }
    closedir(d);
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::string> out;
    out.reserve(entries.size());
    for (auto& e2 : entries) out.push_back(e2.second);
    return out;
}

std::string ApplyBundleToMount(const std::string& bundleName,
                               const std::string& saveMountRoot) {
    if (!BundleNameSafe(bundleName)) return "Invalid bundle name";
    std::string root = std::string(BUNDLE_ROOT) + "/" + bundleName;
    if (!DirExists(root)) return "Bundle not found: " + bundleName;
    // Same journal-drain rationale as RestoreBackup above.
    return RestoreCopyTree(root, saveMountRoot, "tomodata");
}

static std::string LoadFileBytes(const std::string& path, std::vector<uint8_t>& out) {
    out.clear();
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "open " + path + " failed";
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return "ftell " + path + " failed"; }
    out.resize((size_t)sz);
    size_t n = fread(out.data(), 1, out.size(), f);
    int closeRc = fclose(f);
    if (n != out.size()) return "short read " + path
        + " (" + std::to_string(n) + "/" + std::to_string(out.size()) + ")";
    if (closeRc != 0) return "fclose " + path + " failed";
    return "";
}

std::string LoadBundleSavs(const std::string& bundleName, BundleSavs& out) {
    if (!BundleNameSafe(bundleName)) return "Invalid bundle name";
    std::string root = std::string(BUNDLE_ROOT) + "/" + bundleName;
    if (!DirExists(root)) return "Bundle not found: " + bundleName;
    std::string e;
    e = LoadFileBytes(root + "/Mii.sav",    out.mii);    if (!e.empty()) return e;
    e = LoadFileBytes(root + "/Map.sav",    out.map);    if (!e.empty()) return e;
    e = LoadFileBytes(root + "/Player.sav", out.player); if (!e.empty()) return e;
    return "";
}

bool DeleteBundleByName(const std::string& bundleName) {
    if (!BundleNameSafe(bundleName)) return false;
    std::string root = std::string(BUNDLE_ROOT) + "/" + bundleName;
    if (!DirExists(root)) return true;        // already gone — caller intent satisfied
    // Reuse the generic recursive delete used for backups; only refuses to
    // touch anything outside its BUNDLE_ROOT scope by construction of the
    // path we just built.
    RmRf(root);
    return !DirExists(root);
}

int DeleteAllBundles() {
    int removed = 0;
    DIR* d = opendir(BUNDLE_ROOT);
    if (!d) return 0;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        std::string full = std::string(BUNDLE_ROOT) + "/" + de->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        RmRf(full);
        removed++;
    }
    closedir(d);
    return removed;
}

} // namespace BackupService
