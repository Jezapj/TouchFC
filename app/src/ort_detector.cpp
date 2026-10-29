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
//   4. Run touchfc.exe --model path\to\model.onnx --replay clip.mp4, or list it
//      in config/detectors.json and pick it with --detector or the Main tab
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
#  include "touchfc/detr.hpp"
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

// Head layouts, told apart by the outputs:
//   Yolo    one [1, 4+nc, anchors] (anchors > channels), NMS after decode
//   Joint   one [1, Q, 4+nc]: Ultralytics RT-DETR, cxcywh in [0,1], probabilities
//   Split   [1, Q, 4] boxes + [1, Q, C] scores: RF-DETR, cxcywh in [0,1], logits
//   Sparse  labels [1, Q] int64, boxes [1, Q, 4], scores [1, Q]: D-FINE, DEIM
//           and RT-DETR deploy exports; a second int64 input takes the size the
//           boxes are scaled to, which we set to the model input (xyxy pixels)
enum class Head : std::uint8_t { Yolo, Joint, Split, Sparse };

class OrtDetector final : public Detector {
 public:
  explicit OrtDetector(DetectorPreset preset) : preset_(std::move(preset)) {}

  bool prepare(std::string& error) override {
    try {
      env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "touchfc");
      Ort::SessionOptions opts;
      opts.SetIntraOpNumThreads(2);
      opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
      // Convert UTF-8 to wide for Windows session constructor.
      std::wstring wpath(preset_.file.begin(), preset_.file.end());
      session_ = std::make_unique<Ort::Session>(*env_, wpath.c_str(), opts);
      Ort::AllocatorWithDefaultOptions alloc;
      for (std::size_t i = 0; i < session_->GetInputCount() && i < 2; ++i) {
        auto info = session_->GetInputTypeInfo(i).GetTensorTypeAndShapeInfo();
        std::string name = session_->GetInputNameAllocated(i, alloc).get();
        if (info.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64) {
          size_name_ = std::move(name);
          continue;
        }
        const auto shape = info.GetShape();
        if (shape.size() != 4 || shape[1] != 3) {
          error = "model input is not NCHW with 3 channels";
          return false;
        }
        if (shape[2] > 0 && shape[3] > 0 && shape[2] != shape[3]) {
          error = "model input is not square";
          return false;
        }
        side_ = shape[2] > 0 ? static_cast<std::uint32_t>(shape[2]) : 640;
        input_name_ = std::move(name);
      }
      if (input_name_.empty()) {
        error = "model has no image input";
        return false;
      }
      input_tensor_.assign(3ull * side_ * side_, 0.f);
      sizes_[0] = side_;
      sizes_[1] = side_;
      if (!pick_head(alloc, error)) return false;
      std::uint32_t map_classes = num_classes_;
      if (head_ == Head::Sparse && preset_.classes.empty()) {
        map_classes = kMaxModelClasses;
        log_line("detector: no class list for " + preset_.file + "; assuming COCO labels");
      }
      map_n_ = build_class_map(preset_, map_classes, class_map_);
      static constexpr const char* kHead[] = {"yolo", "detr joint", "detr split", "detr sparse"};
      log_line("ORT ready: " + preset_.file + " input=" + std::to_string(side_) + " head=" +
               kHead[static_cast<int>(head_)] + " queries=" + std::to_string(num_queries_) +
               " classes=" + std::to_string(num_classes_) + (preset_.stretch ? " stretch" : "") +
               (preset_.imagenet ? " imagenet" : ""));
      return true;
    } catch (const Ort::Exception& ex) {
      error = ex.what();
      return false;
    }
  }

  void configure(float score_min, float nms_iou) noexcept override {
    score_min_ = score_min;
    nms_iou_ = nms_iou;
  }

  void run(FrameSlot& slot) noexcept override {
    try {
      const LetterboxParams lb = preset_.stretch
                                     ? make_stretch(slot.meta.width, slot.meta.height, side_)
                                     : make_letterbox(slot.meta.width, slot.meta.height, side_);
      bgra_letterbox_to_chw(slot.bgra.get(), slot.meta.width, slot.meta.height, slot.stride,
                            input_tensor_.data(), lb);
      if (preset_.imagenet) normalize_imagenet_chw(input_tensor_.data(), side_);
      Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
      const int64_t in_shape[4] = {1, 3, side_, side_};
      const int64_t size_shape[2] = {1, 2};
      Ort::Value ins[2] = {
          Ort::Value::CreateTensor<float>(mem, input_tensor_.data(), input_tensor_.size(), in_shape, 4),
          Ort::Value::CreateTensor<std::int64_t>(mem, sizes_, 2, size_shape, 2)};
      const char* in_names[2] = {input_name_.c_str(), size_name_.c_str()};
      const std::size_t n_in = size_name_.empty() ? 1 : 2;
      const char* out_names[3] = {out_names_[0].c_str(), out_names_[1].c_str(), out_names_[2].c_str()};
      auto outputs = session_->Run(Ort::RunOptions{nullptr}, in_names, ins, n_in, out_names, n_out_);
      if (outputs.size() != n_out_) {
        slot.det_count = 0;
        return;
      }
      Detection* dets = slot.dets.data();
      const auto cap = static_cast<std::uint32_t>(slot.dets.size());
      std::uint32_t count = 0;
      bool truncated = false;
      switch (head_) {
        case Head::Yolo:
          decode_yolov8(outputs[0].GetTensorData<float>(), num_classes_, num_anchors_, score_min_, lb,
                        dets, cap, count, truncated);
          count = greedy_nms(dets, count, nms_iou_);
          if (count > cap) count = cap;
          count = apply_class_map(dets, count, map(), map_n_);
          break;
        case Head::Joint: {
          const float* d = outputs[0].GetTensorData<float>();
          count = decode_detr_dense(d, 4 + num_classes_, d + 4, 4 + num_classes_, num_queries_,
                                    num_classes_, DetrBox::CxcywhNorm, score_min_, lb, map(), map_n_,
                                    dets, cap, truncated);
          break;
        }
        case Head::Split:
          count = decode_detr_dense(outputs[0].GetTensorData<float>(), 4,
                                    outputs[1].GetTensorData<float>(), num_classes_, num_queries_,
                                    num_classes_, DetrBox::CxcywhNorm, score_min_, lb, map(), map_n_,
                                    dets, cap, truncated);
          break;
        case Head::Sparse:
          count = decode_detr_sparse(outputs[2].GetTensorData<std::int64_t>(),
                                     outputs[0].GetTensorData<float>(),
                                     outputs[1].GetTensorData<float>(), num_queries_,
                                     DetrBox::XyxyPx, score_min_, lb, map(), map_n_, dets, cap,
                                     truncated);
          break;
      }
      slot.det_count = count;
      if (logged_frames_ < 3) {
        ++logged_frames_;
        log_line("ORT detections: " + std::to_string(count) + (truncated ? " (truncated)" : ""));
      }
    } catch (const Ort::Exception&) {
      slot.det_count = 0;
    }
  }

  const char* label() const noexcept override { return preset_.name.c_str(); }

 private:
  const std::int8_t* map() const noexcept { return map_n_ ? class_map_ : nullptr; }

  // Output order for Run: Yolo / Joint [out], Split [boxes, scores], Sparse
  // [boxes, scores, labels].
  bool pick_head(Ort::AllocatorWithDefaultOptions& alloc, std::string& error) {
    struct Out {
      std::string name;
      std::vector<int64_t> shape;
      bool int64;
    };
    std::vector<Out> outs;
    for (std::size_t i = 0; i < session_->GetOutputCount(); ++i) {
      auto info = session_->GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo();
      outs.push_back(Out{session_->GetOutputNameAllocated(i, alloc).get(), info.GetShape(),
                         info.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64});
    }
    auto dim = [](const Out& o, std::size_t k) { return k < o.shape.size() ? o.shape[k] : int64_t{-1}; };
    if (outs.size() == 1) {
      const Out& o = outs[0];
      if (o.shape.size() != 3 || dim(o, 1) <= 4 || dim(o, 2) <= 4) {
        error = "model output is not [1, 4+nc, anchors] or [1, queries, 4+nc]";
        return false;
      }
      if (dim(o, 1) < dim(o, 2)) {
        head_ = Head::Yolo;
        num_classes_ = static_cast<std::uint32_t>(dim(o, 1) - 4);
        num_anchors_ = static_cast<std::uint32_t>(dim(o, 2));
      } else {
        head_ = Head::Joint;
        num_queries_ = static_cast<std::uint32_t>(dim(o, 1));
        num_classes_ = static_cast<std::uint32_t>(dim(o, 2) - 4);
      }
      out_names_[0] = o.name;
      n_out_ = 1;
      return true;
    }
    int boxes = -1, scores = -1, labels = -1;
    for (int i = 0; i < static_cast<int>(outs.size()); ++i) {
      const Out& o = outs[i];
      if (o.int64) {
        if (labels < 0) labels = i;
      } else if (o.shape.size() == 3 && dim(o, 2) == 4 && boxes < 0) {
        boxes = i;
      } else if (scores < 0 && (o.shape.size() == 2 || o.shape.size() == 3)) {
        scores = i;
      }
    }
    if (boxes < 0 || scores < 0 || dim(outs[boxes], 1) <= 0) {
      error = "model outputs are not a known DETR layout (boxes [1,Q,4] plus scores)";
      return false;
    }
    num_queries_ = static_cast<std::uint32_t>(dim(outs[boxes], 1));
    out_names_[0] = outs[boxes].name;
    out_names_[1] = outs[scores].name;
    if (labels >= 0) {
      if (outs[scores].shape.size() != 2 || size_name_.empty()) {
        error = "sparse DETR head needs scores [1,Q] and an orig_target_sizes input";
        return false;
      }
      head_ = Head::Sparse;
      out_names_[2] = outs[labels].name;
      n_out_ = 3;
      num_classes_ = static_cast<std::uint32_t>(preset_.classes.size());
    } else {
      if (outs[scores].shape.size() != 3 || dim(outs[scores], 2) <= 0) {
        error = "split DETR head needs scores [1,Q,C]";
        return false;
      }
      head_ = Head::Split;
      n_out_ = 2;
      num_classes_ = static_cast<std::uint32_t>(dim(outs[scores], 2));
    }
    if (num_classes_ > kMaxModelClasses && head_ == Head::Sparse) num_classes_ = kMaxModelClasses;
    return true;
  }

  DetectorPreset preset_;
  float score_min_ = 0.25f;
  float nms_iou_ = 0.45f;
  std::unique_ptr<Ort::Env> env_;
  std::unique_ptr<Ort::Session> session_;
  std::string input_name_;
  std::string size_name_;
  std::string out_names_[3];
  std::size_t n_out_ = 1;
  Head head_ = Head::Yolo;
  std::uint32_t side_ = 640;
  std::uint32_t num_classes_ = 0;
  std::uint32_t num_anchors_ = 0;
  std::uint32_t num_queries_ = 0;
  std::int64_t sizes_[2] = {640, 640};
  std::int8_t class_map_[kMaxModelClasses]{};
  std::uint32_t map_n_ = 0;
  std::vector<float> input_tensor_;
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

std::unique_ptr<Detector> make_ort_detector(const DetectorPreset& preset, std::string& error) {
  auto d = std::make_unique<OrtDetector>(preset);
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
