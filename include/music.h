#pragma once
// music.h — TomoToolNX
// The game's own music behind the app: each place plays the piece the game
// plays for the same thing (the island by day or by night, the Mii editor,
// the texture editor, island editing, the encyclopedia, local play...).
//
// Nothing of the game ships with the app: the pieces are read from the copy
// of the game on this console (GameData::Root(), its RomFS), the way the Mii
// pictures are made. The game keeps its music as BWAV files (DSP-ADPCM, 48
// kHz, with loop points); they are decoded as they play and mixed into the
// interface sounds' output (Sound::SetMusicMixer).
//
// Some pieces come in several takes that run in step (the title, the day and
// the island-editing arrangements of the island's theme; two arrangements of
// the Mii editor's and of the texture editor's): going from one to another
// crossfades without a jump, as the game does. Other changes fade one piece
// out and the next in; a piece left a moment ago picks up where it was.

#include <string>

namespace Music {

// The pieces Settings can play everywhere instead: a code (the config value,
// and the language key "music.piece.<code>"), the game's file, and the file
// of its second take when it has one.
struct Piece { const char* code; const char* file; const char* alt; };
extern const Piece kPieces[];
extern const int kPieceCount;
// The piece a file is a take of ("" when none).
const char* PieceOf(const std::string& file);

enum class State : int {
    Off,        // turned off in Settings
    Silent,     // on, nothing asked for
    Loading,    // reading a piece, nothing playing yet
    Playing,
    NoGame,     // the game's files cannot be read here (Why(): GameData::Why())
    Missing,    // this copy of the game has no such file (Why(): its name)
};

void Init();                      // after Sound::Init
void Quit();                      // stops at once
void SetEnabled(bool on);
bool Enabled();
void SetVolume(float v);          // 0..2; 1 = about as loud as in the game
float Volume();
// What this place would like to hear: one of the game's files (no folder, no
// extension, e.g. "BGM_MiiEditor_01a"), or "" for silence. Call it every
// frame; a change of piece fades from one to the other.
void Want(const std::string& file);
void Update();                    // every frame, on the main thread
State GetState();
std::string Playing();            // the file heard now ("" when none)
std::string Why();
// Optional: a line per load and change, for debug.log.
void SetLogger(void (*fn)(const char* line));

}  // namespace Music
