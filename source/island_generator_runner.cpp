// island_generator_runner.cpp — see island_generator_runner.h.
//
// One background thread, one mutex-protected Progress record. The worker
// re-loads/saves the three .sav files at each sub-step rather than holding
// them in memory across the whole pipeline; that mirrors what the HTTP
// handlers do and keeps memory usage bounded while still letting the user
// cancel mid-run without leaving a half-mutated SavFile dangling.

#include "island_generator_runner.h"
#include "island_generator.h"
#include "save_editor.h"
#include "save_mount.h"
#include "mii_manager.h"
#include "share_client.h"

#include <switch.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr int kMaxMiiSlots = MII_MAX_SLOTS;

// ─── State ─────────────────────────────────────────────────────────────────
Thread  s_thread;
bool    s_threadActive = false;
Mutex   s_mutex;
bool    s_mutexInit    = false;
bool    s_cancelReq    = false;
IslandGenRunner::Progress s_progress;
IslandGenRunner::Options  s_opts;

void EnsureMutex() {
    if (!s_mutexInit) { mutexInit(&s_mutex); s_mutexInit = true; }
}

void SetProgress(int step, int total, const std::string& label) {
    EnsureMutex();
    mutexLock(&s_mutex);
    s_progress.step  = step;
    s_progress.total = total;
    s_progress.label = label;
    mutexUnlock(&s_mutex);
}
void SetDone(const std::string& err = "") {
    EnsureMutex();
    mutexLock(&s_mutex);
    s_progress.done  = true;
    s_progress.error = err;
    mutexUnlock(&s_mutex);
}
void SetCancelled() {
    EnsureMutex();
    mutexLock(&s_mutex);
    s_progress.cancelled = true;
    s_progress.done      = true;
    mutexUnlock(&s_mutex);
}
bool Cancelled() {
    EnsureMutex();
    mutexLock(&s_mutex);
    bool c = s_cancelReq;
    mutexUnlock(&s_mutex);
    return c;
}

// ─── Sub-steps ─────────────────────────────────────────────────────────────
// Each returns "" on success or an error string. None of them throw.

std::string StepMap(const IslandGenRunner::Options& opts) {
    SaveEditor::SavFile mp;
    std::string err;
    if (!SaveEditor::Load("tomodata:/Map.sav", mp, err))
        return "load Map.sav: " + err;

    // "classic" preserves the seed's existing floor (the 100% save's
    // hand-crafted island layout) — only the snapper runs so we still get
    // randomized building rotations + the item-deletion sweep. "random"
    // generates a fresh procedural terrain first, then after the snap a
    // roads-and-decor pass refills the empty slots with paths between
    // buildings and decorations (trees, lamps, hedges, flowers).
    bool isRandom = (opts.mapMode == "random");
    if (isRandom) {
        // Always use the grass surface theme for random islands. The
        // alternative themes (cherry, snowy, cobblestone, fallenleaves…)
        // give the island a single colour wash that overrides whatever
        // the player might want their seasonal vibe to be — grass is the
        // neutral default that reads correctly under every theme.
        const IslandGen::SurfaceTheme* grass = IslandGen::SurfaceThemeById("grass");
        if (!grass) return "grass surface theme missing";
        err = IslandGen::GenerateRandomMap(mp, *grass);
        if (!err.empty()) return err;
    }
    IslandGen::SnapActorsToLand(mp);
    if (isRandom) IslandGen::PaintRoadsAndDecor(mp);

    err = SaveEditor::Save("tomodata:/Map.sav", mp);
    if (!err.empty()) return "save Map.sav: " + err;
    std::string cerr = SaveMount::Commit();
    if (!cerr.empty()) return "commit Map.sav: " + cerr;
    return "";
}

std::string StepWishes() {
    SaveEditor::SavFile pl;
    std::string err;
    if (!SaveEditor::Load("tomodata:/Player.sav", pl, err))
        return "load Player.sav: " + err;
    IslandGen::UnlockAllWishes(pl);
    err = SaveEditor::Save("tomodata:/Player.sav", pl);
    if (!err.empty()) return "save Player.sav: " + err;
    return SaveMount::Commit();
}

