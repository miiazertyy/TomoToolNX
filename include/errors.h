#pragma once
// errors.h — TomoToolNX
// What the console's error numbers mean, in words.
//
// A Horizon result packs a module (bits 0-8) and a description (bits 9-21);
// the console shows them as "2MMM-DDDD" (module + 2000, description). The bits
// above 21 can carry noise, so only those two parts are compared: the
// "fsdevMountSaveData failed: 0xCE47D402" users saw is 2002-1002, "save data
// not found", the same as 0x0007D402.

#include <cstdint>
#include <string>

namespace Errors {

struct Info {
    std::string code;      // "2002-1002", empty when the text has no result code
    std::string titleKey;  // language keys; empty when the code is not one we know
    std::string bodyKey;
};

// The first "0x…" result code in `text`, if any.
bool FindResult(const std::string& text, uint32_t& rc);
// "2002-1002"
std::string CodeOf(uint32_t rc);
// What a result (or a message carrying one) means. Fills `code` whenever a
// result is found; the keys only for results we can explain.
Info Describe(const std::string& text);
// Save data for this title and user does not exist (2002-1002).
bool IsSaveNotFound(const std::string& text);

}  // namespace Errors
