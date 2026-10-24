// Opt-in ONNX Runtime integration. Compiled only when TOUCHFC_HAS_ORT is set
// (CMake looks for third_party/onnxruntime/include/onnxruntime_cxx_api.h and
// defines the macro). When the macro is not set, stub_detector.cpp provides
// make_ort_detector that returns a helpful error.
//
// How to turn this on, once:
//   1. Download ONNX Runtime for Windows x64 from
//      https://github.com/microsoft/onnxruntime/releases
//      e.g. onnxruntime-win-x64-1.20.1.zip (CPU EP) or the DirectML build.
//   2. Extract to third_party/onnxruntime/ (so that
//      third_party/onnxruntime/include/onnxruntime_cxx_api.h exists).
//   3. Reconfigure CMake. The build copies onnxruntime.dll next to touchfc.exe.
//   4. Run touchfc.exe --model path\to\yolov8n.onnx --replay clip.mp4
//
// Weights are never committed. scripts/fetch_models.ps1 refuses to download
// them for licensing reasons (YOLOv8 is claimed AGPL by Ultralytics; verify
// before shipping).

#if defined(TOUCHFC_HAS_ORT)

#  include "detector.hpp"
#  include "log.hpp"
#  include "pitch_model.hpp"

#  include "touchfc/pitch_pose.hpp"

#  include "touchfc/detection.hpp"
#  include "touchfc/nms.hpp"
#  include "touchfc/preprocess.hpp"
#  include "touchfc/yolo.hpp"

#  include <onnxruntime_cxx_api.h>

#  include <algorithm>
#  include <cstring>
#  include <memory>
#  include <string>
#  include <vector>

namespace touchfc {
namespace {

class OrtDetector final : public Detector {
 public:
  explicit OrtDetector(std::string path) : model_path_(std::move(path)) {}

  bool prepare(std::string& error) override {
    try {
      env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "touchfc");
      Ort::SessionOptions opts;
      opts.SetIntraOpNumThreads(2);
      opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
      // Convert UTF-8 to wide for Windows session constructor.
      std::wstring wpath(model_path_.begin(), model_path_.end());
      session_ = std::make_unique<Ort::Session>(*env_, wpath.c_str(), opts);
      Ort::AllocatorWithDefaultOptions alloc;
      Ort::AllocatedStringPtr in = session_->GetInputNameAllocated(0, alloc);
      Ort::AllocatedStringPtr out = session_->GetOutputNameAllocated(0, alloc);
      input_name_ = in.get();
      output_name_ = out.get();
      auto in_info = session_->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo();
      auto in_shape = in_info.GetShape();
      if (in_shape.size() != 4 || in_shape[1] != 3) {
        error = "model input is not NCHW with 3 channels";
        return false;
      }
      side_ = in_shape[2] > 0 ? static_cast<std::uint32_t>(in_shape[2]) : 640;
      input_tensor_.assign(3 * side_ * side_, 0.f);
      auto out_info = session_->GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo();
      auto out_shape = out_info.GetShape();
      if (out_shape.size() != 3 || out_shape[1] <= 4) {
        error = "model output is not [1, 4+nc, anchors]";
        return false;
      }
      num_classes_ = static_cast<std::uint32_t>(out_shape[1] - 4);
      num_anchors_ = static_cast<std::uint32_t>(out_shape[2]);
      output_buffer_.assign(static_cast<std::size_t>(out_shape[1]) * out_shape[2], 0.f);
      log_line("ORT ready: " + model_path_ + " input=" + std::to_string(side_) +
               " classes=" + std::to_string(num_classes_));
      return true;
    } catch (const Ort::Exception& ex) {
      error = ex.what();
      return false;
    }
  }

  void run(FrameSlot& slot) noexcept override {
    try {
      const LetterboxParams lb = make_letterbox(slot.meta.width, slot.meta.height, side_);
      bgra_letterbox_to_chw(slot.bgra.get(), slot.meta.width, slot.meta.height, slot.stride,
                            input_tensor_.data(), lb);
      Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
      const int64_t in_shape[4] = {1, 3, side_, side_};
      Ort::Value in = Ort::Value::CreateTensor<float>(mem, input_tensor_.data(),
                                                      input_tensor_.size(), in_shape, 4);
      const char* in_names[] = {input_name_.c_str()};
      const char* out_names[] = {output_name_.c_str()};
      auto outputs = session_->Run(Ort::RunOptions{nullptr}, in_names, &in, 1, out_names, 1);
      if (outputs.empty() || !outputs[0].IsTensor()) {
        slot.det_count = 0;
        return;
      }
      const float* data = outputs[0].GetTensorData<float>();
      std::uint32_t count = 0;
      bool truncated = false;
      decode_yolov8(data, num_classes_, num_anchors_, 0.25f, lb, slot.dets.data(),
                    static_cast<std::uint32_t>(slot.dets.size()), count, truncated);
      count = greedy_nms(slot.dets.data(), count, 0.45f);
      if (count > slot.dets.size()) count = static_cast<std::uint32_t>(slot.dets.size());
      // COCO YOLOv8 is 80 classes and class 0 is "person". A broadcast frame is
      // full of other classes (ball ads, cars, seats). Keep people only so the
      // tracker follows players. A smaller model is a soccer head and keeps
      // every class it was trained with.
      if (num_classes_ >= 80) {
        std::uint32_t kept = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
          if (slot.dets[i].class_id == 0 || slot.dets[i].class_id == 32) {
          if (slot.dets[i].class_id == 32) slot.dets[i].class_id = 3;
          slot.dets[kept++] = slot.dets[i];
        }
        }
        count = kept;
      }
      slot.det_count = count;
      if (logged_frames_ < 3) {
        ++logged_frames_;
        log_line("ORT detections: " + std::to_string(count) +
                 (truncated ? " (truncated)" : ""));
      }
      (void)truncated;
    } catch (const Ort::Exception&) {
      slot.det_count = 0;
    }
  }

