// share_client.cpp — see share_client.h for what / why.
//
// Implementation is a 1:1 copy of the former static ShareCurlFetch in
// http_server.cpp. Behaviour preserved exactly (same UA, same timeout, same
// VERIFYPEER=0) so the WebUI proxy and the on-app Island Generator hit
// TomoShare identically.

#include "share_client.h"
#include <curl/curl.h>

namespace {

size_t WriteAppend(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::vector<uint8_t>*>(userdata);
    size_t n = size * nmemb;
    out->insert(out->end(), ptr, ptr + n);
    return n;
}

} // namespace

namespace ShareClient {

long Fetch(const std::string& url,
           std::vector<uint8_t>& out,
           std::string& err) {
    CURL* c = curl_easy_init();
    if (!c) { err = "curl_easy_init failed"; return 0; }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "TomoToolNX/share-proxy");
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L); // Switch has no cert store
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, WriteAppend);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    CURLcode res = curl_easy_perform(c);
    long status = 0;
    if (res != CURLE_OK) {
        err = std::string("curl: ") + curl_easy_strerror(res);
    } else {
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    }
    curl_easy_cleanup(c);
    return status;
}

std::string UrlEscape(const std::string& value) {
    CURL* c = curl_easy_init();
    if (!c) return "";
    char* enc = curl_easy_escape(c, value.c_str(), (int)value.size());
    std::string out = enc ? std::string(enc) : std::string();
    if (enc) curl_free(enc);
    curl_easy_cleanup(c);
    return out;
}

} // namespace ShareClient
