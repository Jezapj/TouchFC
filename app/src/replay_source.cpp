#include "frame_source.hpp"
#include "log.hpp"
#include "win_time.hpp"

#include <memory>
#include <string>

#if defined(TOUCHFC_HAS_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#endif

namespace touchfc {
namespace {

#if defined(TOUCHFC_HAS_FFMPEG)

class ReplaySource final : public FrameSource {
 public:
  explicit ReplaySource(std::string path) : path_(std::move(path)) {}
  ~ReplaySource() override { close(); }

  bool open(std::string& error) override {
    if (avformat_open_input(&fmt_, path_.c_str(), nullptr, nullptr) < 0) {
      error = "avformat_open_input failed for " + path_;
      return false;
    }
    if (avformat_find_stream_info(fmt_, nullptr) < 0) {
      error = "avformat_find_stream_info failed";
      close();
      return false;
    }
    const int index = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (index < 0) {
      error = "no video stream";
      close();
      return false;
    }
    stream_ = index;
    AVStream* st = fmt_->streams[stream_];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
      error = "no decoder for the video stream";
      close();
      return false;
    }
    dec_ = avcodec_alloc_context3(codec);
    if (!dec_ || avcodec_parameters_to_context(dec_, st->codecpar) < 0 ||
        avcodec_open2(dec_, codec, nullptr) < 0) {
      error = "could not open the decoder";
      close();
      return false;
    }
    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    width_ = static_cast<std::uint32_t>(dec_->width);
    height_ = static_cast<std::uint32_t>(dec_->height);
    if (!frame_ || !packet_ || width_ == 0 || height_ == 0) {
      error = "empty video";
      close();
      return false;
    }
    return true;
  }

  void close() override {
    if (sws_) {
      sws_freeContext(sws_);
      sws_ = nullptr;
    }
    if (frame_) {
      av_frame_free(&frame_);
    }
    if (packet_) {
      av_packet_free(&packet_);
    }
    if (dec_) {
      avcodec_free_context(&dec_);
    }
    if (fmt_) {
      avformat_close_input(&fmt_);
    }
  }

  void request_stop() override { stop_.store(true, std::memory_order_release); }
  bool is_live() const noexcept override { return false; }
  std::uint32_t width() const noexcept override { return width_; }
  std::uint32_t height() const noexcept override { return height_; }

  bool decode_into(FrameSlot& slot) override {
    if (stop_.load(std::memory_order_acquire) || !dec_) return false;
    while (!stop_.load(std::memory_order_acquire)) {
      const int rec = avcodec_receive_frame(dec_, frame_);
      if (rec == 0) return scale_into(slot);
      if (rec != AVERROR(EAGAIN) && rec != AVERROR_EOF) return false;
      if (eof_) return false;
      const int read = av_read_frame(fmt_, packet_);
      if (read < 0) {
        eof_ = true;
        avcodec_send_packet(dec_, nullptr);
        continue;
      }
      if (packet_->stream_index == stream_) {
        if (avcodec_send_packet(dec_, packet_) < 0) {
          av_packet_unref(packet_);
          return false;
        }
      }
      av_packet_unref(packet_);
    }
    return false;
  }

 private:
  bool scale_into(FrameSlot& slot) {
    if (!sws_) {
      sws_ = sws_getContext(frame_->width, frame_->height, static_cast<AVPixelFormat>(frame_->format),
                            static_cast<int>(width_), static_cast<int>(height_), AV_PIX_FMT_BGRA,
                            SWS_BILINEAR, nullptr, nullptr, nullptr);
      if (!sws_) return false;
    }
    std::uint8_t* dst_data[4] = {slot.bgra.get(), nullptr, nullptr, nullptr};
    int dst_lines[4] = {static_cast<int>(slot.stride), 0, 0, 0};
    sws_scale(sws_, frame_->data, frame_->linesize, 0, frame_->height, dst_data, dst_lines);

    std::int64_t pts_us = 0;
    if (frame_->best_effort_timestamp != AV_NOPTS_VALUE) {
      pts_us = av_rescale_q(frame_->best_effort_timestamp, fmt_->streams[stream_]->time_base,
                            AV_TIME_BASE_Q);
    } else {
      pts_us = static_cast<std::int64_t>(index_) * 40000;
    }
    if (!have_anchor_) {
      anchor_wall_ = now_ns();
      anchor_pts_ns_ = pts_us * 1000;
      have_anchor_ = true;
    }
    const TimeNs pts_ns = pts_us * 1000;
    wait_.until(anchor_wall_ + (pts_ns - anchor_pts_ns_), stop_);
    slot.meta.width = width_;
    slot.meta.height = height_;
    slot.meta.pts_ns = pts_ns - anchor_pts_ns_;
    slot.meta.t_capture = 0;
    slot.meta.flags = kFlagReplay;
    ++index_;
    av_frame_unref(frame_);
    return !stop_.load(std::memory_order_acquire);
  }

  std::string path_;
  AVFormatContext* fmt_ = nullptr;
  AVCodecContext* dec_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* packet_ = nullptr;
  SwsContext* sws_ = nullptr;
  int stream_ = -1;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  std::uint64_t index_ = 0;
  bool eof_ = false;
  bool have_anchor_ = false;
  TimeNs anchor_wall_ = 0;
  TimeNs anchor_pts_ns_ = 0;
  std::atomic<bool> stop_{false};
  PreciseWait wait_{};
};

#endif

}  // namespace

std::unique_ptr<FrameSource> make_replay_source(const std::string& path, std::string& error) {
#if defined(TOUCHFC_HAS_FFMPEG)
  auto source = std::make_unique<ReplaySource>(path);
  if (!source->open(error)) return nullptr;
  return source;
#else
  (void)path;
  error = "FFmpeg is not linked. Run scripts/fetch_ffmpeg.ps1 and reconfigure.";
  log_line(error);
  return nullptr;
#endif
}

}  // namespace touchfc
