#pragma once
// backup.h

#include "ugc_scanner.h"
#include <string>
#include <cstdint>

#define BACKUP_ROOT  "/switch/TomoToolNX/Backup"
#define BACKUP_SAVE  "/switch/TomoToolNX/Backup/save"
#define BACKUP_IMPORTS "/switch/TomoToolNX/Backup/imports"

namespace BackupService {

// ── Full save backup (runs on background thread) ─────────────────────────────

// Start an async full-save backup from saveMountRoot (e.g. "tomodata:/").
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

// ── Backup folder presence ────────────────────────────────────────────────────

// Returns true if a previous full backup exists at BACKUP_SAVE.
bool HasExistingBackup();

// Delete the entire backup folder (save + imports).
void DeleteBackup();

// ── Per-import UGC backup ─────────────────────────────────────────────────────

// Backs up ugctex/thumb/canvas for a single entry to BACKUP_IMPORTS/<nnn>/.
std::string BackupEntry(const UgcTextureEntry& entry);

} // namespace BackupService