  const char* label() const noexcept override { return "ort"; }

 private:
  std::string model_path_;
  std::unique_ptr<Ort::Env> env_;
  std::unique_ptr<Ort::Session> session_;
  std::string input_name_;
  std::string output_name_;
  std::uint32_t side_ = 640;
  std::uint32_t num_classes_ = 0;
  std::uint32_t num_anchors_ = 0;
  std::vector<float> input_tensor_;
  std::vector<float> output_buffer_;
  int logged_frames_ = 0;
};

class OrtPitch final : public PitchModel {
 public:
  explicit OrtPitch(std::string path) : model_path_(std::move(path)) {}

  bool prepare(std::string& error) override {
    try {
      env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "touchfc-pitch");
      Ort::SessionOptions opts;
      opts.SetIntraOpNumThreads(2);
      opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
      std::wstring wpath(model_path_.begin(), model_path_.end());
      session_ = std::make_unique<Ort::Session>(*env_, wpath.c_str(), opts);
      Ort::AllocatorWithDefaultOptions alloc;
      input_name_ = session_->GetInputNameAllocated(0, alloc).get();
      output_name_ = session_->GetOutputNameAllocated(0, alloc).get();
      auto in_shape = session_->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
      if (in_shape.size() != 4 || in_shape[1] != 3) {
        error = "pitch model input is not NCHW with 3 channels";
        return false;
      }
      side_ = in_shape[2] > 0 ? static_cast<std::uint32_t>(in_shape[2]) : 640;
      input_tensor_.assign(3ull * side_ * side_, 0.f);
      auto out_shape = session_->GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
      if (out_shape.size() != 3 || out_shape[1] < 8 || ((out_shape[1] - 5) % 3) != 0) {
        error = "pitch model output is not [1, 5+3K, anchors]";
        return false;
      }
      num_keypoints_ = static_cast<std::uint32_t>((out_shape[1] - 5) / 3);
      num_anchors_ = static_cast<std::uint32_t>(out_shape[2]);
      if (num_keypoints_ > 32) {
        error = "pitch model has more than 32 keypoints";
        return false;
      }
      log_line("pitch model ready: " + model_path_ + " keypoints=" + std::to_string(num_keypoints_));
      return true;
    } catch (const Ort::Exception& ex) {
      error = ex.what();
      return false;
    }
  }

  void run(FrameSlot& slot, float keypoint_conf) noexcept override {
    slot.kp_fresh = 0;
    slot.kp_n = 0;
    try {
      const LetterboxParams lb = make_letterbox(slot.meta.width, slot.meta.height, side_);
      bgra_letterbox_to_chw(slot.bgra.get(), slot.meta.width, slot.meta.height, slot.stride,
                            input_tensor_.data(), lb);
      Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
      const int64_t in_shape[4] = {1, 3, side_, side_};
      Ort::Value in = Ort::Value::CreateTensor<float>(mem, input_tensor_.data(), input_tensor_.size(),
                                                      in_shape, 4);
      const char* in_names[] = {input_name_.c_str()};
      const char* out_names[] = {output_name_.c_str()};
      auto outputs = session_->Run(Ort::RunOptions{nullptr}, in_names, &in, 1, out_names, 1);
      if (outputs.empty() || !outputs[0].IsTensor()) return;
      PitchKeypoint found[32];
      const std::uint32_t n = decode_pitch_pose(outputs[0].GetTensorData<float>(), num_keypoints_,
                                                num_anchors_, 0.25f, keypoint_conf, lb, found, 32);
      slot.kp_n = static_cast<std::uint8_t>(n);
      for (std::uint32_t i = 0; i < n; ++i) {
        slot.kp_x[i] = found[i].x;
        slot.kp_y[i] = found[i].y;
        slot.kp_id[i] = found[i].id;
      }
      slot.kp_fresh = 1;
      if (logged_frames_ < 3) {
        ++logged_frames_;
        log_line("pitch keypoints: " + std::to_string(n));
      }
    } catch (const Ort::Exception&) {
      slot.kp_fresh = 0;
      slot.kp_n = 0;
    }
  }

 private:
  std::string model_path_;
  std::unique_ptr<Ort::Env> env_;
  std::unique_ptr<Ort::Session> session_;
  std::string input_name_;
  std::string output_name_;
  std::uint32_t side_ = 640;
  std::uint32_t num_keypoints_ = 0;
  std::uint32_t num_anchors_ = 0;
  std::vector<float> input_tensor_;
  int logged_frames_ = 0;
};

}  // namespace

std::unique_ptr<Detector> make_ort_detector(const std::string& model_path, std::string& error) {
  auto d = std::make_unique<OrtDetector>(model_path);
  if (!d->prepare(error)) return nullptr;
  return d;
}

std::unique_ptr<PitchModel> make_pitch_model(const std::string& model_path, std::string& error) {
  auto model = std::make_unique<OrtPitch>(model_path);
  if (!model->prepare(error)) return nullptr;
  return model;
}

}  // namespace touchfc

#endif  // TOUCHFC_HAS_ORT
