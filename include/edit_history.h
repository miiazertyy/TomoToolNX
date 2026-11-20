#pragma once
// edit_history.h — TomoToolNX
// Every change made to the open saves, kept so it can be taken back: the
// sidebar's Undo button takes back the last change (one press, or a value
// held and stepped, counts as one), and held, everything since the island was
// opened or last saved. A step is a setting, not each press: changing the same
// value again right after is the same step, and a value put back the way it
// was takes its step away (the count goes back down). SaveEditor reports each entry just before it changes
// (SetChangeHook), so whatever page or tool made the change, it can be undone.

#include "save_editor.h"

namespace History {

// The saves to keep, each with the flag that says it has unsaved changes.
void Watch(SaveEditor::SavFile* s, bool* dirty);
// Hooks SaveEditor (call once, after Watch).
void Install();
// Once a frame: changes made in one frame are one step.
void Tick();
bool CanUndo();
int  Steps();
// Takes back the last step. Returns which watched saves it changed (bit i =
// the i-th Watch), 0 when there was nothing to undo.
unsigned Undo();
// Forget everything (after a save, or when the files were read again).
void Clear();

}  // namespace History
