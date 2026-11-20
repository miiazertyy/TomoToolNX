// updater.cpp — Finds, downloads and installs TomoToolNX releases from GitHub.
//
// Check:    GET /repos/{repo}/releases/latest; read the tag, the notes and the
//           TomoToolNX.nro asset (its URL, size and "sha256:…" digest).
// Download: the asset to "<this nro>.part", hashing as it arrives; then the
//           size, the digest (when GitHub gives one) and the NRO header are
//           checked, and the file becomes "<this nro>.new".
// Install:  when the app closes (after romfsExit, so nothing holds the file):
//           this copy becomes ".old", ".new" takes its name, and the loader is
//           asked to start it if the user chose "Restart now". The ".old" copy
//           is removed on the next start.
#include "updater.h"
#include <switch.h>
#include <curl/curl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace Updater {

// ─── State ────────────────────────────────────────────────────────────────────
static Mutex       s_mutex;
static bool        s_mutexReady = false;
static State       s_state = State::Idle;
static std::string s_latest, s_notes, s_error;
static std::string s_assetUrl, s_assetDigest;
static uint64_t    s_assetSize = 0;
static float       s_progress = 0.f;
static volatile bool s_cancel = false;
static bool        s_restart = false;
static std::string s_self = "sdmc:/switch/TomoToolNX/TomoToolNX.nro";
static Thread      s_thread;
static bool        s_threadActive = false;

static void Lock()   { if (!s_mutexReady) { mutexInit(&s_mutex); s_mutexReady = true; } mutexLock(&s_mutex); }
static void Unlock() { mutexUnlock(&s_mutex); }

State       GetState()         { Lock(); auto v = s_state;       Unlock(); return v; }
std::string GetLatestVersion() { Lock(); auto v = s_latest;      Unlock(); return v; }
std::string GetNotes()         { Lock(); auto v = s_notes;       Unlock(); return v; }
std::string GetError()         { Lock(); auto v = s_error;       Unlock(); return v; }
float       GetProgress()      { Lock(); auto v = s_progress;    Unlock(); return v; }
uint64_t    GetDownloadSize()  { Lock(); auto v = s_assetSize;   Unlock(); return v; }

static void Finish(State st, const std::string& err = "") {
    Lock(); s_state = st; s_error = err; Unlock();
}

// ─── Versions ─────────────────────────────────────────────────────────────────
// The numbers of a version, ignoring a leading "v" and anything after them.
static std::vector<int> VersionParts(const std::string& v) {
    std::vector<int> out;
    size_t i = 0;
    while (i < v.size() && (v[i] == 'v' || v[i] == 'V' || v[i] == ' ')) i++;
    while (i < v.size() && out.size() < 4) {
        if (v[i] < '0' || v[i] > '9') break;
        int n = 0;
        while (i < v.size() && v[i] >= '0' && v[i] <= '9') { n = n * 10 + (v[i] - '0'); i++; }
        out.push_back(n);
        if (i < v.size() && v[i] == '.') i++; else break;
    }
    return out;
}

bool VersionGreater(const std::string& a, const std::string& b) {
    std::vector<int> pa = VersionParts(a), pb = VersionParts(b);
    while (pa.size() < pb.size()) pa.push_back(0);
    while (pb.size() < pa.size()) pb.push_back(0);
    return pa > pb;
}

bool IsDevBuild() {
    const std::string v = APP_VERSION;
    return v.find("dev") != std::string::npos || VersionParts(v).empty() || VersionParts(v)[0] == 0;
}

bool CanInstall() {
    Lock();
    const bool ok = !s_assetUrl.empty() && s_assetSize > 0;
    Unlock();
    return ok && !IsDevBuild();
}

