#include "services.hpp"

#include "http_win.hpp"
#include "log.hpp"
#include "ocr_win.hpp"
#include "pipeline.hpp"

#include "touchfc/scorebug.hpp"

#include <winrt/base.h>

#include <chrono>
#include <ctime>
#include <fstream>

namespace touchfc {

namespace {

std::string today() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &t);
  char buf[16];
  std::strftime(buf, sizeof(buf), "%Y%m%d", &tm);
  return buf;
}

std::string shift_day(const std::string& yyyymmdd, int days) {
  std::tm tm{};
  tm.tm_year = std::stoi(yyyymmdd.substr(0, 4)) - 1900;
  tm.tm_mon = std::stoi(yyyymmdd.substr(4, 2)) - 1;
  tm.tm_mday = std::stoi(yyyymmdd.substr(6, 2)) + days;
  tm.tm_hour = 12;
  std::mktime(&tm);
  char buf[16];
  std::strftime(buf, sizeof(buf), "%Y%m%d", &tm);
  return buf;
}

std::int64_t now_s() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

Services::Services(Pipeline& pipe) : pipe_(pipe), date_(today()) {}

Services::~Services() { stop(); }

void Services::start() { thread_ = std::thread([this] { run(); }); }

void Services::stop() {
  if (!thread_.joinable()) return;  // also runs from the destructor
  stop_.store(true);
  pipe_.ocr_requests().wake();
  if (thread_.joinable()) thread_.join();
}

void Services::post(const ServiceCmd& cmd) {
  std::lock_guard<std::mutex> lock(mu_);
  queue_.push_back(cmd);
}

std::vector<espn::TeamEntry> Services::teams() {
  std::lock_guard<std::mutex> lock(mu_);
  return teams_;
}

std::string Services::league() {
  std::lock_guard<std::mutex> lock(mu_);
  return league_;
}

void Services::status(const std::string& s) {
  std::snprintf(m_.status, sizeof(m_.status), "%s", s.c_str());
  log_line("match: " + s);
  dirty_ = true;
}

void Services::publish() {
  ++m_.version;
  pipe_.match_t3().write_slot() = m_;
  pipe_.match_t3().publish();
  pipe_.match_ui().write_slot() = m_;
  pipe_.match_ui().publish();
  dirty_ = false;
}

void Services::load_teams() {
  std::string body, err;
  std::string league;
  {
    std::lock_guard<std::mutex> lock(mu_);
    league = league_;
  }
  std::vector<espn::TeamEntry> list;
  if (cached_get(espn::url_teams(league.c_str()), "espn/" + league + "_teams", 7 * 86400, body, err)) {
    try {
      list = espn::parse_teams(body);
    } catch (...) {
    }
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    teams_ = list;
  }
  status(list.empty() ? "no team list for " + league + (err.empty() ? "" : ": " + err)
                      : std::to_string(list.size()) + " teams in " + league);
}

// Fixture, colours and form from the scoreboard (the date, then a day either
// side for time zones), then lineups and season stats.
void Services::identify(std::uint32_t home, std::uint32_t away, MatchSource src) {
  const std::string league = this->league();
  MatchInfo m{};
  std::snprintf(m.league, sizeof(m.league), "%s", league.c_str());
  std::snprintf(m.date, sizeof(m.date), "%s", date_.c_str());
  m.source = src;
  std::memcpy(m.roi, m_.roi, sizeof(m.roi));
  bool found = false;
  for (const int d : {0, -1, 1}) {
    const std::string day = shift_day(date_, d);
    std::string body, err;
    if (!cached_get(espn::url_scoreboard(league.c_str(), day.c_str()), "espn/" + league + "_sb_" + day, 600, body,
                    err))
      continue;
    try {
      found = espn::parse_scoreboard(body, home, away, m);
    } catch (...) {
      found = false;
    }
    if (found) break;
  }
  if (!found) {
    // No fixture that day: names and codes from the team list, no lineup.
    for (const auto& t : teams()) {
      for (int s = 0; s < 2; ++s) {
        if (t.id != (s == 0 ? home : away)) continue;
        m.team[s].provider_id = t.id;
        std::snprintf(m.team[s].name, sizeof(m.team[s].name), "%s", t.name.c_str());
        std::snprintf(m.team[s].code, sizeof(m.team[s].code), "%s", t.code.c_str());
      }
    }
    m.teams_ok = m.team[0].provider_id && m.team[1].provider_id ? 1 : 0;
  }
  m_ = m;
  status(found ? std::string("fixture ") + m_.team[0].code + " v " + m_.team[1].code
               : "no fixture on " + date_ + "; teams set, no lineup");
  if (found) fetch_summary(false);
  for (int s = 0; s < 2; ++s) {
    if (!m_.team[s].provider_id) continue;
    std::string body, err;
    const std::string key = "espn/" + league + "_roster_" + std::to_string(m_.team[s].provider_id) + "_" + date_;
    if (!cached_get(espn::url_roster(league.c_str(), m_.team[s].provider_id), key, 12 * 3600, body, err)) continue;
    try {
      espn::parse_roster(body, m_.team[s]);
    } catch (...) {
    }
  }
  dirty_ = true;
}

