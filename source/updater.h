#pragma once
// updater.h — Auto-update from GitHub releases
// Background thread checks latest release, downloads NRO if newer.

#include <string>

#define APP_VERSION "1.2.0"  // bump this before each release
#define GITHUB_REPO "miiazertyy/TomoToolNX"
#define UPDATE_NRO_PATH "sdmc:/switch/TomoToolNX/TomoToolNX.nro"

namespace Updater {

enum class State {
    Idle,
    Checking,
    UpdateAvailable,
    NoUpdate,
    Downloading,
    Done,
    Error
};

void     StartCheck();           // Start background version check
State    GetState();
float    GetProgress();          // 0.0-1.0 during download
std::string GetLatestVersion();  // e.g. "1.0.1"
std::string GetError();
void     StartDownload();        // Call after user confirms
void     Cleanup();              // Call on exit

// Compare version strings, returns true if a > b
bool VersionGreater(const std::string& a, const std::string& b);

} // namespace Updater
