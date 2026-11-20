// ui_sound.cpp — TomoToolNX
// See ui_sound.h. The mixing follows LLM-Selfbot's WebAudio graph:
//
//   voice ──► out (gain 0.6 × volume) ──► compressor ──► speakers
//     └─wet─► room (0.9) ─► convolver ─► lowpass 4800 Hz ─► out
//
// The bank holds, for every variant of every voice, the dry part and the part
// sent into the room (rendered in Chrome by LLM-Selfbot's own code). The room
// is built here the way uisound.ts builds it: 2.6 s of stereo noise fading as
// (1 - t/T)^3.2, scaled the way Chrome's ConvolverNode normalizes an impulse.
// To keep it cheap it runs at 16 kHz (everything above 4.8 kHz is filtered out
// anyway), as a uniformly partitioned FFT convolution. The compressor uses
// Chrome's static curve and its automatic make-up gain.

#include "ui_sound.h"

#include <SDL2/SDL.h>
#include <switch.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace Sound {
namespace {

constexpr int   kRate    = 48000;
constexpr int   kVoices  = 20;
constexpr int   kVoiceN  = (int)Voice::Count;

const char* const kNames[kVoiceN] = {
    "tap", "up", "tick", "on", "off", "select", "open", "menu", "ok", "info", "err", "send", "receive", "delete",
    "undo", "wind", "slide", "rise", "done", "key", "land", "pluck", "zip", "seat", "glint", "dawn", "whoosh",
    "settle", "grab", "release",
};
// uisound.ts GAP (ms): never a burst.
const uint16_t kGap[kVoiceN] = {
    35, 35, 24, 60, 60, 60, 80, 120, 300, 300, 300, 200, 400, 120,
    120, 26, 30, 45, 500, 22, 60, 28, 60, 50, 12, 800, 120,
    45, 60, 60,
};
// uisound.ts ARRIVING: quieter when many land at once.
bool Arriving(Voice v) {
    switch (v) {
        case Voice::Wind: case Voice::Slide: case Voice::Rise: case Voice::Done: case Voice::Key:
        case Voice::Land: case Voice::Pluck: case Voice::Zip: case Voice::Seat: case Voice::Glint:
        case Voice::Settle: return true;
        default: return false;
    }
}

struct Variant {
    float p; bool strong; uint8_t take;
    uint32_t frames;
    const int16_t* dry;
    const int16_t* wet;   // may be null
};

std::vector<uint8_t> sBank;                // decompressed bank
std::vector<Variant> sVars[kVoiceN];
bool  sReady   = false;
bool  sEnabled = true;
float sVolume  = 1.f;
SDL_AudioDeviceID sDev = 0;
void (*sLog)(const char*) = nullptr;
uint32_t sRng = 0x9e3779b9u;

uint32_t Rand() { sRng ^= sRng << 13; sRng ^= sRng >> 17; sRng ^= sRng << 5; return sRng; }
double NowMs() { return (double)armGetSystemTick() * 1000.0 / (double)armGetSystemTickFreq(); }

// ── Commands to the audio thread ────────────────────────────────────────────
struct Command { const Variant* v; float rate; float gain; int delay; };
Command sQueue[64];
int     sQueueN = 0;
float   sOutGain = 0.6f;   // 0.6 × volume, read by the audio thread

// ── FFT (radix 2, in place) ─────────────────────────────────────────────────
struct Fft {
    int n = 0;
    std::vector<float> c, s;
    std::vector<int> rev;
    void Init(int N) {
        n = N;
        int bits = 0; while ((1 << bits) < N) bits++;
        rev.resize(N);
        for (int i = 0; i < N; i++) { int r = 0; for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b); rev[i] = r; }
        c.resize(N / 2); s.resize(N / 2);
        for (int i = 0; i < N / 2; i++) { c[i] = (float)cos(2 * M_PI * i / N); s[i] = (float)sin(2 * M_PI * i / N); }
    }
    void Run(float* re, float* im, bool inverse) const {
        for (int i = 0; i < n; i++) { int j = rev[i]; if (j > i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); } }
        for (int len = 2; len <= n; len <<= 1) {
            const int half = len >> 1, step = n / len;
            for (int i = 0; i < n; i += len) {
                for (int k = 0; k < half; k++) {
                    const float wr = c[k * step], wi = inverse ? s[k * step] : -s[k * step];
                    const int a = i + k, b = a + half;
                    const float tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - tr; im[b] = im[a] - ti;
                    re[a] += tr;        im[a] += ti;
                }
            }
        }
    }
};

