// backup.cpp – no std::filesystem (incompatible with -fno-exceptions)
#include "backup.h"
#include <string>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void MkdirP(const std::string& path) { mkdir(path.c_str(), 0777); }

static std::string Filename(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

static bool FileExists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static void CopyFile(const std::string& src, const std::string& dst) {
    FILE* in  = fopen(src.c_str(), "rb");
    FILE* out = fopen(dst.c_str(), "wb");
    if (!in || !out) { if(in) fclose(in); if(out) fclose(out); return; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
    fclose(in); fclose(out);
}

namespace BackupService {

static std::string GetNextNumberedFolder(const std::string& backupRoot) {
    int highest = 0, width = 3;
    DIR* dir = opendir(backupRoot.c_str());
    if (dir) {
        struct dirent* de;
        while ((de = readdir(dir)) != nullptr) {
            if (de->d_type != DT_DIR && de->d_type != DT_UNKNOWN) continue;
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
    return backupRoot + "/" + s;
}

std::string BackupEntry(const UgcTextureEntry& entry) {
    std::string backupRoot = entry.directory() + "/Backup";
    MkdirP(backupRoot);

    std::string dest = GetNextNumberedFolder(backupRoot);
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
