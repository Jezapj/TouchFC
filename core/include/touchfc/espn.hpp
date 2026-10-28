#pragma once

#include "touchfc/match_info.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace touchfc {

// ESPN's unofficial public JSON (site.api.espn.com/apis/site/v2/sports/soccer/
// <league>/...). Undocumented, so every field is optional: a missing field
// leaves the default. T7 only (allocates). Checked against live responses in
// October 2026: scoreboard (fixture, form, colours, score), summary (rosters
// with formation once lineups are out, boxscore team stats), teams/<id>/roster
// (season stats, injuries).
namespace espn {

inline std::string url_teams(const char* league) {
  return std::string("https://site.api.espn.com/apis/site/v2/sports/soccer/") + league + "/teams";
}
inline std::string url_scoreboard(const char* league, const char* yyyymmdd) {
  return std::string("https://site.api.espn.com/apis/site/v2/sports/soccer/") + league +
         "/scoreboard?dates=" + yyyymmdd;
}
inline std::string url_summary(const char* league, std::uint32_t event) {
  return std::string("https://site.api.espn.com/apis/site/v2/sports/soccer/") + league +
         "/summary?event=" + std::to_string(event);
}
inline std::string url_roster(const char* league, std::uint32_t team) {
  return std::string("https://site.api.espn.com/apis/site/v2/sports/soccer/") + league + "/teams/" +
         std::to_string(team) + "/roster";
}

namespace detail {
using json = nlohmann::json;

inline void copy(char* dst, std::size_t n, const json& j, const char* key) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_string()) return;
  std::snprintf(dst, n, "%s", it->get_ref<const std::string&>().c_str());
}

inline std::uint32_t to_u32(const json& j, const char* key) {
  const auto it = j.find(key);
  if (it == j.end()) return 0;
  if (it->is_string()) return static_cast<std::uint32_t>(std::strtoul(it->get_ref<const std::string&>().c_str(), nullptr, 10));
  if (it->is_number()) return it->get<std::uint32_t>();
  return 0;
}

inline bool hex_color(const json& j, const char* key, std::uint32_t& out) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_string()) return false;
  const std::string& s = it->get_ref<const std::string&>();
  if (s.size() != 6) return false;
  char* end = nullptr;
  out = static_cast<std::uint32_t>(std::strtoul(s.c_str(), &end, 16));
  return end && *end == 0;
}

inline json parse(const std::string& text) { return json::parse(text, nullptr, false); }
}  // namespace detail

struct TeamEntry {
  std::uint32_t id = 0;
  std::string code;
  std::string name;
};

// League team list, for the manual search box and scorebug code mapping.
inline std::vector<TeamEntry> parse_teams(const std::string& text) {
  std::vector<TeamEntry> out;
  const auto j = detail::parse(text);
  if (j.is_discarded()) return out;
  const auto sports = j.find("sports");
  if (sports == j.end() || !sports->is_array() || sports->empty()) return out;
  const auto& leagues = (*sports)[0].value("leagues", detail::json::array());
  if (!leagues.is_array() || leagues.empty()) return out;
  for (const auto& t : leagues[0].value("teams", detail::json::array())) {
    const auto& team = t.value("team", detail::json::object());
    TeamEntry e;
    e.id = detail::to_u32(team, "id");
    e.code = team.value("abbreviation", "");
    e.name = team.value("displayName", "");
    if (e.id) out.push_back(std::move(e));
  }
  return out;
}

// Finds the fixture between two provider team ids in a scoreboard response and
// fills ids, names, codes, colours and form. Home/away follow the provider.
inline bool parse_scoreboard(const std::string& text, std::uint32_t team_a, std::uint32_t team_b,
                             MatchInfo& m) {
  const auto j = detail::parse(text);
  if (j.is_discarded()) return false;
  for (const auto& ev : j.value("events", detail::json::array())) {
    const auto& comps = ev.value("competitions", detail::json::array());
    if (!comps.is_array() || comps.empty()) continue;
    const auto& cs = comps[0].value("competitors", detail::json::array());
    if (!cs.is_array() || cs.size() != 2) continue;
    std::uint32_t ids[2] = {detail::to_u32(cs[0].value("team", detail::json::object()), "id"),
                            detail::to_u32(cs[1].value("team", detail::json::object()), "id")};
    const bool match = (ids[0] == team_a && ids[1] == team_b) || (ids[0] == team_b && ids[1] == team_a);
    if (!match) continue;
    m.event_id = detail::to_u32(ev, "id");
    for (const auto& c : cs) {
      const int side = c.value("homeAway", "") == "away" ? 1 : 0;
      TeamInfo& t = m.team[side];
      const auto& team = c.value("team", detail::json::object());
      t.provider_id = detail::to_u32(team, "id");
      detail::copy(t.name, sizeof(t.name), team, "displayName");
      detail::copy(t.code, sizeof(t.code), team, "abbreviation");
      detail::copy(t.form, sizeof(t.form), c, "form");
      std::uint32_t col = 0;
      if (detail::hex_color(team, "color", col)) {
        t.color = col;
        t.has_color = 1;
      }
      if (detail::hex_color(team, "alternateColor", col)) t.alt_color = col;
    }
    m.teams_ok = 1;
    return true;
  }
  return false;
}