std::string StepLevels(const IslandGenRunner::Options& opts) {
    SaveEditor::SavFile mi;
    std::string err;
    if (!SaveEditor::Load("tomodata:/Mii.sav", mi, err))
        return "load Mii.sav: " + err;
    int lo = std::max(0,   opts.levelMin);
    int hi = std::min(999, opts.levelMax);
    if (lo > hi) std::swap(lo, hi);
    IslandGen::RandomizeMiiLevels(mi, lo, hi);
    err = SaveEditor::Save("tomodata:/Mii.sav", mi);
    if (!err.empty()) return "save Mii.sav: " + err;
    return SaveMount::Commit();
}

std::string StepHouses() {
    SaveEditor::SavFile mp, mi;
    std::string err;
    if (!SaveEditor::Load("tomodata:/Map.sav", mp, err)) return "load Map.sav: " + err;
    if (!SaveEditor::Load("tomodata:/Mii.sav", mi, err)) return "load Mii.sav: " + err;
    IslandGen::AssignHousing(mi, mp);
    err = SaveEditor::Save("tomodata:/Mii.sav", mi);
    if (!err.empty()) return "save Mii.sav: " + err;
    return SaveMount::Commit();
}

std::string StepRels(const IslandGenRunner::Options& opts) {
    SaveEditor::SavFile mi;
    std::string err;
    if (!SaveEditor::Load("tomodata:/Mii.sav", mi, err))
        return "load Mii.sav: " + err;
    if (opts.relMode == "none")
        IslandGen::WipeRelationships(mi);
    else
        IslandGen::WriteDenseRelationships(mi);
    err = SaveEditor::Save("tomodata:/Mii.sav", mi);
    if (!err.empty()) return "save Mii.sav: " + err;
    return SaveMount::Commit();
}

std::string StepBelongings() {
    SaveEditor::SavFile mi;
    std::string err;
    if (!SaveEditor::Load("tomodata:/Mii.sav", mi, err))
        return "load Mii.sav: " + err;
    IslandGen::RandomizeMiiBelongings(mi);   // 0-6 random Treasure items per Mii
    err = SaveEditor::Save("tomodata:/Mii.sav", mi);
    if (!err.empty()) return "save Mii.sav: " + err;
    return SaveMount::Commit();
}

std::string StepOutfits() {
    SaveEditor::SavFile mi;
    std::string err;
    if (!SaveEditor::Load("tomodata:/Mii.sav", mi, err))
        return "load Mii.sav: " + err;
    IslandGen::RandomizeMiiOutfits(mi);
    err = SaveEditor::Save("tomodata:/Mii.sav", mi);
    if (!err.empty()) return "save Mii.sav: " + err;
    return SaveMount::Commit();
}

std::string StepInteriors() {
    SaveEditor::SavFile mp, mi;
    std::string err;
    if (!SaveEditor::Load("tomodata:/Map.sav", mp, err))
        return "load Map.sav: " + err;
    if (!SaveEditor::Load("tomodata:/Mii.sav", mi, err))
        return "load Mii.sav: " + err;
    IslandGen::RandomizeMiiInteriors(mp, mi);
    err = SaveEditor::Save("tomodata:/Map.sav", mp);
    if (!err.empty()) return "save Map.sav: " + err;
    return SaveMount::Commit();
}

