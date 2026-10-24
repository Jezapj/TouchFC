#pragma once

#include "frame_pool.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace touchfc {

class FrameSource {
 public:
  virtual ~FrameSource() = default;
  virtual bool open(std::string& error) = 0;
  virtual void close() = 0;
  virtual void request_stop() = 0;
  virtual bool is_live() const noexcept = 0;
  // Fills bgra, meta.width/height/stride/pts_ns and, for live capture, t_capture.
  // Returns false on end of stream or stop.
  virtual bool decode_into(FrameSlot& slot) = 0;
  virtual std::uint32_t width() const noexcept = 0;
  virtual std::uint32_t height() const noexcept = 0;
  virtual std::uint64_t capture_drops() const noexcept { return 0; }
};

std::unique_ptr<FrameSource> make_selftest_source();
std::unique_ptr<FrameSource> make_replay_source(const std::string& path, std::string& error);
std::unique_ptr<FrameSource> make_wgc_source(void* hwnd, std::string& error);

}  // namespace touchfc
