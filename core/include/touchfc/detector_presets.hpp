#pragma once

#include "touchfc/detr.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace touchfc {

// One detector the user can pick: an ONNX file plus how to feed and read it.
// Read from config/detectors.json at startup; not for the hot path.
struct DetectorPreset {
  std::string name;
  std::string file;
  std::string note;
  bool stretch = false;   // plain resize instead of letterbox
  bool imagenet = false;  // mean / std normalised input
  std::vector<std::string> classes;  // model label order; empty: COCO if 80+, else as is
};

// The model's class index -> TouchFC id, -1 to drop. Returns the map length;
// 0 means "use the ids as they are".
inline std::uint32_t build_class_map(const DetectorPreset& p, std::uint32_t num_classes,
                                     std::int8_t* map) noexcept {
  const std::uint32_t n = num_classes < kMaxModelClasses ? num_classes : kMaxModelClasses;
  if (!p.classes.empty()) {
    for (std::uint32_t c = 0; c < n; ++c)
      map[c] = static_cast<std::int8_t>(c < p.classes.size() ? touchfc_class_of(p.classes[c]) : -1);
    return n;
  }
  if (num_classes >= 80) {  // COCO: person and sports ball only
    for (std::uint32_t c = 0; c < n; ++c) map[c] = -1;
    map[0] = 0;
    map[32] = 3;
    return n;
  }
  return 0;
}

// {"detectors": [{"name", "file", "resize": "letterbox"|"stretch",
//   "norm": "unit"|"imagenet", "classes": [...], "note"}]}. Bad entries are
// skipped; a bad file gives an empty list.
inline std::vector<DetectorPreset> parse_detector_presets(const std::string& text) {
  std::vector<DetectorPreset> out;
  const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return out;
  const auto list = j.find("detectors");
  if (list == j.end() || !list->is_array()) return out;
  for (const auto& e : *list) {
    if (!e.is_object() || !e.contains("name") || !e.contains("file")) continue;
    if (!e["name"].is_string() || !e["file"].is_string()) continue;
    DetectorPreset p;
    try {  // value() throws on a wrongly typed key
      p.name = e["name"].get<std::string>();
      p.file = e["file"].get<std::string>();
      p.note = e.value("note", std::string());
      p.stretch = e.value("resize", std::string("letterbox")) == "stretch";
      p.imagenet = e.value("norm", std::string("unit")) == "imagenet";
      if (const auto c = e.find("classes"); c != e.end() && c->is_array())
        for (const auto& s : *c) p.classes.push_back(s.is_string() ? s.get<std::string>() : std::string());
    } catch (const nlohmann::json::exception&) {
      continue;
    }
    out.push_back(std::move(p));
  }
  return out;
}

}  // namespace touchfc
