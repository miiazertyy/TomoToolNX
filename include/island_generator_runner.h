#pragma once
// island_generator_runner.h
//
// On-Switch orchestrator for the Island Generator pipeline:
// map -> wishes -> levels -> houses -> relationships.
// (A TomoShare bulk-fetch stage existed previously but was removed —
// the public /mii/download endpoint is rate-limited to 4 req/IP/60s,
// which made a 70-Mii fill take 20+ minutes. The runner now leaves the
// bundled 100%-save residents in seed_Mii.bin in place; users can swap
// individual slots via the Mii Stats tab's per-slot downloader.)
// Implemented as a background thread that posts progress snapshots back
// to the main thread through a mutex.
//
// The save must already be mounted at tomodata:/ before Start() is called;
// the runner does NOT mount/unmount on its own. The bundle-apply flow
// arranges this — see Screen::BundlePick in main.cpp.

#include <string>

namespace IslandGenRunner {

struct Options {
    // "random" picks a SurfaceTheme by tick-rolled index; "template"
    // applies a tpl_<id>.bin from data/.
    std::string mapMode = "random";
    std::string mapId;            // template id when mapMode == "template"
    // "dense" calls WriteDenseRelationships; "none" calls WipeRelationships.
    std::string relMode = "dense";
    // Range forwarded to IslandGen::RandomizeMiiLevels. Stored values are
    // 0-based (displayOffset = 1 → stored 0 = displayed level 1). We pick a
    // conservative low-mid range because Tomodachi Life's UI was observed
    // to refuse-load saves with very high stored levels, even though no
    // explicit cap exists in the schema.
    int  levelMin       = 5;
    int  levelMax       = 25;

    // If true, the runner reads /switch/TomoToolNX/Miis/*.ltd (case-
    // insensitive) and overwrites slots 2..70 with shuffled imports of
    // those files. Set by the on-Switch IslandGenConfig "Batch LTD" row.
    // Empty / missing folder still results in a silent skip — the toggle
    // only enables the step; it doesn't require files to be present.
    bool batchLtdMiis   = false;
};

struct Progress {
    int  step    = 0;      // 0-based current sub-step
    int  total   = 1;      // total sub-steps (so a progress bar can divide)
    std::string label;     // user-facing description of what's happening now
    bool done    = false;
    bool cancelled = false;
    std::string error;     // empty unless something fatal happened
};

// Spawn the worker thread. Idempotent: returns false if a run is already in
// flight. Use Cleanup() to free the previous thread before re-Start().
bool Start(const Options& opts);

// Thread-safe snapshot of the current run.
Progress Snapshot();

// Returns true while the worker thread is alive (running or just-finished
// but not yet joined). Useful for the UI to decide when to leave the
// progress screen.
bool IsActive();

// Sets the cancel flag — worker exits at the next safe checkpoint, which
// is between sub-steps and after each Mii fetch. The save is committed in
// whatever partial state it was in; the caller may want to RestoreBackup
// afterwards.
void RequestCancel();

// Join the worker thread. MUST be called before Start() can run again.
// Safe to call when no run is in flight (no-op).
void Cleanup();

} // namespace IslandGenRunner