// Lineups are posted 20 to 40 minutes before kick-off: polled until they
// appear, then cached for good.
void Services::fetch_summary(bool fresh) {
  if (!m_.event_id) return;
  std::string body, err;
  const std::string key = std::string("espn/") + m_.league + "_sum_" + std::to_string(m_.event_id);
  const int max_age = m_.lineup_ok ? -1 : fresh ? 0 : 120;
  last_poll_s_ = now_s();
  if (!cached_get(espn::url_summary(m_.league, m_.event_id), key, max_age, body, err)) {
    status("summary: " + err);
    return;
  }
  const bool had = m_.lineup_ok;
  try {
    espn::parse_summary(body, m_);
  } catch (...) {
    status("summary: unexpected JSON");
    return;
  }
  if (m_.lineup_ok && !had) status("lineups loaded");
  dirty_ = true;
}

void Services::on_read(const char* text) {
  ScorebugRead r;
  if (!parse_scorebug(text, r)) {
    if (last_unparsed_ != text) {
      last_unparsed_ = text;
      status(std::string("scorebug text not understood: ") + text);
    }
    return;
  }
  if (r.clock_ms >= 0 && r.clock_ms != m_.clock_ms) {
    m_.clock_ms = r.clock_ms;
    dirty_ = true;
  }
  // Left is home; map by code when both are known, so a swapped bug still scores right.
  int hs = 0, as = 1;
  if (m_.teams_ok && std::strcmp(r.home, m_.team[1].code) == 0) std::swap(hs, as);
  if (m_.team[hs].score != r.home_score || m_.team[as].score != r.away_score) {
    m_.team[hs].score = static_cast<std::int16_t>(r.home_score);
    m_.team[as].score = static_cast<std::int16_t>(r.away_score);
    dirty_ = true;
  }
  if (!vote_.add(r) || m_.source == MatchSource::Manual || m_.source == MatchSource::Scorebug) return;
  // Codes to provider ids: data/team_codes.json overrides, then abbreviations.
  const std::string league = this->league();
  std::uint32_t ids[2] = {0, 0};
  const char* codes[2] = {vote_.home(), vote_.away()};
  for (int s = 0; s < 2; ++s) {
    const auto lg = overrides_.find(league);
    if (lg != overrides_.end() && lg->is_object()) {
      const auto it = lg->find(codes[s]);
      if (it != lg->end() && it->is_string()) ids[s] = static_cast<std::uint32_t>(std::stoul(it->get<std::string>()));
    }
    if (!ids[s])
      for (const auto& t : teams())
        if (t.code == codes[s]) ids[s] = t.id;
  }
  if (!ids[0] || !ids[1]) {
    status(std::string("scorebug ") + codes[0] + " v " + codes[1] + ": not in " + league +
           " (add to data/team_codes.json or pick teams)");
    m_.source = MatchSource::Scorebug;  // do not retry every read; manual still wins
    return;
  }
  identify(ids[0], ids[1], MatchSource::Scorebug);
}

void Services::handle(const ServiceCmd& c) {
  switch (c.kind) {
    case ServiceCmd::Kind::SetLeague: {
      {
        std::lock_guard<std::mutex> lock(mu_);
        league_ = c.league;
      }
      if (c.date[0]) date_ = c.date;
      load_teams();
      break;
    }
    case ServiceCmd::Kind::SetTeams:
      if (c.date[0]) date_ = c.date;
      identify(c.home, c.away, MatchSource::Manual);
      break;
    case ServiceCmd::Kind::ClearTeams: {
      MatchInfo m{};
      std::memcpy(m.roi, m_.roi, sizeof(m.roi));
      m_ = m;
      status("cleared");
      break;
    }
    case ServiceCmd::Kind::Refresh:
      fetch_summary(true);
      break;
    case ServiceCmd::Kind::Sub: {
      TeamInfo& t = m_.team[c.side & 1];
      if (c.off < t.n && c.on < t.n) {
        t.squad[c.off].subbed = 1;
        t.squad[c.on].subbed = 2;
        dirty_ = true;
      }
      break;
    }
  }
}

void Services::run() {
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  {
    std::ifstream in("data/team_codes.json");
    if (in) overrides_ = nlohmann::json::parse(in, nullptr, false);
    if (overrides_.is_discarded()) overrides_ = nlohmann::json::object();
  }
  Ocr ocr;
  status(ocr.ready() ? "scorebug OCR ready" : "no OCR engine for the user's languages");
  load_teams();
  publish();
  std::string text;
  while (!stop_.load()) {
    std::vector<ServiceCmd> cmds;
    {
      std::lock_guard<std::mutex> lock(mu_);
      cmds.swap(queue_);
    }
    for (const auto& c : cmds) handle(c);

    std::uint32_t idx = 0;
    if (pipe_.ocr_requests().try_pop(idx)) {
      OcrCrop& crop = pipe_.ocr_crop(idx);
      if (std::memcmp(m_.roi, crop.roi, sizeof(m_.roi)) != 0) {
        std::memcpy(m_.roi, crop.roi, sizeof(m_.roi));
        dirty_ = true;
      }
      if (ocr.read(crop.px.get(), crop.w, crop.h, crop.w * 4, text)) on_read(text.c_str());
      crop.busy.store(false, std::memory_order_release);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (m_.event_id && !m_.lineup_ok && now_s() - last_poll_s_ >= 120) fetch_summary(false);
    if (dirty_) publish();
  }
}

}  // namespace touchfc