// ── The room ─────────────────────────────────────────────────────────────────
constexpr int   kRoomRate = 16000;
constexpr int   kB = 256, kN = 512;     // block, FFT size

struct Fir {                            // windowed-sinc low-pass at 48 kHz, for /3 and ×3
    static constexpr int L = 63;
    float h[L];
    void Init(double cutoffHz) {
        const double fc = cutoffHz / kRate;
        double sum = 0;
        for (int i = 0; i < L; i++) {
            const double m = i - (L - 1) / 2.0;
            const double sinc = m == 0 ? 2 * fc : sin(2 * M_PI * fc * m) / (M_PI * m);
            const double w = 0.42 - 0.5 * cos(2 * M_PI * i / (L - 1)) + 0.08 * cos(4 * M_PI * i / (L - 1));
            h[i] = (float)(sinc * w);
            sum += h[i];
        }
        for (int i = 0; i < L; i++) h[i] = (float)(h[i] / sum);
    }
};

struct Biquad {                         // WebAudio's low-pass (Q in dB)
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void LowPass(double f, double qDb, double rate) {
        const double w0 = 2 * M_PI * f / rate, cw = cos(w0), alpha = sin(w0) / (2 * pow(10.0, qDb / 20.0));
        const double a0 = 1 + alpha;
        b0 = (float)((1 - cw) / 2 / a0); b1 = (float)((1 - cw) / a0); b2 = b0;
        a1 = (float)(-2 * cw / a0); a2 = (float)((1 - alpha) / a0);
    }
    float Run(float x) { const float y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};

struct Room {
    Fft fft;
    int P = 0;
    std::vector<float> gre, gim;        // P × kN impulse spectra (left + j·right)
    std::vector<float> xre, xim;        // P × kN input spectra, a ring
    int head = 0;
    float inBuf[kN] = {};
    float tre[kN], tim[kN];
    int zeroBlocks = 1 << 30;
    // 48 k → 16 k
    Fir fir;
    float dHist[Fir::L * 2] = {}; int dPos = 0, dPhase = 0;   // doubled: no wrap in the loop
    float block[kB]; int blockN = 0;
    // 16 k out FIFO, then ×3 → 48 k
    std::vector<float> fifoL, fifoR; int fHead = 0, fTail = 0, fSize = 0;
    float uHistL[Fir::L / 3 + 1] = {}, uHistR[Fir::L / 3 + 1] = {}; int uPhase = 0;
    Biquad darkL, darkR;

    void Init() {
        fft.Init(kN);
        // uisound.ts impulse(): 2.6 s, decay 3.2, two channels of noise.
        const int len = (int)(kRoomRate * 2.6);
        std::vector<float> ir[2] = { std::vector<float>(len), std::vector<float>(len) };
        double power = 0;
        for (int ch = 0; ch < 2; ch++)
            for (int i = 0; i < len; i++) {
                const float r = (float)(Rand() / 4294967296.0) * 2.f - 1.f;
                ir[ch][i] = r * (float)pow(1.0 - (double)i / len, 3.2);
                power += (double)ir[ch][i] * ir[ch][i];
            }
        // Chrome: scale = 1/rms × -58 dB × 44100/rate. Computed for the 48 kHz
        // room and carried to 16 kHz (×√3 keeps the level of a band-limited
        // signal the same: the 16 kHz impulse has a third of the samples).
        power = sqrt(power / (2.0 * len));
        const double scale = (1.0 / std::max(power, 0.000125)) * 0.0012589254 * (44100.0 / 48000.0) * sqrt(3.0);
        P = (len + kB - 1) / kB;
        gre.assign((size_t)P * kN, 0.f); gim.assign((size_t)P * kN, 0.f);
        xre.assign((size_t)P * kN, 0.f); xim.assign((size_t)P * kN, 0.f);
        for (int p = 0; p < P; p++) {
            float* re = &gre[(size_t)p * kN]; float* im = &gim[(size_t)p * kN];
            for (int i = 0; i < kB; i++) {
                const int k = p * kB + i;
                if (k >= len) break;
                re[i] = (float)(ir[0][k] * scale);
                im[i] = (float)(ir[1][k] * scale);
            }
            fft.Run(re, im, false);
        }
        fir.Init(7000.0);
        darkL.LowPass(4800, 1, kRoomRate); darkR.LowPass(4800, 1, kRoomRate);
        fifoL.assign(2048, 0.f); fifoR.assign(2048, 0.f);
        // A block of latency so the ×3 side never runs dry.
        fHead = 0; fTail = kB + 16; fSize = kB + 16;
    }

