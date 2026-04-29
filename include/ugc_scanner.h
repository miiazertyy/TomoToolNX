#pragma once
// ugc_scanner.h  – port of UgcFolderScanner.cs + UgcTextureEntry.cs

#include <string>
#include <vector>
#include <sys/stat.h>

static inline bool PathExists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

static inline std::string ParentPath(const std::string& p) {
    size_t slash = p.find_last_of("/\\");
    return (slash == std::string::npos) ? "." : p.substr(0, slash);
}

struct UgcTextureEntry {
    std::string stem;        // e.g. "UgcFood003"
    std::string ugctexPath;
    std::string thumbPath;   // empty if not present
    std::string canvasPath;  // empty if not present

    std::string directory() const {
        return ParentPath(ugctexPath);
    }
    bool hasThumb() const  { return !thumbPath.empty()  && PathExists(thumbPath); }
    bool hasCanvas() const { return !canvasPath.empty() && PathExists(canvasPath); }
    const std::string& displayName() const { return stem; }
};

namespace UgcScanner {

std::vector<UgcTextureEntry> Scan(const std::string& folderPath);

} // namespace UgcScanner
