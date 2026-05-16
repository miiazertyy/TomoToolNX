#pragma once
// share_client.h — thin libcurl wrapper for TomoShare API calls.
//
// Previously this lived as a static helper inside http_server.cpp. Extracted
// so the on-Switch Island Generator (island_generator_runner.cpp) can issue
// the same HTTPS calls without going through the local HTTP loopback.

#include <cstdint>
#include <string>
#include <vector>

namespace ShareClient {

// GET `url`, follow redirects, write the response body into `out`. Returns
// the HTTP status code on success, or 0 if the request failed at the
// transport layer (in which case `err` is set). 20-second timeout, no peer
// verification (Switch has no system cert store).
long Fetch(const std::string& url,
           std::vector<uint8_t>& out,
           std::string& err);

// Convenience: percent-encode a single query value using curl_easy_escape.
// Returns "" on failure (rare). Callers should use this for any value that
// might contain non-token characters before splicing into a URL.
std::string UrlEscape(const std::string& value);

} // namespace ShareClient
