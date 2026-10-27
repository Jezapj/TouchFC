#pragma once

#include "touchfc/config_fields.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace touchfc {

// Profiles: the whole Config by key. Not for the hot path (allocates); the
// present thread calls these on a button click.
inline std::string config_to_json(const Config& c) {
  nlohmann::ordered_json j;
  for (const CfgField& f : kConfigFields) {
    const double v = cfg_get(c, f);
    if (f.type == CfgType::F32) j[f.key] = static_cast<float>(v);
    else j[f.key] = static_cast<std::uint32_t>(v);
  }
  return j.dump(2);
}

// Unknown keys are ignored and missing ones keep their defaults, so an older
// profile still loads. Visible fields are clamped to their slider range.
inline bool config_from_json(const std::string& text, Config& out) {
  const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return false;
  Config c{};
  for (const CfgField& f : kConfigFields) {
    const auto it = j.find(f.key);
    if (it == j.end() || !it->is_number()) continue;
    double v = it->get<double>();
    if (f.group != CfgGroup::Hidden) v = v < f.lo ? f.lo : v > f.hi ? f.hi : v;
    cfg_set(c, f, v);
  }
  out = c;
  return true;
}

}  // namespace touchfc