// ─── A small JSON reader (enough for GitHub's release object) ────────────────
struct JVal {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    std::string s;
    double n = 0;
    std::vector<JVal> a;
    std::vector<std::pair<std::string, JVal>> o;
    const JVal* Get(const char* key) const {
        for (const auto& kv : o) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string Text(const char* key) const { const JVal* v = Get(key); return v && v->type == Str ? v->s : std::string(); }
};

struct JReader {
    const char* p;
    const char* e;
    int depth = 0;
    void Ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }
    static void PutUtf8(std::string& out, uint32_t c) {
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
        else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3F)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
    }
    bool Hex4(uint32_t& v) {
        if (e - p < 4) return false;
        v = 0;
        for (int i = 0; i < 4; i++) {
            const char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool String(std::string& out) {
        if (p >= e || *p != '"') return false;
        p++;
        while (p < e && *p != '"') {
            if (*p != '\\') { out += *p++; continue; }
            if (++p >= e) return false;
            const char c = *p++;
            switch (c) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': break;
                case 'b': case 'f': break;
                case 'u': {
                    uint32_t cp;
                    if (!Hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                        p += 2;
                        uint32_t lo;
                        if (!Hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    PutUtf8(out, cp);
                    break;
                }
                default: out += c; break;     // \" \\ \/
            }
        }
        if (p >= e) return false;
        p++;
        return true;
    }
    bool Value(JVal& v) {
        if (++depth > 32) return false;
        Ws();
        if (p >= e) return false;
        bool ok = true;
        if (*p == '{') {
            v.type = JVal::Obj; p++; Ws();
            if (p < e && *p == '}') { p++; }
            else for (;;) {
                Ws();
                std::string k;
                if (!String(k)) { ok = false; break; }
                Ws();
                if (p >= e || *p != ':') { ok = false; break; }
                p++;
                JVal child;
                if (!Value(child)) { ok = false; break; }
                v.o.emplace_back(std::move(k), std::move(child));
                Ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == '}') { p++; break; }
                ok = false; break;
            }
        } else if (*p == '[') {
            v.type = JVal::Arr; p++; Ws();
            if (p < e && *p == ']') { p++; }
            else for (;;) {
                JVal child;
                if (!Value(child)) { ok = false; break; }
                v.a.push_back(std::move(child));
                Ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == ']') { p++; break; }
                ok = false; break;
            }
        } else if (*p == '"') {
            v.type = JVal::Str;
            ok = String(v.s);
        } else if (*p == 't' && e - p >= 4 && !strncmp(p, "true", 4)) { v.type = JVal::Bool; p += 4; }
        else if (*p == 'f' && e - p >= 5 && !strncmp(p, "false", 5)) { v.type = JVal::Bool; p += 5; }
        else if (*p == 'n' && e - p >= 4 && !strncmp(p, "null", 4)) { v.type = JVal::Null; p += 4; }
        else {
            char* end = nullptr;
            v.type = JVal::Num;
            v.n = strtod(p, &end);
            if (!end || end == p) ok = false; else p = end;
        }
        depth--;
        return ok;
    }
};

// ─── CURL helpers ────────────────────────────────────────────────────────────
static size_t WriteString(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = (std::string*)userdata;
    if (s->size() > 512 * 1024) return 0;          // a release object is a few KB
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

static CURL* MakeCurl(const std::string& url, long timeoutSec) {
    CURL* c = curl_easy_init();
    if (!c) return nullptr;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "TomoToolNX/" APP_VERSION);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);   // no certificate store on the Switch; the download is checked by hash
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 4L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeoutSec);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    return c;
}

// ─── Check ────────────────────────────────────────────────────────────────────
static void CheckThreadFunc(void*) {
    std::string body;
    CURL* c = MakeCurl("https://api.github.com/repos/" GITHUB_REPO "/releases/latest", 8L);
    if (!c) { Finish(State::Error, "curl_easy_init failed"); return; }
    struct curl_slist* hdr = curl_slist_append(nullptr, "Accept: application/vnd.github+json");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, WriteString);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    const CURLcode res = curl_easy_perform(c);
    long http = 0;
    if (res == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(c);
    curl_slist_free_all(hdr);
    if (res != CURLE_OK) { Finish(State::Error, "Network error: " + std::string(curl_easy_strerror(res))); return; }
    // 403 / 429: the hourly limit for this address is used up. Not a release.
    if (http != 200) { Finish(State::Error, "GitHub answered HTTP " + std::to_string(http)); return; }

    JVal root;
    JReader r{ body.data(), body.data() + body.size() };
    if (!r.Value(root) || root.type != JVal::Obj) { Finish(State::Error, "Could not read the release"); return; }
    const std::string tag = root.Text("tag_name");
    std::string latest = (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) ? tag.substr(1) : tag;
    if (latest.empty() || latest.size() > 24 || VersionParts(latest).size() < 2) {
        Finish(State::Error, "Could not read the release");
        return;
    }
    std::string url, digest;
    uint64_t size = 0;
    if (const JVal* assets = root.Get("assets")) {
        for (const JVal& a : assets->a) {
            if (a.Text("name") != "TomoToolNX.nro") continue;
            url = a.Text("browser_download_url");
            digest = a.Text("digest");
            if (const JVal* s = a.Get("size")) size = s->type == JVal::Num && s->n > 0 ? (uint64_t)s->n : 0;
            break;
        }
    }
    std::string notes = root.Text("body");
    if (notes.size() > 12000) notes.resize(12000);

    Lock();
    s_latest = latest;
    s_notes = notes;
    s_assetUrl = url;
    s_assetSize = size;
    s_assetDigest = digest.rfind("sha256:", 0) == 0 ? digest.substr(7) : std::string();
    s_state = VersionGreater(latest, APP_VERSION) ? State::UpdateAvailable : State::NoUpdate;
    s_error.clear();
    Unlock();
}

