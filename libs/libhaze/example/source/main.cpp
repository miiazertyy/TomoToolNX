#define NXLINK_LOG 0

#include <switch.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#if NXLINK_LOG
#include <unistd.h>
#endif
#include "haze.h"

namespace {

#define R_SUCCEED() return (Result)0

#define R_THROW(_rc) return _rc

#define R_TRY_RESULT(r, _result) { \
    if (const auto _rc = (r); R_FAILED(_rc)) { \
        R_THROW(_result); \
    } \
}

#define R_TRY(r) { \
    if (const auto _rc = (r); R_FAILED(_rc)) { \
        R_THROW(_rc); \
    } \
}

#define R_UNLESS(expr, res) { \
    if (!(expr)) { \
        R_THROW(res); \
    } \
}

#define CONCATENATE_IMPL(s1, s2) s1##s2
#define CONCATENATE(s1, s2) CONCATENATE_IMPL(s1, s2)
#define ANONYMOUS_VARIABLE(pref) CONCATENATE(pref, __COUNTER__)

template<typename Function>
struct ScopeGuard {
    ScopeGuard(Function&& function) : m_function(std::forward<Function>(function)) {

    }
    ~ScopeGuard() {
        m_function();
    }

    ScopeGuard(const ScopeGuard&) = delete;
    void operator=(const ScopeGuard&) = delete;

private:
    const Function m_function;
};

#define ON_SCOPE_EXIT(_f) ScopeGuard ANONYMOUS_VARIABLE(SCOPE_EXIT_STATE_){[&] { _f; }};

Mutex g_mutex;
std::vector<haze::CallbackData> g_callback_data;

struct FsNative : haze::FileSystemProxyImpl {
    using File = FsFile;
    using Dir = FsDir;
    using DirEntry = FsDirectoryEntry;

    FsNative() = default;
    FsNative(FsFileSystem* fs, bool own) {
        m_fs = *fs;
        m_own = own;
    }

    ~FsNative() {
        fsFsCommit(&m_fs);
        if (m_own) {
            fsFsClose(&m_fs);
        }
    }

    auto FixPath(const char* path, char* out = nullptr) const -> const char* {
        static char buf[FS_MAX_PATH];
        const auto len = std::strlen(GetName());

        if (!out) {
            out = buf;
        }

        if (len && !strncasecmp(path, GetName(), len)) {
            std::snprintf(out, sizeof(buf), "/%s", path + len);
        } else {
            std::strcpy(out, path);
        }

        return out;
    }

    Result GetTotalSpace(const char *path, s64 *out) override {
        return fsFsGetTotalSpace(&m_fs, FixPath(path), out);
    }

    Result GetFreeSpace(const char *path, s64 *out) override {
        return fsFsGetFreeSpace(&m_fs, FixPath(path), out);
    }

    Result GetEntryType(const char *path, haze::FileAttrType *out_entry_type) override {
        FsDirEntryType type;
        R_TRY(fsFsGetEntryType(&m_fs, FixPath(path), &type));
        *out_entry_type = (type == FsDirEntryType_Dir) ? haze::FileAttrType_DIR : haze::FileAttrType_FILE;
        R_SUCCEED();
    }

    Result GetEntryAttributes(const char *path, haze::FileAttr *out) override {
        FsDirEntryType type;
        R_TRY(fsFsGetEntryType(&m_fs, FixPath(path), &type));

        if (type == FsDirEntryType_File) {
            out->type = haze::FileAttrType_FILE;

            // it doesn't matter if this fails.
            FsTimeStampRaw timestamp{};
            if (R_SUCCEEDED(fsFsGetFileTimeStampRaw(&m_fs, FixPath(path), &timestamp)) && timestamp.is_valid) {
                out->ctime = timestamp.created;
                out->mtime = timestamp.modified;
            }

            FsFile file;
            R_TRY(fsFsOpenFile(&m_fs, FixPath(path), FsOpenMode_Read, &file));
            ON_SCOPE_EXIT(fsFileClose(&file));

            s64 size;
            R_TRY(fsFileGetSize(&file, &size));
            out->size = size;
        } else {
            out->type = haze::FileAttrType_DIR;
        }

        if (IsReadOnly()) {
            out->flag |= haze::FileAttrFlag_READ_ONLY;
        }

        R_SUCCEED();
    }

