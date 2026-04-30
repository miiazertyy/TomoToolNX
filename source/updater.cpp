// updater.cpp — GitHub release auto-updater
#include "updater.h"
#include <switch.h>
#include <curl/curl.h>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace Updater {

// ─── State ────────────────────────────────────────────────────────────────────
static Mutex    s_mutex;
static State    s_state    = State::Idle;
static float    s_progress = 0.0f;
static std::string s_latestVersion;
static std::string s_error;
static Thread   s_thread;
static bool     s_threadActive = false;

static void Lock()   { mutexLock(&s_mutex); }
static void Unlock() { mutexUnlock(&s_mutex); }

State    GetState()         { Lock(); auto v=s_state;           Unlock(); return v; }
float    GetProgress()      { Lock(); auto v=s_progress;        Unlock(); return v; }
std::string GetLatestVersion(){ Lock(); auto v=s_latestVersion; Unlock(); return v; }
std::string GetError()      { Lock(); auto v=s_error;           Unlock(); return v; }

// ─── Version comparison ───────────────────────────────────────────────────────
bool VersionGreater(const std::string& a, const std::string& b) {
    // Parse "1.2.3" style
    auto parse = [](const std::string& s, int& ma, int& mi, int& pa) {
        sscanf(s.c_str(), "%d.%d.%d", &ma, &mi, &pa);
    };
    int amaj=0,amin=0,apat=0, bmaj=0,bmin=0,bpat=0;
    parse(a,amaj,amin,apat);
    parse(b,bmaj,bmin,bpat);
    if (amaj!=bmaj) return amaj>bmaj;
    if (amin!=bmin) return amin>bmin;
    return apat>bpat;
}

// ─── CURL helpers ────────────────────────────────────────────────────────────
static size_t WriteString(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = (std::string*)userdata;
    s->append(ptr, size*nmemb);
    return size*nmemb;
}

struct DlCtx { FILE* f; size_t total; size_t done; };
static size_t WriteFile(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = (DlCtx*)userdata;
    size_t written = fwrite(ptr, size, nmemb, ctx->f);
    ctx->done += written * size;
    if (ctx->total > 0) {
        Lock();
        s_progress = (float)ctx->done / ctx->total;
        Unlock();
    }
    return written * size;
}
static int XferInfo(void* userdata, curl_off_t dlTotal, curl_off_t dlNow, curl_off_t, curl_off_t) {
    auto* ctx = (DlCtx*)userdata;
    if (dlTotal > 0) {
        ctx->total = (size_t)dlTotal;
        Lock();
        s_progress = (float)dlNow / dlTotal;
        Unlock();
    }
    return 0;
}

static CURL* MakeCurl(const std::string& url) {
    CURL* c = curl_easy_init();
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "TomoToolNX/" APP_VERSION);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L); // Switch has no cert store
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    return c;
}

// ─── Check thread ─────────────────────────────────────────────────────────────
static void CheckThreadFunc(void*) {
    std::string body;
    CURL* c = MakeCurl("https://api.github.com/repos/" GITHUB_REPO "/releases/latest");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, WriteString);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    CURLcode res = curl_easy_perform(c);
    curl_easy_cleanup(c);

    if (res != CURLE_OK) {
        Lock(); s_state=State::Error; s_error="Network error: "+std::string(curl_easy_strerror(res)); Unlock();
        return;
    }

    // Parse "tag_name" from JSON — simple search, no JSON lib needed
    std::string tag;
    size_t pos = body.find("\"tag_name\"");
    if (pos != std::string::npos) {
        pos = body.find('"', pos+10);
        if (pos != std::string::npos) {
            pos++;
            size_t end = body.find('"', pos);
            tag = body.substr(pos, end-pos);
        }
    }

    if (tag.empty()) {
        Lock(); s_state=State::Error; s_error="Could not parse release info"; Unlock();
        return;
    }

    // Strip leading 'v' for version comparison, but keep original tag for download URL
    std::string latest    = (tag[0]=='v') ? tag.substr(1) : tag;
    std::string latestTag = tag; // preserve original e.g. "v1.1.1"

    Lock();
    s_latestVersion = latestTag; // store full tag so DownloadThreadFunc uses correct URL
    s_state = VersionGreater(latest, APP_VERSION) ? State::UpdateAvailable : State::NoUpdate;
    Unlock();
}

// ─── Download thread ──────────────────────────────────────────────────────────
static void DownloadThreadFunc(void*) {
    Lock(); s_state=State::Downloading; s_progress=0.0f; Unlock();

    std::string latest;
    { Lock(); latest=s_latestVersion; Unlock(); }

    // Asset URL: https://github.com/<repo>/releases/download/<tag>/TomoToolNX.nro
    std::string url = "https://github.com/" GITHUB_REPO "/releases/download/"
                      + latest + "/TomoToolNX.nro";

    // Download to temp file first
    std::string tmpPath = std::string(UPDATE_NRO_PATH) + ".tmp";
    FILE* f = fopen(tmpPath.c_str(), "wb");
    if (!f) {
        Lock(); s_state=State::Error; s_error="Cannot open temp file"; Unlock();
        return;
    }

    DlCtx ctx{f, 0, 0};
    CURL* c = MakeCurl(url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, WriteFile);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, XferInfo);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
    CURLcode res = curl_easy_perform(c);
    curl_easy_cleanup(c);
    fclose(f);

    if (res != CURLE_OK) {
        remove(tmpPath.c_str());
        Lock(); s_state=State::Error; s_error="Download failed: "+std::string(curl_easy_strerror(res)); Unlock();
        return;
    }

    // Replace the NRO
    remove(UPDATE_NRO_PATH);
    if (rename(tmpPath.c_str(), UPDATE_NRO_PATH) != 0) {
        Lock(); s_state=State::Error; s_error="Failed to replace NRO"; Unlock();
        return;
    }

    Lock(); s_state=State::Done; s_progress=1.0f; Unlock();
}

// ─── Public API ───────────────────────────────────────────────────────────────
void StartCheck() {
    mutexInit(&s_mutex);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    Lock(); s_state=State::Checking; Unlock();
    threadCreate(&s_thread, CheckThreadFunc, nullptr, nullptr, 128*1024, 0x2C, -2);
    threadStart(&s_thread);
    s_threadActive = true;
}

void StartDownload() {
    if (s_threadActive) { threadWaitForExit(&s_thread); threadClose(&s_thread); }
    threadCreate(&s_thread, DownloadThreadFunc, nullptr, nullptr, 128*1024, 0x2C, -2);
    threadStart(&s_thread);
    s_threadActive = true;
}

void Cleanup() {
    if (s_threadActive) { threadWaitForExit(&s_thread); threadClose(&s_thread); s_threadActive=false; }
    curl_global_cleanup();
}

} // namespace Updater
