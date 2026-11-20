// music.cpp — TomoToolNX. See music.h.
//
// The pieces ship with the app (romfs/music, tools/assets/build_music.py):
// the game's BWAV music decoded and encoded as Opus, about a fifth of the
// size, with each piece's length, loop points and loudness in index.txt.
// Each voice has its own Opus decoder (libopusfile, reading the piece from
// memory); seeking is exact to the sample, which is how a second take starts
// in step with the first, how a loop goes back, and how a piece picks up
// where it stopped.

#include "music.h"
#include "ui_sound.h"

#include <opusfile.h>
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <vector>

namespace Music {

const Piece kPieces[] = {
    { "title",             "BGM_Field03_Title_Basic",    nullptr },
    { "island_day",        "BGM_Field03_Basic",          nullptr },
    { "island_night",      "BGM_Field06_Basic",          nullptr },
    { "island_edit",       "BGM_Field03_IslandEdit",     "BGM_Field06_IslandEdit" },
    { "mii_editor",        "BGM_MiiEditor_01a",          "BGM_MiiEditor_01b" },
    { "texture_editor",    "BGM_UGCEditor_02a",          "BGM_UGCEditor_02b" },
    { "photo_studio",      "BGM_PhotoStudio",            nullptr },
    { "encyclopedia",      "BGM_Zukan_01",               nullptr },
    { "local_play",        "BGM_Local_Communication_01", nullptr },
    { "fountain",          "BGM_Spot_Fountain_01",       nullptr },
    { "ferris_wheel",      "BGM_Spot_FerrisWheel_01",    nullptr },
    { "news",              "BGM_News_Topic_Happy_01",    nullptr },
    { "shop_food",         "BGM_Shop_Food",              nullptr },
    { "shop_clothes",      "BGM_Shop_Cloth",             nullptr },
    { "shop_goods",        "BGM_Shop_Item",              nullptr },
    { "shop_interior",     "BGM_Shop_Interior_01",       nullptr },
    { "shop_building",     "BGM_Shop_Building",          nullptr },
    { "pawn_shop",         "BGM_Shop_Pawn",              nullptr },
};
const int kPieceCount = (int)(sizeof(kPieces) / sizeof(kPieces[0]));

const char* PieceOf(const std::string& file) {
    for (const Piece& p : kPieces)
        if (file == p.file || (p.alt && file == p.alt)) return p.code;
    return "";
}

namespace {

constexpr int kRate = 48000;
constexpr int kVoices = 4;

// Takes of one piece that run in step: the same length and loop, written to
// be swapped in the middle (the game's "sync groups").
const char* const kGroups[][4] = {
    { "BGM_Field03_Title_Basic", "BGM_Field03_Basic", "BGM_Field03_IslandEdit", nullptr },
    { "BGM_Field06_Basic", "BGM_Field06_IslandEdit", nullptr, nullptr },
    { "BGM_MiiEditor_01a", "BGM_MiiEditor_01b", nullptr, nullptr },
    { "BGM_UGCEditor_02a", "BGM_UGCEditor_02b", nullptr, nullptr },
};
int GroupOf(const std::string& f) {
    for (int g = 0; g < (int)(sizeof(kGroups) / sizeof(kGroups[0])); g++)
        for (const char* n : kGroups[g])
            if (n && f == n) return g;
    return -1;
}

struct Track {
    std::string name;
    std::vector<uint8_t> raw;                // the Opus file
    uint32_t samples = 0, loopStart = 0, loopEnd = 0;
    bool loops = false;
    float norm = 1.f;                        // gain to the usual loudness, for 16-bit samples
    int group = -1;
    int users = 0;                           // voices on it
    double lastUse = 0;
};

// romfs/music/index.txt: name samples loops loopStart loopEnd gain.
struct Info { uint32_t samples = 0, loopStart = 0, loopEnd = 0; bool loops = false; float gain = 1.f; };
std::map<std::string, Info> sIndex;
bool sIndexRead = false;
void ReadIndex() {
    if (sIndexRead) return;
    sIndexRead = true;
    FILE* f = fopen("romfs:/music/index.txt", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char name[96];
        unsigned long n = 0, ls = 0, le = 0;
        int loops = 0;
        float gain = 1.f;
        if (sscanf(line, "%95s %lu %d %lu %lu %f", name, &n, &loops, &ls, &le, &gain) != 6) continue;
        Info inf;
        inf.samples = (uint32_t)n;
        inf.loops = loops != 0 && ls < le;
        inf.loopStart = (uint32_t)ls;
        inf.loopEnd = (uint32_t)le;
        inf.gain = gain;
        sIndex[name] = inf;
    }
    fclose(f);
}

OggOpusFile* OpenTrack(const Track& t) {
    int err = 0;
    return op_open_memory(t.raw.data(), t.raw.size(), &err);
}

// ── Voices (the audio thread plays them; the main thread changes them with
//    the audio locked) ──
struct Voice {
    Track* t = nullptr;
    OggOpusFile* of = nullptr;                    // its own decoder
    uint32_t pos = 0;
    float gain = 0.f, target = 0.f, step = 0.f;   // step: change per sample
    bool ended = false;                           // faded out or finished: the main thread lets it go
};
Voice sV[kVoices];
float sMaster = 1.f, sMasterTarget = 1.f;         // the volume, eased on the audio thread
std::vector<opus_int16> sBuf;                     // stereo, interleaved

void Free(Voice& v) {
    if (v.of) op_free(v.of);
    v = Voice();
}

void Mix(float* lr, int frames) {
    if ((int)sBuf.size() < frames * 2) sBuf.resize((size_t)frames * 2);
    for (Voice& v : sV) {
        if (!v.t || v.ended || !v.of) continue;
        const Track& t = *v.t;
        int done = 0;
        while (done < frames) {
            const uint32_t end = t.loops ? t.loopEnd : t.samples;
            if (v.pos >= end) {
                if (!t.loops || op_pcm_seek(v.of, t.loopStart) != 0) { v.ended = true; break; }
                v.pos = t.loopStart;
            }
            const int want = (int)std::min<uint32_t>((uint32_t)(frames - done), end - v.pos);
            const int got = op_read_stereo(v.of, sBuf.data() + done * 2, want * 2);
            if (got <= 0) { v.ended = true; break; }      // the end, or damaged: stop rather than loop on noise
            v.pos += (uint32_t)got;
            done += got;
        }
        if (done < frames) std::fill(sBuf.begin() + done * 2, sBuf.begin() + frames * 2, (opus_int16)0);
        const float norm = t.norm;
        float m = sMaster;
        const float mStep = (sMasterTarget - m) / (float)frames;
        for (int i = 0; i < frames; i++) {
            if (v.gain != v.target) {
                v.gain += v.step;
                if ((v.step > 0 && v.gain >= v.target) || (v.step <= 0 && v.gain <= v.target)) v.gain = v.target;
            }
            m += mStep;
            const float g = v.gain * norm * m;
            lr[i * 2] += sBuf[i * 2] * g;
            lr[i * 2 + 1] += sBuf[i * 2 + 1] * g;
        }
        if (v.target <= 0.f && v.gain <= 0.f) v.ended = true;
    }
    sMaster = sMasterTarget;
}

// ── Main thread ──
bool sInit = false;
bool sEnabled = true;
float sVolume = 1.f;
std::string sWant;
std::vector<Track*> sCache;
bool sNoGame = false;
std::string sWhy;
std::vector<std::string> sMissing;
void (*sLog)(const char*) = nullptr;
struct Resume { std::string name; uint32_t pos; double when; };
std::vector<Resume> sResume;

double Now() { return (double)armGetSystemTick() / (double)armGetSystemTickFreq(); }
__attribute__((format(printf, 1, 2))) void Log(const char* fmt, ...) {
    if (!sLog) return;
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    sLog(line);
}

// The loading thread: one piece at a time.
Thread sThread;
bool sThreadLive = false;
std::atomic<int> sLoadDone{ 0 };
std::string sLoadName;
Track* sLoaded = nullptr;
int sLoadErr = 0;                                 // 2: not in the app
double sLoadStart = 0;

void LoadFn(void*) {
    ReadIndex();
    auto it = sIndex.find(sLoadName);
    FILE* f = it == sIndex.end() ? nullptr : fopen(("romfs:/music/" + sLoadName + ".opus").c_str(), "rb");
    if (!f) { sLoadErr = 2; sLoadDone = 1; return; }
    Track* t = new Track();
    t->name = sLoadName;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0 && n < (32L << 20)) {
        t->raw.resize((size_t)n);
        if (fread(t->raw.data(), 1, t->raw.size(), f) != t->raw.size()) t->raw.clear();
    }
    fclose(f);
    // It must open as Opus.
    OggOpusFile* probe = t->raw.empty() ? nullptr : OpenTrack(*t);
    if (!probe) { delete t; sLoadErr = 2; sLoadDone = 1; return; }
    op_free(probe);
    const Info& inf = it->second;
    t->samples = inf.samples;
    t->loops = inf.loops;
    t->loopStart = inf.loopStart;
    t->loopEnd = std::min(inf.loopEnd, inf.samples);
    t->norm = inf.gain / 32768.f;
    t->group = GroupOf(t->name);
    sLoaded = t;
    sLoadErr = 0;
    sLoadDone = 1;
}

bool StartLoad(const std::string& name) {
    if (sThreadLive) return false;
    sLoadName = name;
    sLoaded = nullptr;
    sLoadErr = 0;
    sLoadDone = 0;
    sLoadStart = Now();
    if (R_FAILED(threadCreate(&sThread, LoadFn, nullptr, nullptr, 64 * 1024, 0x2D, -2))) return false;
    if (R_FAILED(threadStart(&sThread))) { threadClose(&sThread); return false; }
    sThreadLive = true;
    return true;
}
void JoinLoad() {
    if (!sThreadLive) return;
    threadWaitForExit(&sThread);
    threadClose(&sThread);
    sThreadLive = false;
}

size_t CacheCap() {
    // In an applet (the album) the app has far less memory than as a game.
    const AppletType at = appletGetAppletType();
    return (at == AppletType_Application || at == AppletType_SystemApplication) ? (28u << 20) : (10u << 20);
}

// Pieces no voice plays, least recently used first, until the cache fits.
void Trim() {
    size_t total = 0;
    for (Track* t : sCache) total += t->raw.size();
    const size_t cap = CacheCap();
    while (total > cap) {
        Track* oldest = nullptr;
        for (Track* t : sCache) if (t->users == 0 && (!oldest || t->lastUse < oldest->lastUse)) oldest = t;
        if (!oldest) break;
        total -= oldest->raw.size();
        sCache.erase(std::find(sCache.begin(), sCache.end(), oldest));
        delete oldest;
    }
}

Track* Cached(const std::string& name) {
    for (Track* t : sCache) if (t->name == name) return t;
    return nullptr;
}

// The voice the music is going to (the one fading in or playing).
Voice* Lead() {
    Voice* best = nullptr;
    for (Voice& v : sV) if (v.t && !v.ended && v.target > 0.f && (!best || v.target > best->target)) best = &v;
    return best;
}

void FadeOut(Voice& v, float secs) {
    v.target = 0.f;
    v.step = -std::max(v.gain, 0.001f) / std::max(1.f, secs * kRate);
}

// Where a piece picks up: the takes of one piece share their place.
std::string ResumeKey(const Track* t) { return t->group >= 0 ? "#" + std::to_string(t->group) : t->name; }

// The voices going on fade out over `secs`; unless `inStep` (a take of the
// same piece takes over), each remembers where it would be once silent.
void LeaveOthers(const Voice* keep, float secs, bool inStep) {
    const double now = Now();
    for (Voice& v : sV) {
        if (&v == keep || !v.t || v.ended || v.target <= 0.f) continue;
        FadeOut(v, secs);
        if (inStep) continue;
        const uint32_t end = v.t->loops ? v.t->loopEnd : v.t->samples;
        uint32_t at = v.pos + (uint32_t)(secs * kRate);
        if (at >= end) at = v.t->loops ? v.t->loopStart + (at - end) % std::max<uint32_t>(1, v.t->loopEnd - v.t->loopStart) : 0;
        const std::string key = ResumeKey(v.t);
        sResume.erase(std::remove_if(sResume.begin(), sResume.end(), [&](const Resume& r) { return r.name == key; }), sResume.end());
        sResume.push_back({ key, at, now });
    }
}

// Play `t` (audio locked by the caller): in step with what plays when it is
// a take of the same piece, else from where it was left a moment ago, or
// from its start.
void Start(Track* t) {
    const double now = Now();
    // Still fading out from a moment ago: it comes back from where it is.
    for (Voice& v : sV) {
        if (v.t != t || v.ended) continue;
        LeaveOthers(&v, 0.9f, false);
        v.target = 1.f;
        v.step = std::max(0.00002f, (1.f - v.gain) / (0.5f * kRate));
        t->lastUse = now;
        Log("music: %s back at %.1f s", t->name.c_str(), v.pos / (double)kRate);
        return;
    }
    Voice* lead = Lead();
    const bool inStep = lead && lead->t != t && t->group >= 0 && lead->t->group == t->group && lead->t->samples == t->samples &&
                        lead->t->loopStart == t->loopStart && lead->t->loopEnd == t->loopEnd && lead->t->loops == t->loops;
    uint32_t pos = 0;
    if (inStep) pos = lead->pos;
    else {
        const std::string key = ResumeKey(t);
        for (auto it = sResume.begin(); it != sResume.end(); ++it)
            if (it->name == key) {
                if (now - it->when < 600.0) pos = it->pos;
                sResume.erase(it);
                break;
            }
    }
    if (pos >= (t->loops ? t->loopEnd : t->samples)) pos = t->loops ? t->loopStart : 0;
    LeaveOthers(nullptr, inStep ? 1.2f : 0.9f, inStep);
    // A free voice (or the quietest).
    Voice* slot = nullptr;
    for (Voice& v : sV) if (!v.t) { slot = &v; break; }
    if (!slot) {
        for (Voice& v : sV) if (!slot || v.gain < slot->gain) slot = &v;
        if (slot->t) slot->t->users--;
        Free(*slot);
    }
    Voice& v = *slot;
    v = Voice();
    v.of = OpenTrack(*t);
    if (!v.of || (pos > 0 && op_pcm_seek(v.of, pos) != 0)) { Free(v); return; }
    v.t = t;
    t->users++;
    t->lastUse = now;
    v.pos = pos;
    v.gain = 0.f;
    v.target = 1.f;
    const float in = inStep ? 1.2f : pos > 0 ? 1.0f : 0.5f;
    v.step = 1.f / (in * kRate);
    Log(inStep ? "music: %s, in step at %.1f s" : "music: %s from %.1f s", t->name.c_str(), pos / (double)kRate);
}

}  // namespace

