// updater.cpp — Checks GitHub for a newer release; tells the user to install it manually.
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
static std::string s_latestVersion;
static std::string s_error;
static Thread   s_thread;
static bool     s_threadActive = false;

static void Lock()   { mutexLock(&s_mutex); }
static void Unlock() { mutexUnlock(&s_mutex); }

State    GetState()         { Lock(); auto v=s_state;           Unlock(); return v; }
std::string GetLatestVersion(){ Lock(); auto v=s_latestVersion; Unlock(); return v; }
std::string GetError()      { Lock(); auto v=s_error;           Unlock(); return v; }

// ─── Version comparison ───────────────────────────────────────────────────────
bool VersionGreater(const std::string& a, const std::string& b) {
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

    // Strip leading 'v' for version comparison; keep tag for display.
    std::string latest = (tag[0]=='v') ? tag.substr(1) : tag;

    Lock();
    s_latestVersion = latest;
    s_state = VersionGreater(latest, APP_VERSION) ? State::UpdateAvailable : State::NoUpdate;
    Unlock();
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

void Cleanup() {
    // No-op when StartCheck was never called — calling curl_global_cleanup
    // without a matching curl_global_init is undefined behavior per libcurl.
    if (s_threadActive) {
        threadWaitForExit(&s_thread); threadClose(&s_thread); s_threadActive=false;
        curl_global_cleanup();
    }
}

} // namespace Updater