// Lineups (rosters with starters, once posted) and post-match team stats.
inline bool parse_summary(const std::string& text, MatchInfo& m) {
  const auto j = detail::parse(text);
  if (j.is_discarded()) return false;
  bool lineup = false;
  for (const auto& r : j.value("rosters", detail::json::array())) {
    const int side = r.value("homeAway", "") == "away" ? 1 : 0;
    TeamInfo& t = m.team[side];
    detail::copy(t.formation, sizeof(t.formation), r, "formation");
    int n = 0;
    for (const auto& p : r.value("roster", detail::json::array())) {
      if (n >= kSquadMax) break;
      PlayerInfo& pi = t.squad[n++];
      pi = {};
      detail::copy(pi.name, sizeof(pi.name), p.value("athlete", detail::json::object()), "displayName");
      detail::copy(pi.pos, sizeof(pi.pos), p.value("position", detail::json::object()), "abbreviation");
      pi.jersey = static_cast<std::uint8_t>(detail::to_u32(p, "jersey"));
      pi.starter = p.value("starter", false) ? 1 : 0;
      lineup = lineup || pi.starter;
    }
    if (n > 0) t.n = static_cast<std::uint8_t>(n);
  }
  if (lineup) m.lineup_ok = 1;
  const auto& box = j.value("boxscore", detail::json::object());
  int k = 0;
  for (const auto& bt : box.value("teams", detail::json::array())) {
    if (k > 1) break;
    const std::uint32_t id = detail::to_u32(bt.value("team", detail::json::object()), "id");
    const int side = id && id == m.team[1].provider_id ? 1 : id && id == m.team[0].provider_id ? 0 : k;
    ++k;
    ProviderStats& ps = m.team[side].post;
    for (const auto& s : bt.value("statistics", detail::json::array())) {
      const std::string name = s.value("name", "");
      const std::string v = s.value("displayValue", "");
      if (v.empty()) continue;
      if (name == "possessionPct") ps.possession_pct = std::strtof(v.c_str(), nullptr);
      else if (name == "totalPasses") ps.passes = std::atoi(v.c_str());
      else if (name == "accuratePasses") ps.passes_ok = std::atoi(v.c_str());
      else if (name == "totalShots") ps.shots = std::atoi(v.c_str());
    }
  }
  return true;
}

// Season stats and availability from a team roster. Fills the squad when the
// lineup is not out yet; otherwise adds stats to the lineup players by name.
inline bool parse_roster(const std::string& text, TeamInfo& t) {
  const auto j = detail::parse(text);
  if (j.is_discarded()) return false;
  const bool have_lineup = t.n > 0;
  for (const auto& a : j.value("athletes", detail::json::array())) {
    PlayerInfo tmp{};
    detail::copy(tmp.name, sizeof(tmp.name), a, "displayName");
    detail::copy(tmp.pos, sizeof(tmp.pos), a.value("position", detail::json::object()), "abbreviation");
    tmp.jersey = static_cast<std::uint8_t>(detail::to_u32(a, "jersey"));
    const auto& inj = a.value("injuries", detail::json::array());
    const auto& st = a.value("status", detail::json::object());
    tmp.out = (inj.is_array() && !inj.empty()) || (st.is_object() && st.value("type", "active") != "active") ? 1 : 0;
    const auto& splits = a.value("statistics", detail::json::object()).value("splits", detail::json::object());
    for (const auto& cat : splits.value("categories", detail::json::array())) {
      for (const auto& s : cat.value("stats", detail::json::array())) {
        const std::string ab = s.value("abbreviation", "");
        const auto v = static_cast<std::uint16_t>(s.value("value", 0.0));
        if (ab == "APP") tmp.apps = v;
        else if (ab == "G") tmp.goals = v;
        else if (ab == "A") tmp.assists = v;
        else if (ab == "SHOT") tmp.shots = v;
      }
    }
    PlayerInfo* dst = nullptr;
    if (have_lineup) {
      for (int i = 0; i < t.n; ++i)
        if (std::strcmp(t.squad[i].name, tmp.name) == 0) dst = &t.squad[i];
      if (dst) {
        tmp.starter = dst->starter;
        tmp.subbed = dst->subbed;
        if (!tmp.pos[0]) std::memcpy(tmp.pos, dst->pos, sizeof(tmp.pos));
      }
    } else if (t.n < kSquadMax) {
      dst = &t.squad[t.n++];
    }
    if (dst) *dst = tmp;
  }
  return true;
}

}  // namespace espn
}  // namespace touchfc