// User-fillable Mii pool: scan /switch/TomoToolNX/Miis/ for *.ltd files
// and overwrite slots 2..70 with shuffled imports. The folder is the user's
// (MTP / SD) way to seed any pool of community Miis they like without
// hitting the TomoShare API's hard rate limit. Slot 1 is the founder so we
// always keep it. Empty / missing folder → silent skip, seed Miis stay.
std::string StepMiiReplace() {
    static const char* kPoolDir = "/switch/TomoToolNX/Miis";
    DIR* d = opendir(kPoolDir);
    if (!d) return "";  // folder doesn't exist yet — user hasn't dropped any

    std::vector<std::string> files;
    while (struct dirent* de = readdir(d)) {
        const char* n = de->d_name;
        size_t L = strlen(n);
        if (L < 5) continue;
        // Case-insensitive ".ltd" suffix check — MTP transfers from a PC can
        // produce either case.
        char e2 = (char)tolower((unsigned char)n[L-3]);
        char e3 = (char)tolower((unsigned char)n[L-2]);
        char e4 = (char)tolower((unsigned char)n[L-1]);
        if (!(n[L-4] == '.' && e2 == 'l' && e3 == 't' && e4 == 'd')) continue;
        files.emplace_back(std::string(kPoolDir) + "/" + n);
    }
    closedir(d);
    if (files.empty()) return "";

    // Fisher-Yates shuffle so the user's drop-order doesn't get baked into
    // every island. Seeded off the system tick for cross-run variety.
    uint32_t rng = (uint32_t)armGetSystemTick();
    if (rng == 0) rng = 1;
    auto xshift = [&]() -> uint32_t {
        uint32_t x = rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; rng = x; return x;
    };
    for (size_t k = files.size() - 1; k > 0; k--) {
        size_t j = (size_t)(xshift() % (uint32_t)(k + 1));
        std::swap(files[k], files[j]);
    }

    // Replace every slot 1..70. We used to skip slot 1 as "the founder"
    // but the bundled seed doesn't actually carry a special player Mii
    // there — it's just another "Boy 1" name — so leaving it untouched
    // gave the user one stuck Mii every batch import. All 70 slots get
    // overwritten now; user can always re-import to swap them.
    constexpr int kFirstSlot = 1;
    int n = std::min((int)files.size(), kMaxMiiSlots);
    for (int i = 0; i < n; i++) {
        // ImportMii returns an error string for malformed .ltd files. We
        // swallow per-file errors so one bad upload doesn't abort the
        // whole pass — the seed Mii in that slot just sticks around.
        (void)MiiManager::ImportMii(kFirstSlot + i, files[(size_t)i]);
    }

    // Folder is meant to be ephemeral: delete every .ltd we listed (even
    // the unused tail beyond slot 70) and remove the folder itself so the
    // next generation run starts from a clean slate. If rmdir fails for
    // any reason (stray file we don't recognise, etc.) we let it slide.
    for (const auto& p : files) ::remove(p.c_str());
    rmdir(kPoolDir);
    return "";
}

// TomoShare bulk-fetch (StepMiiFill / DownloadOneMii / ExtractIds /
// SleepCancellable / IdLooksSafe) was removed: the /mii/download endpoint
// is hard-rate-limited to 4 reqs / IP / 60s, which made a single 70-Mii
// generation take 20+ minutes of waiting. The runner now leaves the
// bundled 70 residents from seed_Mii.bin in place; users who want to
// swap individuals out can use the per-slot TomoShare downloader in the
// Mii Stats tab. See the conversation context for the design rationale.