    void Convolve() {
        bool silent = true;
        for (int i = 0; i < kB; i++) if (block[i] != 0.f) { silent = false; break; }
        if (silent) { if (zeroBlocks < P + 1) zeroBlocks++; } else zeroBlocks = 0;
        float outL[kB], outR[kB];
        if (zeroBlocks > P) {
            memset(outL, 0, sizeof outL); memset(outR, 0, sizeof outR);
        } else {
            memmove(inBuf, inBuf + kB, kB * sizeof(float));
            memcpy(inBuf + kB, block, kB * sizeof(float));
            head = (head + 1) % P;
            float* re = &xre[(size_t)head * kN]; float* im = &xim[(size_t)head * kN];
            memcpy(re, inBuf, kN * sizeof(float));
            memset(im, 0, kN * sizeof(float));
            fft.Run(re, im, false);
            memset(tre, 0, sizeof tre); memset(tim, 0, sizeof tim);
            for (int p = 0; p < P; p++) {
                const int slot = (head - p + P) % P;
                const float* xr = &xre[(size_t)slot * kN]; const float* xi = &xim[(size_t)slot * kN];
                const float* hr = &gre[(size_t)p * kN];    const float* hi = &gim[(size_t)p * kN];
                for (int k = 0; k < kN; k++) {
                    tre[k] += xr[k] * hr[k] - xi[k] * hi[k];
                    tim[k] += xr[k] * hi[k] + xi[k] * hr[k];
                }
            }
            fft.Run(tre, tim, true);
            const float inv = 1.f / kN;
            for (int i = 0; i < kB; i++) {
                outL[i] = darkL.Run(tre[kB + i] * inv);
                outR[i] = darkR.Run(tim[kB + i] * inv);
            }
        }
        for (int i = 0; i < kB && fSize < (int)fifoL.size(); i++) {
            fifoL[fTail] = outL[i]; fifoR[fTail] = outR[i];
            fTail = (fTail + 1) % (int)fifoL.size(); fSize++;
        }
    }

    // One 48 kHz sample of wet signal in, one stereo 48 kHz sample of room out.
    void Run(float wet, float& L, float& R) {
        dHist[dPos] = wet; dHist[dPos + Fir::L] = wet; dPos = (dPos + 1) % Fir::L;
        if (++dPhase == 3) {
            dPhase = 0;
            float acc = 0;
            const float* x = dHist + dPos + Fir::L - 1;          // newest sample, older ones before it
            for (int k = 0; k < Fir::L; k++) acc += fir.h[k] * x[-k];
            block[blockN++] = acc;
            if (blockN == kB) { Convolve(); blockN = 0; }
        }
        constexpr int T = Fir::L / 3 + 1;
        if (uPhase == 0) {
            float l = 0, r = 0;
            if (fSize > 0) { l = fifoL[fHead]; r = fifoR[fHead]; fHead = (fHead + 1) % (int)fifoL.size(); fSize--; }
            memmove(uHistL + 1, uHistL, (T - 1) * sizeof(float));
            memmove(uHistR + 1, uHistR, (T - 1) * sizeof(float));
            uHistL[0] = l; uHistR[0] = r;
        }
        float l = 0, r = 0;
        for (int j = 0; j < T; j++) {
            const int k = uPhase + 3 * j;
            if (k >= Fir::L) break;
            l += fir.h[k] * uHistL[j];
            r += fir.h[k] * uHistR[j];
        }
        L = l * 3.f; R = r * 3.f;
        uPhase = (uPhase + 1) % 3;
    }
};

// ── Chrome's compressor: static curve, make-up gain, simple ballistics ──────
struct Compressor {
    float dbThreshold = -20, dbKnee = 14, ratio = 3;
    float linThreshold = 0, kneeThreshold = 0, kneeThresholdDb = 0, yKneeThresholdDb = 0, slope = 0, k = 1;
    float makeup = 1, gain = 1, atk = 0, rel = 0;
    static constexpr int kPre = (int)(0.006 * kRate);
    float delayL[kPre] = {}, delayR[kPre] = {}; int dPos = 0;

