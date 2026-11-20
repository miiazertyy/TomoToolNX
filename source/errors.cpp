// errors.cpp — TomoToolNX
// See errors.h. The ranges follow the file system's own result list (as
// documented by Atmosphère and switchbrew): only what a user of this app can
// meet and do something about is explained; anything else still shows its
// code, so a report on GitHub says exactly what happened.
#include "errors.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace Errors {
namespace {
constexpr uint32_t ModuleOf(uint32_t rc) { return rc & 0x1FF; }
constexpr uint32_t DescOf(uint32_t rc)   { return (rc >> 9) & 0x1FFF; }

struct Range { uint32_t module, lo, hi; const char* key; };
// Most specific first.
const Range kKnown[] = {
    { 2, 1002, 1002, "err.fs.save_missing" },   // TargetNotFound: no save data for this title and user
    { 2,    1,    1, "err.fs.path_missing" },   // PathNotFound
    { 2,    2,    2, "err.fs.path_exists" },    // PathAlreadyExists
    { 2,    7,    7, "err.fs.busy" },           // TargetLocked: open elsewhere
    { 2,   30,   45, "err.fs.no_space" },       // UsableSpaceNotEnough (all storages)
    { 2, 2000, 2499, "err.fs.sd" },             // SdCardAccessFailed (2001: no SD card)
    { 2, 3200, 3499, "err.fs.memory" },         // AllocationMemoryFailed
    { 2, 4000, 4999, "err.fs.corrupted" },      // DataCorrupted
    { 2, 6400, 6449, "err.fs.denied" },         // PermissionDenied
    { 2, 6000, 6499, "err.fs.refused" },        // PreconditionViolation (invalid argument, path, …)
};
}  // namespace

bool FindResult(const std::string& text, uint32_t& rc) {
    for (size_t i = 0; i + 2 < text.size(); i++) {
        if (text[i] != '0' || (text[i + 1] != 'x' && text[i + 1] != 'X')) continue;
        if (i > 0 && std::isalnum((unsigned char)text[i - 1])) continue;
        // Only a number that follows "failed" (how every call that returns a
        // result reports it): other hex in messages is a hash, not a result.
        {
            const size_t from = i > 16 ? i - 16 : 0;
            std::string before = text.substr(from, i - from);
            for (auto& c : before) c = (char)std::tolower((unsigned char)c);
            if (before.find("failed") == std::string::npos) continue;
        }
        size_t j = i + 2;
        while (j < text.size() && std::isxdigit((unsigned char)text[j])) j++;
        if (j - (i + 2) < 4 || j - (i + 2) > 8) continue;
        rc = (uint32_t)std::strtoul(text.substr(i + 2, j - (i + 2)).c_str(), nullptr, 16);
        if (rc == 0) continue;
        return true;
    }
    return false;
}

std::string CodeOf(uint32_t rc) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04u-%04u", (unsigned)(2000 + ModuleOf(rc)), (unsigned)DescOf(rc));
    return buf;
}

Info Describe(const std::string& text) {
    Info out;
    uint32_t rc = 0;
    if (!FindResult(text, rc)) return out;
    out.code = CodeOf(rc);
    const uint32_t m = ModuleOf(rc), d = DescOf(rc);
    for (const Range& r : kKnown) {
        if (m == r.module && d >= r.lo && d <= r.hi) {
            out.titleKey = std::string(r.key) + ".title";
            out.bodyKey = std::string(r.key) + ".body";
            break;
        }
    }
    return out;
}

bool IsSaveNotFound(const std::string& text) {
    uint32_t rc = 0;
    return FindResult(text, rc) && ModuleOf(rc) == 2 && DescOf(rc) == 1002;
}

}  // namespace Errors