#if 0
// ── TomoShare bulk fetch ──────────────────────────────────────────────────
// Pull the numeric `id` of each entry in the response's `"miis":[ ... ]`
// array. The response looks like:
//
//   { "miis": [
//       { "id": 12345, "name": "...", "user": { "id": 678, "name": "..." } },
//       ...
//     ],
//     "totalCount": ...,
//     "lastPage": ...
//   }
//
// Each mii object has a nested `user` object that also carries its own
// `id`. An earlier substring-only scan for `"id":` was picking those up
// too — most of the "ids" we collected were user ids, and downloading
// /mii/<userid>/download 404'd. That's why the slot counter advanced
// while only ~1 of 69 ever succeeded. We now traverse the JSON with
// brace-depth awareness so only the *top-level* id inside each `{...}`
// entry of the miis array gets captured.
void ExtractIds(const std::vector<uint8_t>& body, std::vector<std::string>& out) {
    if (body.size() < 8) return;
    const char* s = (const char*)body.data();
    size_t n = body.size();

    // Find the literal `"miis"` key. The array is always at the top level,
    // so the first occurrence is the right one. We then advance to its
    // opening `[`.
    static const char keyTok[] = "\"miis\"";
    size_t kLen = sizeof(keyTok) - 1;
    size_t miiKey = std::string::npos;
    for (size_t i = 0; i + kLen <= n; i++) {
        if (memcmp(s + i, keyTok, kLen) == 0) { miiKey = i; break; }
    }
    if (miiKey == std::string::npos) return;
    size_t p = miiKey + kLen;
    while (p < n && s[p] != '[') {
        if (s[p] == ',' || s[p] == '}') return; // not "miis": [ ... ]
        p++;
    }
    if (p >= n) return;
    p++; // step past '['

    // Walk the array entry-by-entry, brace-balanced.
    while (p < n) {
        while (p < n && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' ||
                         s[p] == '\r' || s[p] == ',')) p++;
        if (p >= n || s[p] == ']') break;
        if (s[p] != '{') { p++; continue; }
        size_t objStart = p;
        int depth = 0;
        bool inStr = false, esc = false;
        // Find matching '}'.
        while (p < n) {
            char c = s[p++];
            if (inStr) {
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') inStr = true;
            else if (c == '{') depth++;
            else if (c == '}') { depth--; if (depth == 0) break; }
        }
        size_t objEnd = p; // exclusive

        // Inside this single mii object, find the FIRST `"id":<num>` that
        // lives at depth 0 of the object body. depth tracks how deep we've
        // descended into nested braces (i.e. `user: { ... }`).
        int subDepth = 0;
        bool inSubStr = false, subEsc = false;
        static const char idTok[] = "\"id\":";
        size_t idTokLen = sizeof(idTok) - 1;
        for (size_t q = objStart + 1; q + idTokLen <= objEnd; ) {
            char cc = s[q];
            if (inSubStr) {
                if (subEsc) subEsc = false;
                else if (cc == '\\') subEsc = true;
                else if (cc == '"') inSubStr = false;
                q++;
                continue;
            }
            // The `"id":` token itself starts with `"`, so we must look for
            // it BEFORE the generic `"` -> inSubStr branch below — otherwise
            // every opening quote of `"id"` gets eaten as a string-start
            // and the memcmp below is never reached.
            if (subDepth == 0 && memcmp(s + q, idTok, idTokLen) == 0) {
                size_t r = q + idTokLen;
                while (r < objEnd && (s[r] == ' ' || s[r] == '\t')) r++;
                if (r < objEnd && s[r] >= '0' && s[r] <= '9') {
                    size_t start = r;
                    while (r < objEnd && s[r] >= '0' && s[r] <= '9') r++;
                    out.emplace_back(s + start, r - start);
                } else if (r < objEnd && s[r] == '"') {
                    // Defensive — API has never returned string ids in the
                    // wild, but if a future version does we don't want to
                    // silently drop the entry.
                    size_t start = ++r;
                    while (r < objEnd && s[r] != '"') r++;
                    if (r > start) out.emplace_back(s + start, r - start);
                }
                break; // only one id per entry
            }
            if (cc == '"') { inSubStr = true; q++; continue; }
            if (cc == '{') { subDepth++; q++; continue; }
            if (cc == '}') { subDepth--; q++; continue; }
            q++;
        }
    }
}

bool IdLooksSafe(const std::string& id) {
    if (id.empty() || id.size() > 16) return false;
    for (char c : id) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
            return false;
    }
    return true;
}

// Download one .ltd and import it into `slot`. Returns:
//    1  → imported a new Mii
//    0  → transient failure (network glitch, empty response, etc.)
//   -1  → HTTP 429 rate limit — caller should wait for the next 60-second
//        window before trying again. Surfaced as a distinct return code
//        because /mii/download is hard-capped at 4 requests / IP / 60s by
//        the TomoShare backend (see rate-limit.ts and
//        backend/src/app/mii/[id]/download/route.ts — `new RateLimit(req,
//        4, "/mii/download")` with WINDOW_SIZE = 60). Earlier we treated
//        429 as a normal failure and silently skipped, which produced the
//        "counter goes up but only (1 ok)" symptom: the first four worked,
//        then the rest 429'd in silence.
int DownloadOneMii(int slot, const std::string& id) {
    std::string dlUrl = "https://api.tomodachishare.com/mii/" + id + "/download";
    std::vector<uint8_t> dl;
    std::string err;
    long status = ShareClient::Fetch(dlUrl, dl, err);
    if (status == 429) return -1;
    if (status != 200 || dl.empty()) return 0;

    mkdir("/switch/TomoToolNX", 0777);
    std::string tmpPath = "/switch/TomoToolNX/.share_import_tmp_runner.ltd";
    FILE* f = fopen(tmpPath.c_str(), "wb");
    if (!f) return 0;
    fwrite(dl.data(), 1, dl.size(), f);
    fclose(f);

    std::string importErr = MiiManager::ImportMii(slot, tmpPath);
    remove(tmpPath.c_str());
    if (!importErr.empty()) return 0;
    return 1;
}