    static float ToDb(float x) { return x <= 0.000001f ? -120.f : 20.f * log10f(x); }
    static float ToLin(float d) { return powf(10.f, d / 20.f); }
    float Knee(float x, float kk) const {
        if (x < linThreshold) return x;
        return linThreshold + (1 - expf(-kk * (x - linThreshold))) / kk;
    }
    float SlopeAt(float x, float kk) const {
        if (x < linThreshold) return 1;
        const float x2 = x * 1.001f;
        const float xDb = ToDb(x), x2Db = ToDb(x2), yDb = ToDb(Knee(x, kk)), y2Db = ToDb(Knee(x2, kk));
        return (y2Db - yDb) / (x2Db - xDb);
    }
    float KAtSlope(float desired) const {
        const float x = ToLin(dbThreshold + dbKnee);
        float minK = 0.1f, maxK = 10000.f, kk = 5.f;
        for (int i = 0; i < 15; i++) {
            if (SlopeAt(x, kk) < desired) maxK = kk; else minK = kk;
            kk = sqrtf(minK * maxK);
        }
        return kk;
    }
    float Saturate(float x) const {
        if (x < kneeThreshold) return Knee(x, k);
        return ToLin(yKneeThresholdDb + slope * (ToDb(x) - kneeThresholdDb));
    }
    void Init() {
        linThreshold = ToLin(dbThreshold);
        slope = 1.f / ratio;
        k = KAtSlope(slope);
        kneeThresholdDb = dbThreshold + dbKnee;
        kneeThreshold = ToLin(kneeThresholdDb);
        yKneeThresholdDb = ToDb(Knee(kneeThreshold, k));
        makeup = powf(1.f / Saturate(1.f), 0.6f);
        atk = 1.f - expf(-1.f / (0.002f * kRate));
        rel = 1.f - expf(-1.f / (0.2f * kRate));
    }
    void Run(float& L, float& R) {
        const float x = std::max(fabsf(L), fabsf(R));
        const float target = x > linThreshold ? Saturate(x) / x : 1.f;
        gain += (target - gain) * (target < gain ? atk : rel);
        const float dl = delayL[dPos], dr = delayR[dPos];
        delayL[dPos] = L; delayR[dPos] = R; dPos = (dPos + 1) % kPre;
        L = dl * gain * makeup; R = dr * gain * makeup;
    }
};

// ── Voices ───────────────────────────────────────────────────────────────────
struct Playing { const Variant* v = nullptr; double pos = 0; float rate = 1, gain = 1; int delay = 0; uint32_t age = 0; };
Playing sPlay[kVoices];
uint32_t sAge = 0;
Room* sRoom = nullptr;
Compressor sComp;
std::vector<float> sDry, sWet, sMusic;
void (*sMusicMix)(float*, int) = nullptr;

// The interface draws on core 0. SDL starts its audio thread on that same core
// at the same priority, so a long frame (the very first one prepares every
// font and icon) kept the mixer from running and the sound stuttered. On its
// first call the mixer moves to another core the app may use, one step above
// the drawing thread.
void TuneAudioThread() {
    u64 mask = 0;
    if (R_SUCCEEDED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) {
        for (int c : { 2, 1 }) {
            if (mask & (1ull << c)) { svcSetThreadCoreMask(CUR_THREAD_HANDLE, c, 1u << c); break; }
        }
    }
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2B);
}