    Result CreateFile(const char* path, s64 size) override {
        u32 flags = 0;
        const s64 _4_GB = 0x100000000;
        if (size >= _4_GB) {
            flags = FsCreateOption_BigFile;
        }

        // do not set the size here because it can block for too long which may cause timeouts.
        // SEE: https://github.com/ITotalJustice/libhaze/issues/1#issuecomment-3305067733
        return fsFsCreateFile(&m_fs, FixPath(path), 0, flags);
    }

    Result DeleteFile(const char* path) override {
        return fsFsDeleteFile(&m_fs, FixPath(path));
    }

    Result RenameFile(const char *old_path, const char *new_path) override {
        char temp[FS_MAX_PATH];
        return fsFsRenameFile(&m_fs, FixPath(old_path, temp), FixPath(new_path));
    }

    Result OpenFile(const char *path, haze::FileOpenMode mode, haze::File *out_file) override {
        u32 flags = FsOpenMode_Read;
        if (mode == haze::FileOpenMode_WRITE) {
            flags = FsOpenMode_Write | FsOpenMode_Append;
        }

        auto f = new File();
        const auto rc = fsFsOpenFile(&m_fs, FixPath(path), flags, f);
        if (R_FAILED(rc)) {
            delete f;
            return rc;
        }

        out_file->impl = f;
        R_SUCCEED();
    }

    Result GetFileSize(haze::File *file, s64 *out_size) override {
        auto f = static_cast<File*>(file->impl);
        return fsFileGetSize(f, out_size);
    }

    Result SetFileSize(haze::File *file, s64 size) override {
        // set to 0 if your switch freezes here when allocating a huge (1+GB) file.
        // afaik this only happens when using emuMMC + windows.
        // DM me for more info.
        #if 1
        auto f = static_cast<File*>(file->impl);
        return fsFileSetSize(f, size);
        #else
        R_SUCCEED();
        #endif
    }

    Result ReadFile(haze::File *file, s64 off, void *buf, u64 read_size, u64 *out_bytes_read) override {
        auto f = static_cast<File*>(file->impl);
        return fsFileRead(f, off, buf, read_size, FsReadOption_None, out_bytes_read);
    }

    Result WriteFile(haze::File *file, s64 off, const void *buf, u64 write_size) override {
        auto f = static_cast<File*>(file->impl);
        return fsFileWrite(f, off, buf, write_size, FsWriteOption_None);
    }

    void CloseFile(haze::File *file) override {
        auto f = static_cast<File*>(file->impl);
        if (f) {
            fsFileClose(f);
            delete f;
            file->impl = nullptr;
        }
    }

    Result CreateDirectory(const char* path) override {
        return fsFsCreateDirectory(&m_fs, FixPath(path));
    }

    Result DeleteDirectoryRecursively(const char* path) override {
        return fsFsDeleteDirectoryRecursively(&m_fs, FixPath(path));
    }

    Result RenameDirectory(const char *old_path, const char *new_path) override {
        char temp[FS_MAX_PATH];
        return fsFsRenameDirectory(&m_fs, FixPath(old_path, temp), FixPath(new_path));
    }

    Result OpenDirectory(const char *path, haze::Dir *out_dir) override {
        auto dir = new Dir();
        const auto rc = fsFsOpenDirectory(&m_fs, FixPath(path), FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles | FsDirOpenMode_NoFileSize, dir);
        if (R_FAILED(rc)) {
            delete dir;
            return rc;
        }

        out_dir->impl = dir;
        R_SUCCEED();
    }

    Result ReadDirectory(haze::Dir *d, s64 *out_total_entries, size_t max_entries, haze::DirEntry *buf) override {
        auto dir = static_cast<Dir*>(d->impl);

        std::vector<FsDirectoryEntry> entries(max_entries);
        R_TRY(fsDirRead(dir, out_total_entries, entries.size(), entries.data()));

        for (s64 i = 0; i < *out_total_entries; i++) {
            std::strcpy(buf[i].name, entries[i].name);
        }

        R_SUCCEED();
    }

    Result GetDirectoryEntryCount(haze::Dir *d, s64 *out_count) override {
        auto dir = static_cast<Dir*>(d->impl);
        return fsDirGetEntryCount(dir, out_count);
    }

    void CloseDirectory(haze::Dir *d) override {
        auto dir = static_cast<Dir*>(d->impl);
        if (dir) {
            fsDirClose(dir);
            delete dir;
            d->impl = nullptr;
        }
    }

    FsFileSystem m_fs{};
    bool m_own{true};
};

struct FsSdmc final : FsNative {
    FsSdmc() : FsNative(fsdevGetDeviceFileSystem("sdmc"), false) {
    }