// Sleep `seconds` while honouring the cancel flag. We poll every 250ms so
// the user can cancel mid-wait without waiting out the full back-off.
// Returns true if cancelled mid-wait.
bool SleepCancellable(int seconds, int baseStep, int totalSteps,
                      const char* labelPrefix) {
    for (int s = seconds; s > 0; s--) {
        if (Cancelled()) return true;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s (%ds)", labelPrefix, s);
        SetProgress(baseStep, totalSteps, buf);
        for (int k = 0; k < 4; k++) {
            if (Cancelled()) return true;
            svcSleepThread(250ULL * 1000ULL * 1000ULL); // 250 ms
        }
    }
    return false;
}

std::string StepMiiFill(const IslandGenRunner::Options& opts,
                        int baseStep, int totalSteps) {
    int count = std::min(opts.miiCount, kMaxMiiSlots);
    if (count < 2) return "";

    // Pre-flight network check. With no WiFi every ShareClient::Fetch call
    // failed at the transport layer and the outer loop swallowed all 69
    // failures and ended with "Done", which made it look like the
    // generator had silently filled the island with placeholders. Surface
    // this as a real error instead so the user knows why.
    {
        u32 ip = 0;
        Result rc = nifmGetCurrentIpAddress(&ip);
        if (R_FAILED(rc) || ip == 0) {
            return "no internet — connect to WiFi and re-run, "
                   "or pick \"Skip\" for fetch on the previous screen";
        }
    }

    int targetSlots = count - 1; // slot 1 is the founder, untouched

    // ── 1. Build an ID pool ──────────────────────────────────────────────
    // /api/mii/list is NOT rate-limited (only /mii/download is). One call
    // with limit=100 gets us up to 100 candidate IDs per request. We hit
    // a few pages so dedup against `used` actually has room to work on
    // larger islands. limit=100 is the schema's hard max — see
    // searchSchema in tomodachi-share-main/shared/src/schemas.ts.
    SetProgress(baseStep, totalSteps, "Querying TomoShare for a Mii pool…");
    static const char* kSorts[] = { "newest", "likes", "oldest" };
    std::vector<std::string> pool;
    pool.reserve((size_t)targetSlots * 2);
    // Track the last call's outcome so we can surface a specific reason
    // when no pool gets built instead of the generic "service may be down".
    long lastStatus = -1;
    size_t lastBodyLen = 0;
    std::string lastCurlErr;
    int idsFoundTotal = 0;
    std::vector<uint8_t> lastBody;
    for (int p = 1; p <= 3 && (int)pool.size() < targetSlots * 2; p++) {
        if (Cancelled()) return "";
        const char* sortKey =
            kSorts[((uint32_t)armGetSystemTick() ^ (uint32_t)p) % 3];
        std::string url = "https://api.tomodachishare.com/api/mii/list?limit=100&page="
                        + std::to_string(p)
                        + "&sort=" + sortKey;
        std::vector<uint8_t> body;
        std::string err;
        long status = ShareClient::Fetch(url, body, err);
        lastStatus   = status;
        lastBodyLen  = body.size();
        lastCurlErr  = err;
        if (!body.empty()) lastBody = body;
        if (status != 200 || body.empty()) continue;
        std::vector<std::string> ids;
        ExtractIds(body, ids);
        idsFoundTotal += (int)ids.size();
        for (auto& id : ids) {
            if (IdLooksSafe(id)) pool.emplace_back(std::move(id));
        }
    }
    if (pool.empty()) {
        char buf[256];
        if (lastStatus <= 0) {
            std::snprintf(buf, sizeof(buf),
                          "list call failed: %s (no HTTP status reached). "
                          "Check WiFi DNS / proxy.",
                          lastCurlErr.empty() ? "transport error" : lastCurlErr.c_str());
        } else if (lastStatus != 200) {
            std::snprintf(buf, sizeof(buf),
                          "list HTTP %ld (body %zu bytes). "
                          "API may have changed or is rejecting our query.",
                          lastStatus, lastBodyLen);
        } else if (idsFoundTotal == 0) {
            // Dump the body so we can inspect what actually came back.
            // Most likely culprits: Cloudflare HTML challenge served with
            // 200, gzip-compressed body we didn't decode, or a new JSON
            // envelope shape.
            mkdir("/switch/TomoToolNX", 0777);
            const char* dumpPath = "/switch/TomoToolNX/.last_list_response.bin";
            if (!lastBody.empty()) {
                FILE* fp = fopen(dumpPath, "wb");
                if (fp) {
                    fwrite(lastBody.data(), 1, lastBody.size(), fp);
                    fclose(fp);
                }
            }
            char preview[33] = {0};
            size_t pn = std::min<size_t>(lastBody.size(), 32);
            for (size_t i = 0; i < pn; i++) {
                unsigned char c = lastBody[i];
                preview[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
            }
            std::snprintf(buf, sizeof(buf),
                          "HTTP 200, %zu bytes, 0 ids. First bytes: \"%s\". "
                          "Dumped to %s",
                          lastBodyLen, preview, dumpPath);
        } else {
            std::snprintf(buf, sizeof(buf),
                          "list returned %d ids but all failed IdLooksSafe.",
                          idsFoundTotal);
        }
        return std::string(buf);
    }
    // Local shuffle so we don't always start from the same end of the pool.
    for (size_t k = pool.size() - 1; k > 0; k--) {
        size_t j = (size_t)(armGetSystemTick() % (k + 1));
        std::swap(pool[k], pool[j]);
    }

    // ── 2. Drip-download honouring the 4-per-60s rate limit ──────────────
    // The TomoShare /mii/download endpoint allows 4 requests per fixed
    // 60-second window per IP. We track our own request count over the
    // most recent window-start and sleep until the next window when we've
    // used the budget — or back off if the server beats us to telling us
    // we're rate-limited (429).
    std::vector<std::string> used;
    used.reserve((size_t)targetSlots);
    int successCount = 0;
    size_t poolPos   = 0;

    // Window state. windowStart = unix seconds when the current 60s window
    // began for OUR clock; reqsInWindow = how many downloads we kicked off
    // since then. The server uses its own clock so we always allow a tiny
    // safety margin (we cap at 3 reqs/window instead of 4).
    const int kReqsPerWindow = 3;
    const int kWindowSec     = 60;
    time_t windowStart = std::time(nullptr);
    int    reqsInWindow = 0;

    auto waitForNextWindow = [&]() -> bool {
        time_t now = std::time(nullptr);
        int elapsed = (int)(now - windowStart);
        int remaining = kWindowSec - elapsed;
        if (remaining < 1) remaining = 1;
        char prefix[96];
        std::snprintf(prefix, sizeof(prefix),
                      "Waiting for TomoShare rate-limit window (%d Miis filled)",
                      successCount);
        if (SleepCancellable(remaining, baseStep, totalSteps, prefix)) return true;
        windowStart  = std::time(nullptr);
        reqsInWindow = 0;
        return false;
    };

    for (int slot = 2; slot <= count; slot++) {
        if (Cancelled()) break;

        // Find the next unused ID in the pool. If we run out, refresh
        // with another list page.
        std::string pick;
        while (poolPos < pool.size()) {
            std::string& cand = pool[poolPos++];
            bool dup = false;
            for (const auto& u : used) { if (u == cand) { dup = true; break; } }
            if (!dup) { pick = cand; break; }
        }
        if (pick.empty()) break; // ran out of unique candidates

        // Rate-limit budget management. If we've already issued our quota
        // for this window, sleep until the next one.
        if (reqsInWindow >= kReqsPerWindow) {
            if (waitForNextWindow()) break;
        }

        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "Fetching Mii %d / %d… (%d ok)",
                      slot, count, successCount);
        SetProgress(baseStep, totalSteps, buf);

        reqsInWindow++;
        int rc = DownloadOneMii(slot, pick);
        if (rc == 1) {
            used.push_back(pick);
            successCount++;
            SaveMount::Commit();
        } else if (rc == -1) {
            // Server-side rate-limit ahead of our local counter — wait
            // out the rest of this window and retry the SAME slot.
            if (waitForNextWindow()) break;
            slot--; // retry this slot in the next window
            continue;
        }
        // rc == 0: transient failure (timeout, 4xx, parse, etc.). Move on
        // to the next slot — there's no point burning the rate-limit
        // budget retrying a broken mii id.
    }

    if (successCount == 0 && targetSlots > 0) {
        return "TomoShare returned no Miis — service may be down "
               "or your network can't reach api.tomodachishare.com";
    }
    if (successCount < targetSlots) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "Fetched %d / %d Miis (%d slots kept placeholders)",
                      successCount, targetSlots, targetSlots - successCount);
        SetProgress(baseStep, totalSteps, buf);
    }
    return "";
}
#endif

