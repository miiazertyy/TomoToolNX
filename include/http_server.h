#pragma once
// http_server.h — TomoToolNX
// Tiny HTTP/1.1 server for the Switch NRO.
// Runs on a background thread, serves the web UI and API endpoints.
//
// THREADING: IMG_Load (SDL_image) is NOT thread-safe on Atmosphere.
// The server thread queues ImportJob; the MAIN thread executes it via
// HasPendingImport / TakePendingImport / FinishImport.

#include <string>
#include <functional>
#include <vector>
#include "texture_processor.h"

namespace HttpServer {

// Start the server on the given port. Spawns a background thread.
// ugcPath: the path to scan for UGC files (e.g. "save:/Ugc")
void Start(int port, const std::string& ugcPath);

// Stop the server and join the thread.
void Stop();

// Returns true if running
bool IsRunning();

// Returns armGetSystemTick() of the last accepted client connection; 0 if none.
uint64_t LastConnectTick();

// Called from main loop to process any pending save commits
// (import writes to save:/ then we need to commit)
bool HasPendingCommit();
void ClearPendingCommit();

// ── Import job (queued by server thread, executed by main thread) ───────────
// The server thread must NOT call IMG_Load — it's not thread-safe on Atmosphere.
// Instead it writes the upload to a temp file and queues an ImportJob.
// The main thread calls HasPendingImport() each frame, picks up the job with
// TakePendingImport(), runs TextureProcessor::ImportPng(), then calls FinishImport().
struct ImportJob {
    TextureProcessor::ImportOptions opts;
    std::string tmpPath; // temp file to delete after import
};

// Returns true when the server thread has queued an import for the main thread.
bool HasPendingImport();

// Take ownership of the pending import job (marks it InProgress).
ImportJob TakePendingImport();

// Called by the main thread after executing the import.
// Pass empty string on success, error message on failure.
void FinishImport(const std::string& result);

// ── Activity log (ring buffer, read by main thread for on-screen display) ──
struct LogEntry {
    std::string text;
    bool        isError;
};

// Drain all pending log entries into `out`. Call from main thread each frame.
void DrainLog(std::vector<LogEntry>& out);

} // namespace HttpServer
