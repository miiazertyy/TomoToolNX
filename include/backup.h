#pragma once
// backup.h

#include "ugc_scanner.h"
#include <string>
#include <cstdint>

#define BACKUP_ROOT    "/switch/TomoToolNX/Backup"


namespace BackupService {

// ── Full save backup (runs on background thread) ─────────────────────────────

// Start an async full-save backup from saveMountRoot (e.g. "tomodata:/").
// `uidTag` scopes the backup under BACKUP_ROOT/<uidTag>/ so each Switch user
// has their own isolated history. Pass an empty string for a legacy/global
// backup landing directly in BACKUP_ROOT.
// Returns immediately. Call BackupProgress() / BackupDone() to poll.
void StartFullBackup(const std::string& saveMountRoot,
                     const std::string& uidTag = "");

// 0.0 – 1.0 progress. Only valid while !BackupDone().
float BackupProgress();

// True when the background thread has finished (success or error).
bool BackupDone();

// Wait for the backup thread to finish and release its resources.
// Safe to call even if no backup was started. Must be called before
// unmounting the save filesystem.
void Cleanup();

// Error string from the backup thread, empty on success.
std::string BackupError();

// ── Backup folder management ──────────────────────────────────────────────────

// Returns true if at least one timestamped save backup exists.
// When `uidTag` is non-empty, only that user's directory is considered.
bool HasExistingBackup(const std::string& uidTag = "");

// Returns the number of existing timestamped save backups (0..MAX_BACKUPS).
// When `uidTag` is non-empty, counts only backups inside BACKUP_ROOT/<uidTag>/.
int CountBackups(const std::string& uidTag = "");

// Delete the oldest timestamped save backup (by name order).
// When `uidTag` is non-empty, only that user's directory is touched.
void DeleteOldestBackup(const std::string& uidTag = "");

// Delete a specific timestamped save backup by its full path.
// Returns true if the path was removed (or never existed).
// Refuses to act on a path that isn't inside BACKUP_ROOT, for safety.
bool DeleteBackupAt(const std::string& backupPath);

// Delete the entire backup root folder (save backups + imports).
void DeleteBackup();

// ── Restore ───────────────────────────────────────────────────────────────────

// List all timestamped save backups, newest first (returns full paths).
// When `uidTag` is non-empty, only that user's directory is enumerated.
std::vector<std::string> ListBackups(const std::string& uidTag = "");

// Synchronously copy backupPath over saveMountRoot.
// Returns empty string on success, error message on failure.
std::string RestoreBackup(const std::string& backupPath, const std::string& saveMountRoot);

// ── Save bundles (Island Generator Phase C) ───────────────────────────────────
//
// A "bundle" is a self-contained set of three .sav files + Ugc/PhotoStudio
// directories stored under /switch/TomoToolNX/Bundles/<name>/ on the SD card,
// independent of any Switch account. Bundles can be created from the embedded
// seed save (with optional generation) and later applied to a specific user.

// Bundle root, no trailing slash.
#define BUNDLE_ROOT "/switch/TomoToolNX/Bundles"

// Write the three embedded seed .sav blobs into a new bundle directory.
// Creates BUNDLE_ROOT/<name>/{Mii.sav,Map.sav,Player.sav,Ugc/,PhotoStudio/}.
// Returns "" on success or an error string.
std::string WriteBundleFromSeed(const std::string& name,
                                const uint8_t* mii,    size_t miiLen,
                                const uint8_t* map,    size_t mapLen,
                                const uint8_t* player, size_t playerLen);

// List all bundle directory names under BUNDLE_ROOT, newest first.
std::vector<std::string> ListBundles();

// Copy the bundle's files into saveMountRoot. Mirrors RestoreBackup but
// sourced from BUNDLE_ROOT/<name>/.
std::string ApplyBundleToMount(const std::string& bundleName,
                               const std::string& saveMountRoot);

// Recursively delete a bundle directory by name. No-op if it doesn't exist.
// Used to keep bundles ephemeral — once a bundle has been applied (or the
// user dismissed the apply picker), the on-disk copy is no longer needed
// and shouldn't accumulate ~4.5 MB per generation in BUNDLE_ROOT.
// Returns true on a successful removal (or if the bundle was already gone).
bool DeleteBundleByName(const std::string& bundleName);

// Sweep BUNDLE_ROOT and remove every bundle directory. Intended for
// startup cleanup of any bundles a previous run left behind because of
// a crash or unclean shutdown before the per-bundle delete could fire.
// Returns the number of bundle directories removed.
int DeleteAllBundles();

} // namespace BackupService
