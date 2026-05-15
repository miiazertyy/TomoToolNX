#include "i18n.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

// ── Registered locales ──────────────────────────────────────────────────────
// To add a new language: drop a `romfs/lang/<code>.txt` file in the source
// tree and add an entry below. See include/i18n.h for the full contributor
// flow.
const std::vector<Lang::Entry> kAvailable = {
    {"en", "English"},
    {"fr", "Français"},
    {"es", "Español"},
    {"de", "Deutsch"},
    {"ru", "Русский"},
    {"zh", "简体中文"},
    // {"it", "Italiano"},
    // {"pt", "Português"},
    // {"ja", "日本語"},
    // ...add yours here.
};

// strings[code][key] = translation
std::map<std::string, std::map<std::string, std::string>> g_strings;
std::string g_current = "en";

void Trim(std::string& s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) b++;
    size_t e = s.size();
    while (e > b && (s[e-1] == ' ' || s[e-1] == '\t' || s[e-1] == '\r' || s[e-1] == '\n')) e--;
    s = s.substr(b, e - b);
}

void LoadFile(const std::string& code) {
    std::string path = "romfs:/lang/" + code + ".txt";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return;
    auto& bucket = g_strings[code];
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        Trim(s);
        if (s.empty() || s[0] == '#') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = s.substr(0, eq);
        std::string val = s.substr(eq + 1);
        Trim(key);
        Trim(val);
        if (key.empty()) continue;
        bucket[key] = val;
    }
    fclose(f);
}

} // namespace

namespace Lang {

void Init() {
    g_strings.clear();
    for (const auto& e : kAvailable) {
        LoadFile(e.code);
    }
    // Default to English if the file failed to load or no locale was saved.
    if (g_strings.find("en") == g_strings.end()) g_strings["en"] = {};
    if (g_strings.find(g_current) == g_strings.end()) g_current = "en";
}

void SetCurrent(const std::string& code) {
    for (const auto& e : kAvailable) {
        if (e.code == code) {
            g_current = code;
            return;
        }
    }
}

const std::string& Current() {
    return g_current;
}

const std::vector<Entry>& Available() {
    return kAvailable;
}

std::string T(const std::string& key) {
    auto cit = g_strings.find(g_current);
    if (cit != g_strings.end()) {
        auto kit = cit->second.find(key);
        if (kit != cit->second.end() && !kit->second.empty()) return kit->second;
    }
    auto en = g_strings.find("en");
    if (en != g_strings.end()) {
        auto kit = en->second.find(key);
        if (kit != en->second.end() && !kit->second.empty()) return kit->second;
    }
    return key;
}

} // namespace Lang