void Callback(void*, Uint8* stream, int len) {
    static bool tuned = false;
    if (!tuned) { tuned = true; TuneAudioThread(); }
    int16_t* out = (int16_t*)stream;
    const int frames = len / 4;
    for (int q = 0; q < sQueueN; q++) {
        int slot = -1; uint32_t oldest = UINT32_MAX;
        for (int i = 0; i < kVoices; i++) {
            if (!sPlay[i].v) { slot = i; break; }
            if (sPlay[i].age < oldest) { oldest = sPlay[i].age; slot = i; }
        }
        sPlay[slot] = { sQueue[q].v, 0.0, sQueue[q].rate, sQueue[q].gain, sQueue[q].delay, ++sAge };
    }
    sQueueN = 0;
    if ((int)sDry.size() < frames) { sDry.resize(frames); sWet.resize(frames); }
    std::fill(sDry.begin(), sDry.begin() + frames, 0.f);
    std::fill(sWet.begin(), sWet.begin() + frames, 0.f);
    constexpr float kS = 1.f / 32768.f;
    for (Playing& pl : sPlay) {
        if (!pl.v) continue;
        const Variant& v = *pl.v;
        for (int i = 0; i < frames; i++) {
            if (pl.delay > 0) { pl.delay--; continue; }
            const uint32_t i0 = (uint32_t)pl.pos;
            if (i0 + 1 >= v.frames) { pl.v = nullptr; break; }
            const float f = (float)(pl.pos - i0);
            const float d = (v.dry[i0] + (v.dry[i0 + 1] - v.dry[i0]) * f) * kS;
            sDry[i] += d * pl.gain;
            if (v.wet) sWet[i] += (v.wet[i0] + (v.wet[i0 + 1] - v.wet[i0]) * f) * kS * pl.gain;
            pl.pos += pl.rate;
        }
    }
    // The music, beside the interface sounds rather than through their
    // compressor (it would pump the music on every tick).
    const float* mus = nullptr;
    if (sMusicMix) {
        if ((int)sMusic.size() < frames * 2) sMusic.resize((size_t)frames * 2);
        std::fill(sMusic.begin(), sMusic.begin() + frames * 2, 0.f);
        sMusicMix(sMusic.data(), frames);
        mus = sMusic.data();
    }
    const float g = sOutGain;
    for (int i = 0; i < frames; i++) {
        float rl = 0, rr = 0;
        if (sRoom) sRoom->Run(sWet[i] * 0.9f, rl, rr);
        float L = (sDry[i] + rl) * g, R = (sDry[i] + rr) * g;
        sComp.Run(L, R);
        if (mus) { L += mus[i * 2]; R += mus[i * 2 + 1]; }
        L = std::max(-1.f, std::min(1.f, L)); R = std::max(-1.f, std::min(1.f, R));
        out[i * 2] = (int16_t)lrintf(L * 32767.f);
        out[i * 2 + 1] = (int16_t)lrintf(R * 32767.f);
    }
}