static bool StartThread(ThreadFunc fn) {
    if (s_threadActive) { threadWaitForExit(&s_thread); threadClose(&s_thread); s_threadActive = false; }
    if (R_FAILED(threadCreate(&s_thread, fn, nullptr, nullptr, 192 * 1024, 0x2C, -2))) return false;
    if (R_FAILED(threadStart(&s_thread))) { threadClose(&s_thread); return false; }
    s_threadActive = true;
    return true;
}

static bool HaveInternet() {
    NifmInternetConnectionType type; u32 strength = 0;
    NifmInternetConnectionStatus status = NifmInternetConnectionStatus_ConnectingUnknown1;
    return R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &status)) &&
           status == NifmInternetConnectionStatus_Connected;
}

// libcurl's global init/cleanup is done once by main(), not here.
void StartCheck() {
    const State st = GetState();
    if (st == State::Checking || st == State::Downloading || st == State::Ready) return;
    if (!HaveInternet()) { Finish(State::Error, "No internet connection"); return; }
    Finish(State::Checking);
    if (!StartThread(CheckThreadFunc)) Finish(State::Error, "Could not start the update check");
}

// ─── Download ─────────────────────────────────────────────────────────────────
struct Sink { FILE* f; Sha256Context sha; uint64_t got, want; };

static size_t WriteFile(char* ptr, size_t size, size_t nmemb, void* userdata) {
    Sink* s = (Sink*)userdata;
    const size_t n = size * nmemb;
    if (s_cancel) return 0;
    if (s->got + n > 128ull * 1024 * 1024) return 0;   // no NRO is that big
    if (fwrite(ptr, 1, n, s->f) != n) return 0;
    sha256ContextUpdate(&s->sha, ptr, n);
    s->got += n;
    if (s->want) { Lock(); s_progress = (float)((double)s->got / (double)s->want); Unlock(); }
    return n;
}

static std::string Hex(const uint8_t* d, size_t n) {
    static const char* k = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n; i++) { out += k[d[i] >> 4]; out += k[d[i] & 15]; }
    return out;
}

