// mtp_server.cpp — MTP file-sharing via ITotalJustice's libhaze
// Modelled directly on the libhaze example (example/source/main.cpp @ commit 81154c1).
// The previous versions invented a fake API; this one uses haze::Initialize / haze::Exit
// exactly as the library expects.

#include "mtp_server.h"
#include "haze.h"
#include <switch.h>
#include <strings.h>
#include <cstring>
#include <cstdio>
#include <vector>
#include <memory>

#define R_SUCCEED()  return (Result)0
#define R_THROW(_rc) return (_rc)
#define R_TRY(r)     { if (const auto _rc = (r); R_FAILED(_rc)) R_THROW(_rc); }

namespace MtpServer {
namespace {

// ─── FsNative ────────────────────────────────────────────────────────────────
// Mirrors libhaze's example FsNative exactly.
// Key point: FixPath does NOT prepend '/'. When the storage name is empty (""),
// the path passes through unchanged. The previous implementations prepended "/"
// to a path already starting with "/", producing "//foo" → kernel error.
struct FsNative : haze::FileSystemProxyImpl {

    FsNative() = default;
    FsNative(FsFileSystem* fs, bool own) : m_own(own) { m_fs = *fs; }

    ~FsNative() {
        fsFsCommit(&m_fs);
        if (m_own) fsFsClose(&m_fs);
    }

    const char* FixPath(const char* path, char* out = nullptr) const {
        static char buf[FS_MAX_PATH];
        if (!out) out = buf;
        const size_t len = std::strlen(GetName());
        if (len && !strncasecmp(path, GetName(), len)) {
            std::snprintf(out, FS_MAX_PATH, "/%s", path + len);
        } else {
            std::strcpy(out, path);   // pass through — do NOT prepend '/'
        }
        return out;
    }

    Result GetTotalSpace(const char* p, s64* out) override {
        return fsFsGetTotalSpace(&m_fs, FixPath(p), out);
    }
    Result GetFreeSpace(const char* p, s64* out) override {
        return fsFsGetFreeSpace(&m_fs, FixPath(p), out);
    }

    Result GetEntryType(const char* p, haze::FileAttrType* out) override {
        FsDirEntryType t;
        R_TRY(fsFsGetEntryType(&m_fs, FixPath(p), &t));
        *out = (t == FsDirEntryType_Dir) ? haze::FileAttrType_DIR : haze::FileAttrType_FILE;
        R_SUCCEED();
    }

    Result GetEntryAttributes(const char* p, haze::FileAttr* out) override {
        const char* fp = FixPath(p);
        FsDirEntryType t;
        R_TRY(fsFsGetEntryType(&m_fs, fp, &t));

        if (t == FsDirEntryType_File) {
            out->type = haze::FileAttrType_FILE;
            FsTimeStampRaw ts{};
            if (R_SUCCEEDED(fsFsGetFileTimeStampRaw(&m_fs, fp, &ts)) && ts.is_valid) {
                out->ctime = ts.created;
                out->mtime = ts.modified;
            }
            FsFile f;
            R_TRY(fsFsOpenFile(&m_fs, fp, FsOpenMode_Read, &f));
            s64 sz{};
            fsFileGetSize(&f, &sz);
            fsFileClose(&f);
            out->size = sz;
        } else {
            out->type = haze::FileAttrType_DIR;
        }
        if (IsReadOnly()) out->flag |= haze::FileAttrFlag_READ_ONLY;
        R_SUCCEED();
    }

    Result CreateFile(const char* p, s64 size) override {
        u32 flags = (size >= (s64)0x100000000LL) ? FsCreateOption_BigFile : 0;
        // Pass size=0: allocating upfront blocks long enough to cause MTP timeouts.
        // SetFileSize is called afterwards by libhaze's threaded transfer.
        return fsFsCreateFile(&m_fs, FixPath(p), 0, flags);
    }
    Result DeleteFile(const char* p) override {
        return fsFsDeleteFile(&m_fs, FixPath(p));
    }
    Result RenameFile(const char* o, const char* n) override {
        char tmp[FS_MAX_PATH];
        return fsFsRenameFile(&m_fs, FixPath(o, tmp), FixPath(n));
    }