// A bank into `bank` / `vars` (the caller swaps them in).
bool LoadBankInto(const char* path, std::vector<uint8_t>& bank, std::vector<Variant> (&vars)[kVoiceN]) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> packed(n > 0 ? (size_t)n : 0);
    const bool ok = n > 8 && fread(packed.data(), 1, packed.size(), f) == packed.size();
    fclose(f);
    if (!ok || memcmp(packed.data(), "TTSZ", 4) != 0) return false;
    uint32_t rawSize; memcpy(&rawSize, packed.data() + 4, 4);
    if (rawSize < 16 || rawSize > 64u * 1024 * 1024) return false;
    bank.resize(rawSize);
    uLongf dst = rawSize;
    if (uncompress(bank.data(), &dst, packed.data() + 8, (uLong)(packed.size() - 8)) != Z_OK || dst != rawSize) return false;
    const uint8_t* b = bank.data();
    if (memcmp(b, "TTSB", 4) != 0) return false;
    uint32_t version, rate, count;
    memcpy(&version, b + 4, 4); memcpy(&rate, b + 8, 4); memcpy(&count, b + 12, 4);
    if (version != 1 || rate != kRate) return false;
    const size_t headBytes = 16 + (size_t)count * 36;
    if (headBytes > rawSize) return false;
    const int16_t* data = (const int16_t*)(b + headBytes);
    const size_t samples = (rawSize - headBytes) / 2;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* e = b + 16 + i * 36;
        char name[13] = {}; memcpy(name, e, 12);
        Variant v{};
        memcpy(&v.p, e + 12, 4);
        v.strong = e[16] != 0; v.take = e[17];
        uint32_t frames, dry, wet;
        memcpy(&frames, e + 20, 4); memcpy(&dry, e + 24, 4); memcpy(&wet, e + 28, 4);
        if ((size_t)dry + frames > samples) continue;
        v.frames = frames;
        v.dry = data + dry;
        v.wet = (wet != 0xffffffffu && (size_t)wet + frames <= samples) ? data + wet : nullptr;
        for (int k = 0; k < kVoiceN; k++)
            if (strcmp(name, kNames[k]) == 0) { vars[k].push_back(v); break; }
    }
    return true;
}

// The game's own sounds (game.bank, tools/sounds/build_game_bank.py) or
// LLM-Selfbot's (ui.bank); the other when one is missing.
int sStyle = 0;
const char* BankPath(int style) { return style == 0 ? "romfs:/sfx/game.bank" : "romfs:/sfx/ui.bank"; }
bool LoadStyle(int style, std::vector<uint8_t>& bank, std::vector<Variant> (&vars)[kVoiceN]) {
    if (LoadBankInto(BankPath(style), bank, vars)) return true;
    for (auto& v : vars) v.clear();
    return LoadBankInto(BankPath(style ? 0 : 1), bank, vars);
}

const Variant* Pick(Voice voice, const Opts& o, float& rate) {
    const auto& list = sVars[(int)voice];
    if (list.empty()) return nullptr;
    bool haveStrong = false;
    for (const Variant& v : list) if (v.strong == o.strong) { haveStrong = true; break; }
    float best = 1e9f;
    for (const Variant& v : list) if (!haveStrong || v.strong == o.strong) best = std::min(best, fabsf(v.p - o.p));
    const Variant* takes[8]; int n = 0;
    for (const Variant& v : list)
        if ((!haveStrong || v.strong == o.strong) && fabsf(fabsf(v.p - o.p) - best) < 1e-4f && n < 8) takes[n++] = &v;
    const Variant* v = takes[Rand() % (uint32_t)n];
    // Voices whose pitch slides with p: close the gap to the nearest variant.
    const float p = std::max(0.f, std::min(1.f, o.p));
    rate = 1.f;
    switch (voice) {
        case Voice::Tick: rate = (3200 + 1400 * p) / (3200 + 1400 * v->p); break;
        case Voice::Wind: rate = (2600 + 1600 * p) / (2600 + 1600 * v->p); break;
        case Voice::Seat: if (!o.strong) rate = (1300 + 700 * p) / (1300 + 700 * v->p); break;
        default: break;
    }
    return v;
}

std::vector<double> sBooked[kVoiceN];
std::vector<double> sArrivals;

}  // namespace

bool Init() {
    if (sReady) return true;
    if (!LoadStyle(sStyle, sBank, sVars)) { if (sLog) sLog("sound: no sound bank, running silent"); return false; }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
    sRng ^= (uint32_t)armGetSystemTick();
    sRoom = new Room();
    sRoom->Init();
    sComp.Init();
    SDL_AudioSpec want{}, have{};
    // 1024 frames (21 ms): room for a late wake-up without a click, still well
    // under what reads as a delay on a press.
    want.freq = kRate; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; want.callback = Callback;
    sDev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!sDev) { delete sRoom; sRoom = nullptr; SDL_QuitSubSystem(SDL_INIT_AUDIO); return false; }
    sOutGain = 0.6f * sVolume;
    SDL_PauseAudioDevice(sDev, 0);
    sReady = true;
    return true;
}