static void DownloadThreadFunc(void*) {
    Lock();
    const std::string url = s_assetUrl, digest = s_assetDigest, self = s_self;
    const uint64_t want = s_assetSize;
    Unlock();
    const std::string part = self + ".part", ready = self + ".new";
    remove(part.c_str());
    Sink sink{ fopen(part.c_str(), "wb"), {}, 0, want };
    if (!sink.f) { Finish(State::Error, "Could not write to the SD card"); return; }
    sha256ContextCreate(&sink.sha);
    auto fail = [&](const std::string& why) { remove(part.c_str()); Finish(State::UpdateAvailable, why); };
    // A connection that stalls part way (WiFi, or a server that keeps the
    // socket open after the last byte) is picked up where it stopped, with a
    // range request, a few times. The size and the hash decide whether the
    // file is complete, not how the connection ended.
    CURLcode res = CURLE_OK;
    long http = 0;
    for (int attempt = 0; attempt < 5 && !s_cancel; attempt++) {
        if (want && sink.got == want) break;
        CURL* c = MakeCurl(url, 0L);                    // no overall limit: 17 MB on slow WiFi takes a while
        if (!c) { fclose(sink.f); fail("curl_easy_init failed"); return; }
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 512L);   // but give up on a 20 s stall
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 20L);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, WriteFile);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
        const uint64_t from = sink.got;
        if (from > 0) curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)from);
        res = curl_easy_perform(c);
        http = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
        curl_easy_cleanup(c);
        if (from > 0 && http == 200 && sink.got > from) {
            // The server ignored the range and sent the file again from the
            // start: what came in is not a continuation. Start over.
            fclose(sink.f);
            sink.f = fopen(part.c_str(), "wb");
            if (!sink.f) { fail("Could not write to the SD card"); return; }
            sink.got = 0;
            sha256ContextCreate(&sink.sha);
            continue;
        }
        if (res == CURLE_OK) break;
        // Not worth retrying: the server said no, or the file could not be written.
        if (http >= 400 || res == CURLE_WRITE_ERROR) break;
    }
    const bool closed = fclose(sink.f) == 0;
    if (s_cancel) { remove(part.c_str()); Finish(State::UpdateAvailable); return; }
    const bool complete = want ? sink.got == want : (res == CURLE_OK && sink.got > 0);
    if (!complete) {
        if (http >= 400) fail("GitHub answered HTTP " + std::to_string(http));
        else if (res != CURLE_OK) fail("Network error: " + std::string(curl_easy_strerror(res)));
        else fail("The download is incomplete");
        return;
    }
    if (!closed) { fail("Could not write to the SD card"); return; }
    uint8_t hash[SHA256_HASH_SIZE];
    sha256ContextGetHash(&sink.sha, hash);
    if (!digest.empty() && Hex(hash, sizeof hash) != digest) { fail("The download does not match the release"); return; }
    // An NRO: "NRO0" right after its 16-byte start.
    {
        FILE* f = fopen(part.c_str(), "rb");
        char magic[4] = {};
        const bool isNro = f && fseek(f, 0x10, SEEK_SET) == 0 && fread(magic, 1, 4, f) == 4 && !memcmp(magic, "NRO0", 4);
        if (f) fclose(f);
        if (!isNro) { fail("The download is not TomoToolNX"); return; }
    }
    remove(ready.c_str());
    if (rename(part.c_str(), ready.c_str()) != 0) { fail("Could not write to the SD card"); return; }
    Lock(); s_progress = 1.f; Unlock();
    Finish(State::Ready);
}

void StartDownload() {
    const State st = GetState();
    if (st == State::Downloading || st == State::Ready || !CanInstall()) return;
    if (!HaveInternet()) { Finish(State::UpdateAvailable, "No internet connection"); return; }
    s_cancel = false;
    Lock(); s_progress = 0.f; s_state = State::Downloading; s_error.clear(); Unlock();
    if (!StartThread(DownloadThreadFunc)) Finish(State::UpdateAvailable, "Could not start the download");
}

void CancelDownload() { s_cancel = true; }

// ─── Install ──────────────────────────────────────────────────────────────────
void SetSelfPath(const char* argv0) {
    // The homebrew loader passes "sdmc:/switch/…/TomoToolNX.nro"; anything
    // else (an emulator's own path) keeps the default location.
    if (argv0 && !strncmp(argv0, "sdmc:/", 6) && strlen(argv0) > 10) s_self = argv0;
}

void CleanLeftovers() {
    remove((s_self + ".part").c_str());
    remove((s_self + ".new").c_str());
    remove((s_self + ".old").c_str());
}

void RequestRestart() { s_restart = true; }
bool RestartRequested() { return s_restart; }

void FinishInstall() {
    if (GetState() != State::Ready) return;
    const std::string cur = s_self, nw = s_self + ".new", old = s_self + ".old";
    struct stat st {};
    if (stat(nw.c_str(), &st) != 0) return;
    remove(old.c_str());
    if (rename(cur.c_str(), old.c_str()) != 0) return;            // this copy stays as it is
    if (rename(nw.c_str(), cur.c_str()) != 0) { rename(old.c_str(), cur.c_str()); return; }
    if (s_restart && envHasNextLoad()) envSetNextLoad(cur.c_str(), cur.c_str());
}

void Cleanup() {
    s_cancel = true;
    if (s_threadActive) { threadWaitForExit(&s_thread); threadClose(&s_thread); s_threadActive = false; }
}

} // namespace Updater
