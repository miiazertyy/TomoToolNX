#pragma once
// Minimal QR Code SVG generator
// Supports byte mode, version 1-10, error correction M
// Sufficient for URLs like http://192.168.x.x:8080
// Pure C++17, no dependencies
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>

namespace QrSvg {

// Returns SVG string for the given URL, or empty on failure
std::string Generate(const std::string& url, int moduleSize=8, int border=4);

} // namespace QrSvg