void Quit() {
    if (!sReady) return;
    SDL_CloseAudioDevice(sDev);
    sDev = 0;
    delete sRoom; sRoom = nullptr;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    for (auto& v : sVars) v.clear();
    sBank.clear(); sBank.shrink_to_fit();
    sReady = false;
}

void Play(Voice voice, const Opts& o) {
    if (!sReady || !sEnabled || (int)voice >= kVoiceN) return;
    const double now = NowMs();
    const double when = now + std::max(0.f, o.delay);
    auto& times = sBooked[(int)voice];
    times.erase(std::remove_if(times.begin(), times.end(), [&](double t) { return t <= now - 3000; }), times.end());
    for (double t : times) if (fabs(t - when) < kGap[(int)voice]) return;
    float rate;
    const Variant* v = Pick(voice, o, rate);
    if (!v) return;
    times.push_back(when);
    float level = 1.f;
    if (Arriving(voice)) {
        sArrivals.erase(std::remove_if(sArrivals.begin(), sArrivals.end(), [&](double t) { return now - t >= 900; }), sArrivals.end());
        level = std::max(0.4f, 1.f / (1.f + 0.08f * (float)sArrivals.size()));
        sArrivals.push_back(now);
    }
    level *= std::max(0.05f, std::min(1.f, o.level));
    SDL_LockAudioDevice(sDev);
    if (sQueueN < (int)(sizeof sQueue / sizeof sQueue[0]))
        sQueue[sQueueN++] = { v, rate, level, (int)((when - now) * kRate / 1000.0) };
    SDL_UnlockAudioDevice(sDev);
    if (sLog) {
        char line[96];
        snprintf(line, sizeof line, "sound %s p=%.2f%s level=%.2f delay=%.0f", kNames[(int)voice], o.p,
                 o.strong ? " strong" : "", level, std::max(0.f, o.delay));
        sLog(line);
    }
}

void Arrivals(int count, float firstMs, float stepMs, float level) {
    for (int i = 0; i < count; i++) Play(Voice::Settle, { 0.5f, false, level, firstMs + i * stepMs });
}

void SetEnabled(bool on) { sEnabled = on; }

void SetStyle(int style) {
    style = style == 1 ? 1 : 0;
    if (style == sStyle) return;
    sStyle = style;
    if (!sReady) return;
    // Read before the swap: the audio thread only waits for the swap itself.
    std::vector<uint8_t> bank;
    std::vector<Variant> vars[kVoiceN];
    if (!LoadStyle(sStyle, bank, vars)) return;
    SDL_LockAudioDevice(sDev);
    for (Playing& p : sPlay) p.v = nullptr;
    sQueueN = 0;
    sBank.swap(bank);
    for (int k = 0; k < kVoiceN; k++) sVars[k].swap(vars[k]);
    SDL_UnlockAudioDevice(sDev);
}
int Style() { return sStyle; }
bool Enabled() { return sEnabled; }
void SetVolume(float v) {
    sVolume = std::max(0.f, std::min(2.f, v));
    if (sDev) { SDL_LockAudioDevice(sDev); sOutGain = 0.6f * sVolume; SDL_UnlockAudioDevice(sDev); }
    else sOutGain = 0.6f * sVolume;
}
float Volume() { return sVolume; }
float PitchAt(int x, int y) {
    const float p = ((float)x / 1280.f + (1.f - (float)y / 720.f)) / 2.f;
    return std::max(0.f, std::min(1.f, p));
}
void SetLogger(void (*fn)(const char*)) { sLog = fn; }
const char* NameOf(Voice v) { return (int)v < kVoiceN ? kNames[(int)v] : "?"; }

void SetMusicMixer(void (*fn)(float*, int)) {
    if (sDev) SDL_LockAudioDevice(sDev);
    sMusicMix = fn;
    if (sDev) SDL_UnlockAudioDevice(sDev);
}
void LockAudio()   { if (sDev) SDL_LockAudioDevice(sDev); }
void UnlockAudio() { if (sDev) SDL_UnlockAudioDevice(sDev); }
bool Ready()       { return sReady; }

}  // namespace Sound
