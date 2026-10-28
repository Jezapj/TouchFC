#pragma once

#include <string>

namespace touchfc {

// Blocking HTTPS GET with WinHTTP. T7 only.
bool http_get(const std::string& url, std::string& body, std::string& error);

// GET through cache/<key>.json. A cached copy younger than max_age_s (or any
// age when max_age_s < 0) is used without the network; a failed fetch falls
// back to a stale copy. A replay of a past match costs no requests.
bool cached_get(const std::string& url, const std::string& key, int max_age_s, std::string& body,
                std::string& error);

}  // namespace touchfc
