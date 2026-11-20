#pragma once
// image_search.h — TomoToolNX
// Pictures people share freely, found by a word: Wikimedia Commons first (it
// answers in a second), then Openverse (slower: it is given longer and asked
// twice). The results are the same either way, so the console's sheet and the
// WebUI show them alike. Runs on whichever thread calls it (it waits on the
// network).

#include <string>
#include <vector>

namespace ImageSearch {

struct Hit {
    std::string title, creator, license;   // licence as it reads ("CC BY-SA 3.0", "Public domain")
    std::string url;                       // the picture, at most about 1000 pixels wide
    std::string thumb;                     // a small one for the grid
};
// One page (from 1) of pictures for `query`. `more`: there is a next page.
// "" when it worked; otherwise a language key: "find.offline" (no library
// answered) or "find.none" (nothing found).
std::string Search(const std::string& query, int page, std::vector<Hit>& out, bool& more);
// What the last search did, for the logs ("commons 200, 10 found" ...).
std::string LastNote();

}  // namespace ImageSearch
