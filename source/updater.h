#pragma once
// updater.h — Finds the newest TomoToolNX release on GitHub, downloads it and
// installs it over this copy.
//
// The version this build reports comes from the release tag it was built from
// (the Makefile stamps it in: `make APP_VERSION=2.0.1`, which the GitHub
// workflow does for a tag). A build that is not a release is a development
// build, 0.0.0-dev: it sees new releases but never installs over itself.

#include <cstdint>
#include <string>

#ifndef APP_VERSION
#define APP_VERSION "0.0.0-dev"
#endif
#define GITHUB_REPO "miiazertyy/TomoToolNX"

namespace Updater {

enum class State {
    Idle,
    Checking,
    UpdateAvailable,     // a newer release exists
    NoUpdate,
    Error,
    Downloading,         // GetProgress() goes 0 → 1
    Ready,               // downloaded and checked; installs when the app closes
};

void        StartCheck();        // background check of the latest release
State       GetState();
std::string GetLatestVersion();  // "2.0.1"
std::string GetNotes();          // the release's notes (Markdown, as written on GitHub)
std::string GetError();
void        Cleanup();           // join the worker (call on exit)

// Is this a development build (it never installs over itself)?
bool IsDevBuild();
// Can the latest release be installed from here (a release build, and the
// release carries TomoToolNX.nro)?
bool CanInstall();
// Download TomoToolNX.nro next to this copy, then check its size, its SHA-256
// against the one GitHub publishes, and that it is an NRO.
void        StartDownload();
float       GetProgress();       // 0..1 while downloading
uint64_t    GetDownloadSize();   // bytes, from the release
void        CancelDownload();

// Where this copy of the app lives (argv[0] from the homebrew loader).
void SetSelfPath(const char* argv0);
// Remove what an earlier update left behind (.part, .old). Call at start.
void CleanLeftovers();
// Close the app now and start the new version straight away.
void RequestRestart();
bool RestartRequested();
// Called by main() once nothing holds the app's file open (after romfsExit):
// puts the downloaded copy in place of this one, and asks the loader to start
// it when a restart was requested. Does nothing without a Ready download.
void FinishInstall();

// True if version a is newer than b. A leading "v" is ignored, and so is
// anything after the numbers ("2.0.1-beta" counts as 2.0.1).
bool VersionGreater(const std::string& a, const std::string& b);

} // namespace Updater
