#pragma once
// backup.h

#include "ugc_scanner.h"
#include <string>
#include <cstdint>

#define BACKUP_ROOT    "/switch/TomoToolNX/Backup"


namespace BackupService {

// ── Full save backup (runs on background thread) ─────────────────────────────

// Start an async full-save backup from saveMountRoot (e.g. "tomodata:/").
// Creates a new timestamped folder (save_YYYYMMDD_HHMMSS).
// Returns immediately. Call BackupProgress() / BackupDone() to poll.
void StartFullBackup(const std::string& saveMountRoot);

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
bool HasExistingBackup();

// Returns the number of existing timestamped save backups (0..MAX_BACKUPS).
int CountBackups();

// Delete the oldest timestamped save backup (by name order).
void DeleteOldestBackup();

// Delete the entire backup root folder (save backups + imports).
void DeleteBackup();

// ── Restore ───────────────────────────────────────────────────────────────────

// List all timestamped save backups, newest first (returns full paths).
std::vector<std::string> ListBackups();

// Synchronously copy backupPath over saveMountRoot.
// Returns empty string on success, error message on failure.
std::string RestoreBackup(const std::string& backupPath, const std::string& saveMountRoot);

} // namespace BackupService
