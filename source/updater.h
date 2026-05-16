#pragma once
// updater.h — Check GitHub for a newer release and tell the user where to grab it.

#include <string>

#define APP_VERSION "1.3.4"  // bump this before each release
#define GITHUB_REPO "miiazertyy/TomoToolNX"

namespace Updater {

enum class State {
    Idle,
    Checking,
    UpdateAvailable,
    NoUpdate,
    Error
};

void     StartCheck();           // Start background version check
State    GetState();
std::string GetLatestVersion();  // e.g. "1.0.1"
std::string GetError();
void     Cleanup();              // Call on exit

// Compare version strings, returns true if a > b
bool VersionGreater(const std::string& a, const std::string& b);

} // namespace Updater