void Init() {
    if (sInit) return;
    sInit = true;
    sMaster = sMasterTarget = sVolume;
    Sound::SetMusicMixer(Mix);
}

void Quit() {
    if (!sInit) return;
    Sound::SetMusicMixer(nullptr);
    JoinLoad();
    delete sLoaded;
    sLoaded = nullptr;
    for (Voice& v : sV) Free(v);
    for (Track* t : sCache) delete t;
    sCache.clear();
    sInit = false;
}

void SetEnabled(bool on) { sEnabled = on; }
bool Enabled() { return sEnabled; }
void SetVolume(float v) {
    sVolume = std::max(0.f, std::min(2.f, v));
    Sound::LockAudio();
    sMasterTarget = sVolume;
    Sound::UnlockAudio();
}
float Volume() { return sVolume; }
void Want(const std::string& file) { sWant = file; }
void SetLogger(void (*fn)(const char*)) { sLog = fn; }

void Update() {
    if (!sInit || !Sound::Ready()) return;
    // A piece read: into the cache.
    if (sThreadLive && sLoadDone) {
        JoinLoad();
        if (sLoaded) {
            Log("music: read %s in %.0f ms (%u KB, gain %.2f)", sLoaded->name.c_str(), (Now() - sLoadStart) * 1000.0,
                (unsigned)(sLoaded->raw.size() / 1024), sLoaded->norm * 32768.f);
            sLoaded->lastUse = Now();
            sCache.push_back(sLoaded);
            sLoaded = nullptr;
        } else {
            sMissing.push_back(sLoadName);
            sWhy = sLoadName;
            Log("music: %s is not in the app", sLoadName.c_str());
        }
    }
    const std::string want = (sEnabled && sVolume > 0.f) ? sWant : std::string();
    Sound::LockAudio();
    // Voices done: let their pieces go.
    for (Voice& v : sV)
        if (v.t && v.ended) { v.t->users--; v.t->lastUse = Now(); Free(v); }
    Voice* lead = Lead();
    const std::string playing = lead ? lead->t->name : std::string();
    if (want != playing) {
        if (want.empty()) {
            for (Voice& v : sV) if (v.t && !v.ended && v.target > 0.f) FadeOut(v, 0.8f);
        } else if (Track* t = Cached(want)) {
            Start(t);
        }
    }
    Sound::UnlockAudio();
    // Not read yet: read it (what plays goes on meanwhile).
    if (!want.empty() && want != playing && !Cached(want) && !sThreadLive && !sNoGame &&
        std::find(sMissing.begin(), sMissing.end(), want) == sMissing.end())
        StartLoad(want);
    Trim();
}

State GetState() {
    if (!sEnabled) return State::Off;
    if (sNoGame) return State::NoGame;
    if (!sWant.empty() && std::find(sMissing.begin(), sMissing.end(), sWant) != sMissing.end()) return State::Missing;
    Sound::LockAudio();
    const bool playing = Lead() != nullptr;
    Sound::UnlockAudio();
    if (playing) return State::Playing;
    if (sThreadLive) return State::Loading;
    return State::Silent;
}

std::string Playing() {
    Sound::LockAudio();
    Voice* lead = Lead();
    const std::string n = lead ? lead->t->name : std::string();
    Sound::UnlockAudio();
    return n;
}

std::string Why() { return sWhy; }

}  // namespace Music