    Result OpenFile(const char* p, haze::FileOpenMode mode, haze::File* out) override {
        const u32 flags = (mode == haze::FileOpenMode_WRITE)
            ? (FsOpenMode_Write | FsOpenMode_Append) : FsOpenMode_Read;
        auto* f = new FsFile{};
        if (const auto rc = fsFsOpenFile(&m_fs, FixPath(p), flags, f); R_FAILED(rc)) {
            delete f; return rc;
        }
        out->impl = f;
        R_SUCCEED();
    }
    Result GetFileSize(haze::File* f, s64* out) override {
        return fsFileGetSize(static_cast<FsFile*>(f->impl), out);
    }
    Result SetFileSize(haze::File* f, s64 sz) override {
        return fsFileSetSize(static_cast<FsFile*>(f->impl), sz);
    }
    Result ReadFile(haze::File* f, s64 off, void* buf, u64 sz, u64* read) override {
        return fsFileRead(static_cast<FsFile*>(f->impl), off, buf, sz, FsReadOption_None, read);
    }
    Result WriteFile(haze::File* f, s64 off, const void* buf, u64 sz) override {
        return fsFileWrite(static_cast<FsFile*>(f->impl), off, buf, sz, FsWriteOption_None);
    }
    void CloseFile(haze::File* f) override {
        if (auto* p = static_cast<FsFile*>(f->impl)) {
            fsFileClose(p); delete p; f->impl = nullptr;
        }
    }

