#pragma once
// ui_sound.h — TomoToolNX
// The interface sounds: LLM-Selfbot's voices (webui/src/lib/uisound.ts),
// rendered by its own WebAudio code into romfs/sfx/ui.bank
// (tools/sounds/render_sounds.mjs), mixed here the way its browser graph mixes
// them: each voice straight out and partly into a reverb room, then a master
// gain of 0.6 and a compressor. Play() keeps its rules too: a least gap per
// sound, quieter arrivals when many land at once, delayed starts.

#include <cstdint>

namespace Sound {

enum class Voice : uint8_t {
    Tap, Up, Tick, On, Off, Select, Open, Menu, Ok, Info, Err, Send, Receive, Delete, Undo,
    Wind, Slide, Rise, Done, Key, Land, Pluck, Zip, Seat, Glint, Dawn, Whoosh, Settle, Grab, Release,
    Count
};

struct Opts {
    float p      = 0.5f;   // 0..1: pitch of a tick or note (see each voice)
    bool  strong = false;  // a major mark / a hard knock
    float level  = 1.f;    // 0..1, softer
    float delay  = 0.f;    // ms from now (sample-accurate)
};

bool  Init();              // opens the audio device and loads the bank; false = silent
void  Quit();
void  Play(Voice v, const Opts& o = {});
// A run of `settle` sounds, one per card arriving (Big Picture's arrivals()).
void  Arrivals(int count, float firstMs = 90.f, float stepMs = 62.f, float level = 0.55f);
void  SetEnabled(bool on);
bool  Enabled();
// Whose sounds: 0 Tomodachi Life's own (romfs/sfx/game.bank, the default),
// 1 LLM-Selfbot's (romfs/sfx/ui.bank). Before Init, or any time after.
void  SetStyle(int style);
int   Style();
void  SetVolume(float v);  // 0..2, 1 = as made
float Volume();
// Where something is on screen → a tick's pitch: higher to the right and up.
float PitchAt(int x, int y);
// Optional: a line per sound played, for debug.log.
void  SetLogger(void (*fn)(const char* line));
const char* NameOf(Voice v);

// The music (music.cpp) shares the output: `fn` runs on the audio thread for
// every buffer and adds `frames` stereo frames (interleaved, -1..1) into
// `lr`, after the interface sounds' compressor. LockAudio / UnlockAudio keep
// the audio thread out while the music's state changes. Ready(): the output
// is open.
void  SetMusicMixer(void (*fn)(float* lr, int frames));
void  LockAudio();
void  UnlockAudio();
bool  Ready();

}  // namespace Sound