    const char* GetName() const {
        return "";
    }
    const char* GetDisplayName() const {
        return "micro SD Card";
    }
};

struct FsAlbum final : FsNative {
    FsAlbum(FsImageDirectoryId id) {
        fsOpenImageDirectoryFileSystem(&m_fs, id);
    }

    const char* GetName() const {
        return "album:/";
    }
    const char* GetDisplayName() const {
        return "Album";
    }

    bool IsReadOnly() override { return true; }
};

void callbackHandler(const haze::CallbackData* data) {
    mutexLock(&g_mutex);
        g_callback_data.emplace_back(*data);
    mutexUnlock(&g_mutex);
}

void processEvents() {
    std::vector<haze::CallbackData> data;

    mutexLock(&g_mutex);
        std::swap(data, g_callback_data);
    mutexUnlock(&g_mutex);

    // log events
    for (const auto& e : data) {
        switch (e.type) {
            case haze::CallbackType_OpenSession: std::printf("\nOpening Session\n\n"); break;
            case haze::CallbackType_CloseSession: std::printf("\nClosing Session\n\n"); break;

            case haze::CallbackType_CreateFile: std::printf("Creating File: %s\n", e.file.filename); break;
            case haze::CallbackType_DeleteFile: std::printf("Deleting File: %s\n", e.file.filename); break;

            case haze::CallbackType_RenameFile: std::printf("Rename File: %s -> %s\n", e.rename.filename, e.rename.newname); break;
            case haze::CallbackType_RenameFolder: std::printf("Rename Folder: %s -> %s\n", e.rename.filename, e.rename.newname); break;

            case haze::CallbackType_CreateFolder: std::printf("Creating Folder: %s\n", e.file.filename); break;
            case haze::CallbackType_DeleteFolder: std::printf("Deleting Folder: %s\n", e.file.filename); break;

            case haze::CallbackType_ReadBegin: std::printf("Reading File Begin: %s \n\n", e.file.filename); break;
            case haze::CallbackType_ReadProgress: std::printf("\tReading File: offset: %lld size: %lld\r", e.progress.offset, e.progress.size); break;
            case haze::CallbackType_ReadEnd: std::printf("\nReading File Finished: %s\n\n", e.file.filename); break;

            case haze::CallbackType_WriteBegin: std::printf("Writing File Begin: %s \n\n", e.file.filename); break;
            case haze::CallbackType_WriteProgress: std::printf("\tWriting File: offset: %lld size: %lld\r", e.progress.offset, e.progress.size); break;
            case haze::CallbackType_WriteEnd: std::printf("\nWriting File Finished: %s\n\n", e.file.filename); break;
        }
    }

    consoleUpdate(NULL);
}

} // namespace

int main(int argc, char** argv) {
    #if NXLINK_LOG
    socketInitializeDefault();
    int fd = nxlinkStdio();
    #endif

    haze::FsEntries fs_entries;
    fs_entries.emplace_back(std::make_shared<FsSdmc>());
    fs_entries.emplace_back(std::make_shared<FsAlbum>(FsImageDirectoryId_Sd));

    // default vid/pid for switch.
    const u16 vid = 0x057e;
    const u16 pid = 0x201d;
    // if set, logs will be written to sdmc:/haze_log.txt
    // logs are buffered, so you must first exit libhaze in order to read the log file.
    // you can change this behavoir by editing source/log.cpp BUFFERED_LOG to 0.
    // note that if you do this, the performance will suffer greatly.
    const bool enable_log = true;

    mutexInit(&g_mutex);
    haze::Initialize(callbackHandler, fs_entries, vid, pid, enable_log); // init libhaze (creates thread)
    consoleInit(NULL); // console to display to the screen

    // init controller
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    std::printf("libhaze example TEST v10!\n\nPress (+) to exit\n");
    consoleUpdate(NULL);

    // loop until + button is pressed
    while (appletMainLoop()) {
        padUpdate(&pad);

        const u64 kDown = padGetButtonsDown(&pad);
        if (kDown & HidNpadButton_Plus)
            break; // break in order to return to hbmenu

        processEvents();
        svcSleepThread(1e+9/60);
    }

    #if NXLINK_LOG
    close(fd);
    socketExit();
    #endif
    consoleExit(NULL); // exit console display
    haze::Exit(); // signals libhaze to exit, closes thread
}

extern "C" {

// called before main
void userAppInit(void) {
    appletLockExit(); // block exit until everything is cleaned up
}

// called after main has exit
void userAppExit(void) {
    appletUnlockExit(); // unblocks exit to cleanly exit
}

} // extern "C"