// ── Worker ────────────────────────────────────────────────────────────────
void WorkerFunc(void*) {
    IslandGenRunner::Options opts = s_opts;

    // Pipeline: map regen, optional Mii replacement from the user-fillable
    // /switch/TomoToolNX/Miis/ folder (gated by opts.batchLtdMiis from the
    // config screen), dense relationships (Knows/Friend only — see
    // memory:project-relationship-uniqueness for why), random outfits
    // (one of 347 official coordinates per Mii, gender-aware), random
    // per-Mii belongings (0-6 official Treasure items), random room
    // interiors. Wishes / Levels / Houses remain off; the seed already
    // has sensible defaults for those.
    int total = 5 + (opts.batchLtdMiis ? 1 : 0);
    int step  = 0;

    auto fail = [&](const std::string& msg) {
        SetDone(msg);
    };
    auto checkCancel = [&]() -> bool {
        if (Cancelled()) { SetCancelled(); return true; }
        return false;
    };

    SetProgress(step, total, "Generating map…");
    std::string e = StepMap(opts);
    if (!e.empty()) return fail("map: " + e);
    if (checkCancel()) return;
    step++;

    if (opts.batchLtdMiis) {
        SetProgress(step, total, "Replacing Miis from /Miis/…");
        e = StepMiiReplace();
        if (!e.empty()) return fail("mii replace: " + e);
        if (checkCancel()) return;
        step++;
    }

    SetProgress(step, total, "Writing relationships…");
    e = StepRels(opts);
    if (!e.empty()) return fail("relationships: " + e);
    if (checkCancel()) return;
    step++;

    SetProgress(step, total, "Dressing miis…");
    e = StepOutfits();
    if (!e.empty()) return fail("outfits: " + e);
    if (checkCancel()) return;
    step++;

    SetProgress(step, total, "Filling Mii pockets…");
    e = StepBelongings();
    if (!e.empty()) return fail("belongings: " + e);
    if (checkCancel()) return;
    step++;

    SetProgress(step, total, "Decorating houses…");
    e = StepInteriors();
    if (!e.empty()) return fail("interiors: " + e);
    if (checkCancel()) return;
    step++;

    SetProgress(step, total, "Done");
    SetDone();
}

} // namespace

namespace IslandGenRunner {

bool Start(const Options& opts) {
    EnsureMutex();
    if (s_threadActive) return false;
    {
        mutexLock(&s_mutex);
        s_progress  = Progress{};
        s_cancelReq = false;
        s_opts      = opts;
        mutexUnlock(&s_mutex);
    }
    Result rc = threadCreate(&s_thread, WorkerFunc, nullptr, nullptr,
                             256 * 1024, 0x2C, -2);
    if (R_FAILED(rc)) return false;
    rc = threadStart(&s_thread);
    if (R_FAILED(rc)) { threadClose(&s_thread); return false; }
    s_threadActive = true;
    return true;
}

Progress Snapshot() {
    EnsureMutex();
    mutexLock(&s_mutex);
    Progress p = s_progress;
    mutexUnlock(&s_mutex);
    return p;
}

bool IsActive() {
    return s_threadActive;
}

void RequestCancel() {
    EnsureMutex();
    mutexLock(&s_mutex);
    s_cancelReq = true;
    mutexUnlock(&s_mutex);
}

void Cleanup() {
    if (!s_threadActive) return;
    threadWaitForExit(&s_thread);
    threadClose(&s_thread);
    s_threadActive = false;
}

} // namespace IslandGenRunner
