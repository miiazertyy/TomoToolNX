// image_search.cpp — TomoToolNX. See image_search.h.

#include "image_search.h"
#include "share_client.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace ImageSearch {
namespace {

const char* kAgent = "TomoToolNX/2.0 (Nintendo Switch homebrew)";
thread_local std::string sNote;   // each thread its own (the console's sheet and the WebUI may search at once)

// ── A small JSON reader ──
struct J {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    double n = 0;
    bool b = false;
    std::string s;
    std::vector<J> a;
    std::vector<std::pair<std::string, J>> o;
    const J* Get(const char* k) const { for (auto& kv : o) if (kv.first == k) return &kv.second; return nullptr; }
    const J& operator[](const char* k) const { static const J none; const J* v = Get(k); return v ? *v : none; }
    std::string Str_() const { return t == Str ? s : std::string(); }
    double Num_() const { return t == Num ? n : 0; }
};
struct Parser {
    const char* p;
    const char* e;
    int depth = 0;
    void Ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
    static void Utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }
    bool String(std::string& out) {
        if (p >= e || *p != '"') return false;
        p++;
        while (p < e && *p != '"') {
            if (*p != '\\') { out += *p++; continue; }
            if (++p >= e) return false;
            const char c = *p++;
            switch (c) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': break;
                case 'b': case 'f': break;
                case 'u': {
                    auto hex4 = [&](unsigned& v) {
                        if (e - p < 4) return false;
                        v = (unsigned)strtoul(std::string(p, 4).c_str(), nullptr, 16);
                        p += 4;
                        return true;
                    };
                    unsigned cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                        p += 2;
                        unsigned lo = 0;
                        if (!hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    Utf8(out, cp);
                    break;
                }
                default: out += c; break;
            }
        }
        if (p >= e) return false;
        p++;
        return true;
    }
    bool Value(J& v) {
        Ws();
        if (p >= e || ++depth > 64) return false;
        bool ok = true;
        if (*p == '{') {
            v.t = J::Obj; p++; Ws();
            if (p < e && *p == '}') p++;
            else for (;;) {
                Ws();
                std::string k;
                if (!String(k)) { ok = false; break; }
                Ws();
                if (p >= e || *p != ':') { ok = false; break; }
                p++;
                v.o.emplace_back(std::move(k), J());
                if (!Value(v.o.back().second)) { ok = false; break; }
                Ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == '}') { p++; break; }
                ok = false; break;
            }
        } else if (*p == '[') {
            v.t = J::Arr; p++; Ws();
            if (p < e && *p == ']') p++;
            else for (;;) {
                v.a.emplace_back();
                if (!Value(v.a.back())) { ok = false; break; }
                Ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == ']') { p++; break; }
                ok = false; break;
            }
        } else if (*p == '"') {
            v.t = J::Str; ok = String(v.s);
        } else if (!strncmp(p, "true", std::min<size_t>(4, e - p)) && e - p >= 4) { v.t = J::Bool; v.b = true; p += 4; }
        else if (!strncmp(p, "false", std::min<size_t>(5, e - p)) && e - p >= 5) { v.t = J::Bool; p += 5; }
        else if (!strncmp(p, "null", std::min<size_t>(4, e - p)) && e - p >= 4) { p += 4; }
        else {
            char* end = nullptr;
            const std::string num(p, std::min<size_t>(32, e - p));
            v.t = J::Num; v.n = strtod(num.c_str(), &end);
            if (end == num.c_str()) ok = false; else p += end - num.c_str();
        }
        depth--;
        return ok;
    }
};
bool Parse(const std::vector<uint8_t>& body, J& out) {
    if (body.empty()) return false;
    Parser ps{ (const char*)body.data(), (const char*)body.data() + body.size() };
    return ps.Value(out);
}

// What a credit or title looks like once its HTML is gone.
std::string Plain(const std::string& html) {
    std::string out;
    bool tag = false;
    for (size_t i = 0; i < html.size(); i++) {
        const char c = html[i];
        if (c == '<') { tag = true; continue; }
        if (c == '>') { tag = false; continue; }
        if (tag) continue;
        if (c == '&') {
            const size_t semi = html.find(';', i);
            if (semi != std::string::npos && semi - i < 8) {
                const std::string ent = html.substr(i + 1, semi - i - 1);
                const char* rep = ent == "amp" ? "&" : ent == "lt" ? "<" : ent == "gt" ? ">" : ent == "quot" ? "\"" : ent == "#039" || ent == "apos" ? "'" : ent == "nbsp" ? " " : nullptr;
                if (rep) { out += rep; i = semi; continue; }
            }
        }
        out += (c == '\n' || c == '\t') ? ' ' : c;
    }
    // Spaces squeezed, the ends trimmed, a long credit shortened.
    std::string sq;
    for (char c : out) { if (c == ' ' && (sq.empty() || sq.back() == ' ')) continue; sq += c; }
    while (!sq.empty() && sq.back() == ' ') sq.pop_back();
    if (sq.size() > 80) { size_t cut = 77; while (cut > 0 && ((unsigned char)sq[cut] & 0xC0) == 0x80) cut--; sq = sq.substr(0, cut) + "..."; }
    return sq;
}

