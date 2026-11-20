// edit_history.cpp — TomoToolNX. See edit_history.h.

#include "edit_history.h"
#include "ui_motion.h"

#include <vector>

namespace History {
namespace {

struct Watched { SaveEditor::SavFile* s; bool* dirty; };
std::vector<Watched> sWatched;

// An entry as it was before a step changed it.
struct Snap { int file; uint32_t hash; SaveEditor::Entry before; };
struct Step {
    uint64_t frame = 0;
    double last = 0;                 // when it was last added to
    std::vector<Snap> snaps;
    std::vector<char> wasDirty;      // each file's flag before the step
    size_t bytes = 0;
    bool checked = false;            // looked at once it was finished
};
std::vector<Step> sSteps;
size_t sBytes = 0;
uint64_t sFrame = 1;
bool sMuted = false;                 // while undoing

constexpr size_t kMaxSteps = 400;
constexpr size_t kMaxBytes = 24u << 20;
// Held ◀ ▶, a slider dragged, a paint stroke: the same entries changed again
// within this long are still the same step.
constexpr double kJoin = 0.7;

int FileOf(const SaveEditor::SavFile* s) {
    for (size_t i = 0; i < sWatched.size(); i++) if (sWatched[i].s == s) return (int)i;
    return -1;
}

bool Holds(const Step& st, int file, uint32_t hash) {
    for (const Snap& sn : st.snaps) if (sn.file == file && sn.hash == hash) return true;
    return false;
}

void Trim() {
    while (!sSteps.empty() && (sSteps.size() > kMaxSteps || sBytes > kMaxBytes)) {
        sBytes -= sSteps.front().bytes;
        sSteps.erase(sSteps.begin());
    }
}

void OnChange(SaveEditor::SavFile& s, const SaveEditor::Entry& e) {
    if (sMuted) return;
    const int file = FileOf(&s);
    if (file < 0) return;
    const double now = Motion::Now();
    Step* st = sSteps.empty() ? nullptr : &sSteps.back();
    if (st && st->frame != sFrame) {
        // A later frame: the same thing changed again soon after is the same
        // step; anything else starts a new one.
        if (now - st->last < kJoin && Holds(*st, file, e.hash)) { st->last = now; st->frame = sFrame; st->checked = false; return; }
        st = nullptr;
    }
    if (!st) {
        sSteps.emplace_back();
        st = &sSteps.back();
        st->frame = sFrame;
        st->wasDirty.resize(sWatched.size());
        for (size_t i = 0; i < sWatched.size(); i++) st->wasDirty[i] = sWatched[i].dirty && *sWatched[i].dirty;
    }
    st->last = now;
    if (Holds(*st, file, e.hash)) return;
    st->snaps.push_back({ file, e.hash, e });
    const size_t b = e.payload.size() + sizeof(Snap);
    st->bytes += b;
    sBytes += b;
    Trim();
}

void OnLoad(SaveEditor::SavFile& s) {
    // The file was read again: the old entries the steps hold are not its own
    // any more, so putting them back would mix two versions.
    if (FileOf(&s) >= 0) Clear();
}

}  // namespace

void Watch(SaveEditor::SavFile* s, bool* dirty) {
    if (FileOf(s) < 0) sWatched.push_back({ s, dirty });
}

void Install() {
    SaveEditor::SetChangeHook(OnChange);
    SaveEditor::SetLoadHook(OnLoad);
}

// Where two versions of an entry differ: [lo, hi) in its bytes, or all of it
// (another type, an inline value, another size). false when they are equal.
static bool Diff(const SaveEditor::Entry& a, const SaveEditor::Entry& b, size_t& lo, size_t& hi) {
    if (a.type != b.type || a.inlineRaw != b.inlineRaw || a.payload.size() != b.payload.size()) { lo = 0; hi = (size_t)-1; return true; }
    size_t i = 0, n = a.payload.size();
    while (i < n && a.payload[i] == b.payload[i]) i++;
    if (i == n) return false;
    size_t j = n;
    while (j > i && a.payload[j - 1] == b.payload[j - 1]) j--;
    lo = i; hi = j;
    return true;
}
static const SaveEditor::Entry* Current(int file, uint32_t hash) {
    for (const auto& e : sWatched[file].s->entries) if (e.hash == hash) return &e;
    return nullptr;
}
static const Snap* SnapOf(const Step& st, int file, uint32_t hash) {
    for (const Snap& sn : st.snaps) if (sn.file == file && sn.hash == hash) return &sn;
    return nullptr;
}
static bool SameEntries(const Step& a, const Step& b) {
    if (a.snaps.size() != b.snaps.size()) return false;
    for (const Snap& sn : a.snaps) if (!SnapOf(b, sn.file, sn.hash)) return false;
    return true;
}

// A setter given the value already there still reports a change: a step
// whose entries all end up as they were is no step at all.
static bool Changes(const Step& st) {
    for (const Snap& sn : st.snaps) {
        const SaveEditor::SavFile* s = sWatched[sn.file].s;
        for (const auto& e : s->entries) {
            if (e.hash != sn.hash) continue;
            if (e.type != sn.before.type || e.inlineRaw != sn.before.inlineRaw || e.payload != sn.before.payload) return true;
            break;
        }
    }
    return false;
}

// The last step, once finished, against the ones before it: a setting is one
// step, however many presses it took, and gone when it is back as it was.
static void Settle() {
    Step& last = sSteps.back();
    if (!Changes(last)) { sBytes -= last.bytes; sSteps.pop_back(); return; }
    if (sSteps.size() < 2) return;
    // The same setting changed again just after: one step, its first "before"
    // kept. (One entry can hold every Mii's value: the same bytes must have
    // changed both times, so two Miis' names stay two steps.)
    Step& prev = sSteps[sSteps.size() - 2];
    if (SameEntries(prev, last)) {
        bool same = true;
        for (const Snap& sn : last.snaps) {
            const Snap* ps = SnapOf(prev, sn.file, sn.hash);
            const SaveEditor::Entry* cur = Current(sn.file, sn.hash);
            size_t plo = 0, phi = 0, llo = 0, lhi = 0;
            const bool pd = ps && Diff(ps->before, sn.before, plo, phi);
            const bool ld = cur && Diff(sn.before, *cur, llo, lhi);
            if (!pd || !ld || plo >= lhi || llo >= phi) { same = false; break; }
        }
        if (same) {
            sBytes -= last.bytes;
            sSteps.pop_back();
            Step& merged = sSteps.back();
            merged.checked = true;
            if (!Changes(merged)) { sBytes -= merged.bytes; sSteps.pop_back(); }
            return;
        }
    }
    // Put back as it was before an earlier step, nothing in between touching
    // it: both steps go (they cancel out).
    const Step* earlier = nullptr;
    size_t at = 0;
    for (size_t k = sSteps.size() - 1; k-- > 0;) {
        bool touches = false;
        for (const Snap& sn : last.snaps) if (SnapOf(sSteps[k], sn.file, sn.hash)) { touches = true; break; }
        if (!touches) continue;
        earlier = &sSteps[k];
        at = k;
        break;
    }
    if (!earlier || !SameEntries(*earlier, last)) return;
    for (const Snap& sn : earlier->snaps) {
        const SaveEditor::Entry* cur = Current(sn.file, sn.hash);
        size_t lo = 0, hi = 0;
        if (!cur || Diff(sn.before, *cur, lo, hi)) return;
    }
    sBytes -= last.bytes;
    sSteps.pop_back();
    sBytes -= sSteps[at].bytes;
    sSteps.erase(sSteps.begin() + (long)at);
}

void Tick() {
    sFrame++;
    if (!sSteps.empty()) {
        Step& st = sSteps.back();
        if (!st.checked && st.frame + 1 < sFrame) {
            st.checked = true;
            Settle();
        }
    }
}
bool CanUndo() { return !sSteps.empty(); }
int  Steps() { return (int)sSteps.size(); }

unsigned Undo() {
    if (sSteps.empty()) return 0;
    Step st = std::move(sSteps.back());
    sSteps.pop_back();
    sBytes -= st.bytes;
    unsigned touched = 0;
    sMuted = true;
    for (auto it = st.snaps.rbegin(); it != st.snaps.rend(); ++it) {
        SaveEditor::SavFile* s = sWatched[it->file].s;
        for (auto& e : s->entries)
            if (e.hash == it->hash) { e = it->before; break; }
        touched |= 1u << it->file;
    }
    sMuted = false;
    for (size_t i = 0; i < sWatched.size() && i < st.wasDirty.size(); i++)
        if ((touched & (1u << i)) && sWatched[i].dirty) *sWatched[i].dirty = st.wasDirty[i] != 0;
    return touched;
}

void Clear() {
    sSteps.clear();
    sBytes = 0;
}

}  // namespace History
