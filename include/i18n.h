// Lightweight i18n for the on-Switch native UI.
//
// HOW TO ADD A NEW LANGUAGE (contributors welcome!):
//   1. Copy `romfs/lang/en.txt` to `romfs/lang/<code>.txt` (e.g. `es.txt`).
//   2. Translate every value on the right side of `=`. Missing keys fall back
//      to English, so partial translations are fine.
//   3. Open `source/i18n.cpp` and add an entry to the kAvailable list inside
//      Lang::Init() with your code and display name (e.g.
//      {"es", "Español"}).
//   4. Rebuild the .nro and submit a Pull Request on
//      https://github.com/miiazertyy/TomoToolNX — thanks!
#pragma once
#include <string>
#include <vector>

namespace Lang {

struct Entry {
    std::string code;
    std::string label;
};

// Load `romfs:/lang/<code>.txt` for every entry registered inside Init().
// English is the baseline and is always loaded first; if a non-English file
// is missing keys, those keys fall back to the English value.
void Init();

// Switch to the locale with the given code. Unknown codes are ignored.
void SetCurrent(const std::string& code);

// Currently-active locale code (e.g. "en", "fr").
const std::string& Current();

// All locales registered in Init().
const std::vector<Entry>& Available();

// Returns the translated value for `key`. Falls back to the English value,
// and finally to `key` itself if nothing is registered. Safe to call on every
// frame — lookups are O(log n) in a static map.
std::string T(const std::string& key);

} // namespace Lang