// ── Wikimedia Commons ──
std::string Commons(const std::string& q, int page, std::vector<Hit>& out, bool& more) {
    const int per = 10;
    const std::string url = "https://commons.wikimedia.org/w/api.php?action=query&format=json&formatversion=2&generator=search"
                            "&gsrsearch=" + ShareClient::UrlEscape(q + " filetype:bitmap") + "&gsrnamespace=6&gsrlimit=" + std::to_string(per) +
                            "&gsroffset=" + std::to_string((page - 1) * per) +
                            "&prop=imageinfo&iiprop=url|size|mime|extmetadata&iiurlwidth=320&iiextmetadatafilter=LicenseShortName|Artist|ObjectName";
    std::vector<uint8_t> body;
    std::string err;
    const long st = ShareClient::FetchEx(url, body, err, 15, kAgent);
    sNote = "commons " + std::to_string(st) + (err.empty() ? std::string() : " (" + err + ")") + ", " + std::to_string(body.size()) + " bytes";
    if (st != 200) return "find.offline";
    J root;
    if (!Parse(body, root)) { sNote += ", unreadable"; return "find.offline"; }
    more = root.Get("continue") != nullptr;
    const J& pages = root["query"]["pages"];
    std::vector<std::pair<int, Hit>> ranked;
    for (const J& pg : pages.a) {
        const J* ii = pg["imageinfo"].a.empty() ? nullptr : &pg["imageinfo"].a[0];
        if (!ii) continue;
        const std::string mime = (*ii)["mime"].Str_();
        const std::string thumb = (*ii)["thumburl"].Str_();
        if (thumb.empty()) continue;
        Hit h;
        h.thumb = thumb;
        // The download: the 960-pixel copy, or the file itself when smaller.
        const J& resp = (*ii)["responsiveUrls"];
        const std::string big = resp["2"].Str_();
        const bool plainFile = mime == "image/jpeg" || mime == "image/png" || mime == "image/webp" || mime == "image/gif";
        if ((*ii)["width"].Num_() > 960 && !big.empty()) h.url = big;
        else if (plainFile) h.url = (*ii)["url"].Str_();
        else h.url = big.empty() ? thumb : big;
        const J& em = (*ii)["extmetadata"];
        h.license = Plain(em["LicenseShortName"]["value"].Str_());
        h.creator = Plain(em["Artist"]["value"].Str_());
        h.title = Plain(em["ObjectName"]["value"].Str_());
        if (h.title.empty()) {
            std::string t = pg["title"].Str_();
            if (t.compare(0, 5, "File:") == 0) t = t.substr(5);
            const size_t dot = t.find_last_of('.');
            if (dot != std::string::npos) t = t.substr(0, dot);
            h.title = t;
        }
        if (h.url.compare(0, 8, "https://") != 0) continue;
        ranked.push_back({ (int)pg["index"].Num_(), std::move(h) });
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& r : ranked) out.push_back(std::move(r.second));
    sNote += ", " + std::to_string(out.size()) + " found";
    return out.empty() ? "find.none" : "";
}

// ── Openverse ──
std::string OpenverseLicense(const std::string& lic, const std::string& ver) {
    std::string u = lic;
    for (char& c : u) c = (char)toupper((unsigned char)c);
    if (u == "CC0") return "CC0";
    if (u == "PDM") return "Public domain";
    return "CC " + u + (ver.empty() ? std::string() : " " + ver);
}
std::string Openverse(const std::string& q, int page, std::vector<Hit>& out, bool& more) {
    const std::string url = "https://api.openverse.org/v1/images/?q=" + ShareClient::UrlEscape(q) + "&page=" + std::to_string(page) +
                            "&page_size=10&mature=false&filter_dead=false";
    std::vector<uint8_t> body;
    std::string err;
    long st = 0;
    // It is slow on a word it has not seen yet, and may give up once (504);
    // a second try usually finds it ready.
    for (int attempt = 0; attempt < 2 && st != 200; attempt++) {
        body.clear();
        st = ShareClient::FetchEx(url, body, err, 30, kAgent);
    }
    sNote += "; openverse " + std::to_string(st) + (err.empty() ? std::string() : " (" + err + ")");
    if (st != 200) return "find.offline";
    J root;
    if (!Parse(body, root)) { sNote += ", unreadable"; return "find.offline"; }
    more = page < (int)root["page_count"].Num_();
    for (const J& r : root["results"].a) {
        Hit h;
        h.title = Plain(r["title"].Str_());
        h.creator = Plain(r["creator"].Str_());
        h.license = OpenverseLicense(r["license"].Str_(), r["license_version"].Str_());
        h.url = r["url"].Str_();
        h.thumb = r["thumbnail"].Str_();
        if (h.url.compare(0, 8, "https://") != 0) continue;
        out.push_back(std::move(h));
    }
    return out.empty() ? "find.none" : "";
}

}  // namespace

std::string LastNote() { return sNote; }

std::string Search(const std::string& query, int page, std::vector<Hit>& out, bool& more) {
    out.clear();
    more = false;
    if (query.empty()) return "find.none";
    page = std::max(1, page);
    const std::string a = Commons(query, page, out, more);
    if (a.empty()) return a;
    // Commons found nothing, or did not answer: Openverse.
    std::vector<Hit> ov;
    bool ovMore = false;
    const std::string b = Openverse(query, page, ov, ovMore);
    if (b.empty()) { out = std::move(ov); more = ovMore; return b; }
    return (a == "find.none" || b == "find.none") ? "find.none" : "find.offline";
}

}  // namespace ImageSearch
