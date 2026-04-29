#pragma once
// backup.h  – port of BackupService.cs

#include "ugc_scanner.h"
#include <string>

namespace BackupService {
    // Returns the backup folder path, or empty string on failure.
    std::string BackupEntry(const UgcTextureEntry& entry);
}
