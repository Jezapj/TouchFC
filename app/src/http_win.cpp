#include "http_win.hpp"

#include "win_prelude.hpp"

#include <winhttp.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace touchfc {

namespace {

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

struct Handle {
  HINTERNET h = nullptr;
  ~Handle() {
    if (h) WinHttpCloseHandle(h);
  }
};

}  // namespace

bool http_get(const std::string& url, std::string& body, std::string& error) {
  body.clear();
  const std::wstring wurl = widen(url);
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);
  wchar_t host[256]{};
  wchar_t path[2048]{};
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 2048;
  wchar_t extra[1024]{};
  uc.lpszExtraInfo = extra;
  uc.dwExtraInfoLength = 1024;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
    error = "bad url";
    return false;
  }
  const std::wstring object = std::wstring(path) + extra;
  Handle session{WinHttpOpen(L"TouchFC/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!session.h) {
    error = "WinHttpOpen failed";
    return false;
  }
  WinHttpSetTimeouts(session.h, 5000, 5000, 10000, 15000);
  Handle conn{WinHttpConnect(session.h, host, uc.nPort, 0)};
  const DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
  Handle req{conn.h ? WinHttpOpenRequest(conn.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, flags)
                    : nullptr};
  if (!req.h || !WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(req.h, nullptr)) {
    error = "request failed (" + std::to_string(GetLastError()) + ")";
    return false;
  }
  DWORD status = 0, size = sizeof(status);
  WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                      &status, &size, WINHTTP_NO_HEADER_INDEX);
  for (;;) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(req.h, &avail) || avail == 0) break;
    const std::size_t at = body.size();
    body.resize(at + avail);
    DWORD got = 0;
    if (!WinHttpReadData(req.h, body.data() + at, avail, &got)) break;
    body.resize(at + got);
  }
  if (status != 200) {
    error = "HTTP " + std::to_string(status);
    return false;
  }
  return true;
}

bool cached_get(const std::string& url, const std::string& key, int max_age_s, std::string& body,
                std::string& error) {
  namespace fs = std::filesystem;
  const fs::path file = fs::path("cache") / (key + ".json");
  std::error_code ec;
  bool have_cache = fs::exists(file, ec);
  auto read_cache = [&] {
    std::ifstream in(file, std::ios::binary);
    body.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !body.empty();
  };
  if (have_cache) {
    const auto age = fs::file_time_type::clock::now() - fs::last_write_time(file, ec);
    if (max_age_s < 0 || std::chrono::duration_cast<std::chrono::seconds>(age).count() < max_age_s)
      if (read_cache()) return true;
  }
  if (http_get(url, body, error)) {
    fs::create_directories(file.parent_path(), ec);
    std::ofstream(file, std::ios::binary) << body;
    return true;
  }
  return have_cache && read_cache();
}

}  // namespace touchfc
