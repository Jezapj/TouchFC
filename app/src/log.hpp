#pragma once

#include "win_prelude.hpp"

#include <cstdio>
#include <mutex>
#include <string>

namespace touchfc {

inline void log_line(const std::string& text) {
  static std::mutex mu;
  std::lock_guard lock(mu);
  std::fputs(text.c_str(), stderr);
  std::fputc('\n', stderr);
  std::fflush(stderr);
  OutputDebugStringA(text.c_str());
  OutputDebugStringA("\n");
}

}  // namespace touchfc