    Result CreateDirectory(const char* p) override {
        return fsFsCreateDirectory(&m_fs, FixPath(p));
    }
    Result DeleteDirectoryRecursively(const char* p) override {
        return fsFsDeleteDirectoryRecursively(&m_fs, FixPath(p));
    }
    Result RenameDirectory(const char* o, const char* n) override {
        char tmp[FS_MAX_PATH];
        return fsFsRenameDirectory(&m_fs, FixPath(o, tmp), FixPath(n));
    }
    Result OpenDirectory(const char* p, haze::Dir* out) override {
        auto* d = new FsDir{};
        if (const auto rc = fsFsOpenDirectory(&m_fs, FixPath(p),
                FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles | FsDirOpenMode_NoFileSize, d);
            R_FAILED(rc)) { delete d; return rc; }
        out->impl = d;
        R_SUCCEED();
    }
    Result ReadDirectory(haze::Dir* d, s64* total, size_t max, haze::DirEntry* buf) override {
        std::vector<FsDirectoryEntry> entries(max);
        R_TRY(fsDirRead(static_cast<FsDir*>(d->impl), total, entries.size(), entries.data()));
        for (s64 i = 0; i < *total; i++) std::strcpy(buf[i].name, entries[i].name);
        R_SUCCEED();
    }
    Result GetDirectoryEntryCount(haze::Dir* d, s64* out) override {
        return fsDirGetEntryCount(static_cast<FsDir*>(d->impl), out);
    }
    void CloseDirectory(haze::Dir* d) override {
        if (auto* p = static_cast<FsDir*>(d->impl)) {
            fsDirClose(p); delete p; d->impl = nullptr;
        }
    }

protected:
    FsFileSystem m_fs{};
    bool m_own{true};
};

// ─── FsSdmc ──────────────────────────────────────────────────────────────────
// Borrows the already-mounted "sdmc" device — same as the libhaze example.
struct FsSdmc final : FsNative {
    FsSdmc() : FsNative(fsdevGetDeviceFileSystem("sdmc"), false) {}
    const char* GetName()        const override { return ""; }
    const char* GetDisplayName() const override { return "micro SD Card"; }
};

// ─── State ───────────────────────────────────────────────────────────────────
LogFn g_log_fn  = nullptr;
Mutex g_mutex   = {};
bool  g_running = false;
bool  g_session_open = false;
u64   g_last_activity_tick = 0;  // armGetSystemTick() of last haze callback

void HazeCallback(const haze::CallbackData* data) {
    if (!data) return;
    g_last_activity_tick = armGetSystemTick();
    char msg[FS_MAX_PATH + 64];
    switch (data->type) {
        case haze::CallbackType_OpenSession:  g_session_open = true;  std::snprintf(msg, sizeof msg, "MTP: session opened"); break;
        case haze::CallbackType_CloseSession: g_session_open = false; std::snprintf(msg, sizeof msg, "MTP: session closed"); break;
        case haze::CallbackType_WriteBegin:   std::snprintf(msg, sizeof msg, "MTP: writing %s", data->file.filename); break;
        case haze::CallbackType_ReadBegin:    std::snprintf(msg, sizeof msg, "MTP: reading %s", data->file.filename); break;
        case haze::CallbackType_CreateFile:   std::snprintf(msg, sizeof msg, "MTP: create %s", data->file.filename); break;
        case haze::CallbackType_DeleteFile:   std::snprintf(msg, sizeof msg, "MTP: delete %s", data->file.filename); break;
        case haze::CallbackType_RenameFile:   std::snprintf(msg, sizeof msg, "MTP: rename %s→%s", data->rename.filename, data->rename.newname); break;
        case haze::CallbackType_CreateFolder: std::snprintf(msg, sizeof msg, "MTP: mkdir %s", data->file.filename); break;
        case haze::CallbackType_DeleteFolder: std::snprintf(msg, sizeof msg, "MTP: rmdir %s", data->file.filename); break;
        default: return;
    }
    if (g_log_fn) g_log_fn(msg, true);
}

} // namespace

// ─── Public API ──────────────────────────────────────────────────────────────

void SetLogCallback(LogFn fn) {
    g_log_fn = std::move(fn);
}

bool Init() {
    mutexLock(&g_mutex);
    if (g_running) { mutexUnlock(&g_mutex); return false; }

    haze::FsEntries entries;
    entries.emplace_back(std::make_shared<FsSdmc>());

    // haze::Initialize spins up its own thread internally.
    // Returns false if already running (shouldn't happen given our g_running guard).
    const bool ok = haze::Initialize(HazeCallback, entries, 0x057e, 0x201d, false);
    if (ok) g_running = true;
    mutexUnlock(&g_mutex);

    if (g_log_fn) g_log_fn(ok ? "MTP: file sharing started" : "MTP: failed to start", ok);
    return ok;
}

bool IsRunning() {
    mutexLock(&g_mutex);
    const bool r = g_running;
    mutexUnlock(&g_mutex);
    return r;
}

bool IsSessionOpen() {
    return g_session_open;
}

void PollUsbState() {
    if (!g_session_open) return;
    // Windows often doesn't send CloseSession when closing File Explorer.
    // If we haven't seen any MTP callback in 10 seconds since the session opened,
    // assume the PC disconnected and clear the active flag.
    u64 now = armGetSystemTick();
    u64 freq = armGetSystemTickFreq();
    if (g_last_activity_tick > 0 && (now - g_last_activity_tick) > freq * 10) {
        g_session_open = false;
        if (g_log_fn) g_log_fn("MTP: disconnected", true);
    }
}

void Exit() {
    mutexLock(&g_mutex);
    if (!g_running) { mutexUnlock(&g_mutex); return; }
    haze::Exit();   // blocks until the internal thread exits
    g_running = false;
    g_session_open = false;
    g_last_activity_tick = 0;
    mutexUnlock(&g_mutex);

    if (g_log_fn) g_log_fn("MTP: file sharing stopped", true);
}

} // namespace MtpServer
