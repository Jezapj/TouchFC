#include "services.hpp"

#include "http_win.hpp"
#include "jpeg_win.hpp"
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

// M18: the score went up. The banner shows at once; the name follows.
void Services::add_goal(int side) {
  if (m_.goal_n >= static_cast<std::uint32_t>(kGoalsMax)) return;
  const std::uint32_t i = m_.goal_n++;
  GoalInfo& g = m_.goals[i];
  g = {};
  g.side = static_cast<std::uint8_t>(side);
  g.minute = m_.clock_ms >= 0 ? static_cast<std::uint8_t>(std::min(250, m_.clock_ms / 60000 + 1)) : 0;
  goal_s_[i] = now_s();
  goal_tries_[i] = 0;
  ++m_.goal_seq;
  dirty_ = true;
  log_line(std::string("goal: ") + m_.team[side].code);
  // The shot that went in is the scoring team's newest clip.
  std::lock_guard<std::mutex> lock(mu_);
  for (auto it = log_.rbegin(); it != log_.rend(); ++it) {
    if (goal_s_[i] - it->wall_s > 45) break;
    if (it->e.kind == 1 && it->e.team == side + 1) {
      it->e.goal = 1;
      break;
    }
  }
}

// The score went back down: the read that added the goal was wrong.
void Services::drop_goal(int side) {
  for (int i = static_cast<int>(m_.goal_n) - 1; i >= 0; --i) {
    if (m_.goals[i].side != side) continue;
    for (int k = i; k + 1 < static_cast<int>(m_.goal_n); ++k) {
      m_.goals[k] = m_.goals[k + 1];
      goal_s_[k] = goal_s_[k + 1];
      goal_tries_[k] = goal_tries_[k + 1];
    }
    --m_.goal_n;
    ++m_.goal_seq;
    dirty_ = true;
    return;
  }
}

// Name each new goal: a squad name of the scoring team that OCR saw from 25 s
// before the score changed; failing that for 30 s, the provider's scoring
// plays, asked at most five times, 30 s apart (the provider lags the match).
void Services::resolve_goals() {
  const std::int64_t now = now_s();
  for (std::uint32_t i = 0; i < m_.goal_n; ++i) {
    GoalInfo& g = m_.goals[i];
    const std::int64_t age = now - goal_s_[i];
    if (g.name[0] || age > 240) continue;
    if (seen_side_ == g.side && seen_s_ >= goal_s_[i] - 25) {
      const PlayerInfo& p = m_.team[g.side].squad[seen_idx_];
      std::snprintf(g.name, sizeof(g.name), "%s", p.name);
      g.jersey = p.jersey;
      g.source = 1;
      seen_side_ = -1;
      ++m_.goal_seq;
      dirty_ = true;
      log_line(std::string("scorer from the caption: ") + g.name);
      continue;
    }
    if (age < 30 || !m_.event_id || goal_tries_[i] >= 5 || now - goal_try_s_ < 30) continue;
    ++goal_tries_[i];
    goal_try_s_ = now;
    std::string body, err;
    const std::string key = std::string("espn/") + m_.league + "_sum_" + std::to_string(m_.event_id);
    if (!cached_get(espn::url_summary(m_.league, m_.event_id), key, 0, body, err)) continue;
    std::vector<GoalInfo> plays;
    try {
      plays = espn::parse_goals(body, m_);
    } catch (...) {
    }
    // Our list starts when the app did; the provider's at kick-off.
    const int total = std::max<int>(0, m_.team[0].score) + std::max<int>(0, m_.team[1].score);
    const int k = total - static_cast<int>(m_.goal_n) + static_cast<int>(i);
    if (k < 0 || k >= static_cast<int>(plays.size())) continue;
    const GoalInfo& src = plays[static_cast<std::size_t>(k)];
    if (!src.name[0] || (!src.own_goal && src.side != g.side)) continue;
    std::memcpy(g.name, src.name, sizeof(g.name));
    g.own_goal = src.own_goal;
    if (!g.minute) g.minute = src.minute;
    g.source = 2;
    for (const TeamInfo& t : m_.team)
      for (int q = 0; q < t.n; ++q)
        if (std::strcmp(t.squad[q].name, g.name) == 0) g.jersey = t.squad[q].jersey;
    ++m_.goal_seq;
    dirty_ = true;
    log_line(std::string("scorer from the provider: ") + g.name);
  }
}

void Services::on_read(const char* text) {
  {
    int side = 0, idx = 0;
    if (find_player(text, m_, side, idx)) {
      seen_side_ = side;
      seen_idx_ = idx;
      seen_s_ = now_s();
    }
  }
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
    const int was[2] = {m_.team[hs].score, m_.team[as].score}, is[2] = {r.home_score, r.away_score};
    const int sides[2] = {hs, as};
    for (int k = 0; k < 2; ++k) {
      if (was[k] >= 0 && is[k] == was[k] + 1) add_goal(sides[k]);
      else if (was[k] >= 0 && is[k] < was[k]) drop_goal(sides[k]);
    }
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
    case ServiceCmd::Kind::PlayLog:
      play_log(c.log_id);
      break;
  }
}

std::vector<LogEntry> Services::replay_log() {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<LogEntry> out;
  out.reserve(log_.size());
  for (const auto& c : log_) out.push_back(c.e);
  return out;
}

// M18: keep the clip as JPEGs (about 4 MB, against 73 MB raw), newest last.
void Services::save_clip(const ClipBuffer& clip, int keep) {
  if (!jpeg_ || !jpeg_->ready() || clip.n < 4) return;
  LogClip lc;
  lc.req = clip.req;
  lc.wall_s = now_s();
  lc.meta.reserve(static_cast<std::size_t>(clip.n));
  lc.jpg.reserve(static_cast<std::size_t>(clip.n));
  std::size_t bytes = 0;
  for (int k = 0; k < clip.n; ++k) {
    std::vector<std::uint8_t> j;
    if (!jpeg_->encode(clip.frame(k), kThumbW, kThumbH, 0.8f, j)) continue;
    bytes += j.size();
    lc.meta.push_back(clip.meta[k]);
    lc.jpg.push_back(std::move(j));
  }
  if (lc.jpg.size() < 4) return;
  lc.e.id = ++log_id_;
  lc.e.kind = clip.req.kind;
  lc.e.team = clip.req.team;
  lc.e.minute = m_.clock_ms >= 0 ? static_cast<std::uint8_t>(std::min(250, m_.clock_ms / 60000 + 1)) : 0;
  lc.e.seconds = lc.meta.back().t_s - lc.meta.front().t_s;
  lc.e.kbytes = static_cast<std::uint32_t>(bytes / 1024);
  log_line("replay log: saved clip " + std::to_string(lc.e.id) + ", " + std::to_string(lc.e.kbytes) + " KB");
  std::lock_guard<std::mutex> lock(mu_);
  log_.push_back(std::move(lc));
  while (static_cast<int>(log_.size()) > keep) log_.erase(log_.begin());
}

// Decode a logged clip into the pipeline's log clip; present plays it.
void Services::play_log(std::uint32_t id) {
  const LogClip* src = nullptr;
  for (const auto& c : log_)
    if (c.e.id == id) src = &c;
  if (!src || !jpeg_) return;
  ClipBuffer& dst = pipe_.log_clip();
  // One on screen: present frees it within a frame of the click.
  bool mine = false;
  for (int tries = 0; tries < 30 && !mine; ++tries) {
    for (const ClipState from : {ClipState::Free, ClipState::Ready}) {
      ClipState s = from;
      if (!mine && dst.state.compare_exchange_strong(s, ClipState::Filling)) mine = true;
    }
    if (!mine) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!mine) return;
  dst.allocate();
  dst.req = src->req;
  dst.n = 0;
  for (std::size_t k = 0; k < src->jpg.size() && dst.n < kClipFrames; ++k) {
    if (!jpeg_->decode(src->jpg[k].data(), src->jpg[k].size(), kThumbW, kThumbH, dst.frame(dst.n))) continue;
    dst.meta[dst.n++] = src->meta[k];
  }
  dst.seq = ++clip_seq_;
  dst.state.store(dst.n >= 4 ? ClipState::Ready : ClipState::Free, std::memory_order_release);
}

// M15: wait until the ring holds t + 1.5 s, then copy the window into a free
// clip (or over one that has not started playing). Present plays it.
void Services::serve_clips() {
  ClipReq r;
  while (clip_wait_n_ < 4 && pipe_.clip_requests().try_pop(r)) {
    clip_wait_[clip_wait_n_++] = r;
    log_line("mini replay requested at " + std::to_string(r.t_s) + " s");
  }
  const ThumbRing& ring = pipe_.thumb_ring();
  while (clip_wait_n_ > 0 && ring.last_t() >= clip_wait_[0].t_s + kClipAfterS) {
    r = clip_wait_[0];
    for (int k = 1; k < clip_wait_n_; ++k) clip_wait_[k - 1] = clip_wait_[k];
    --clip_wait_n_;
    ClipBuffer* dst = nullptr;
    for (const ClipState from : {ClipState::Free, ClipState::Ready})
      for (int i = 0; i < 2 && !dst; ++i) {
        ClipState s = from;
        if (pipe_.clip(i).state.compare_exchange_strong(s, ClipState::Filling)) dst = &pipe_.clip(i);
      }
    if (!dst) {
      log_line("mini replay skipped: both clips busy");
      continue;
    }
    bool dead_before = false;
    const int n = copy_clip(ring, r, *dst, dead_before);
    // Playback is by timestamp, so a low present rate is fine; only a window
    // the ring barely covers (gap, pause, startup) is skipped.
    const float span = n > 1 ? dst->meta[n - 1].t_s - dst->meta[0].t_s : 0.f;
    if ((dead_before && r.kind != 2) || n < 4 || span < 2.5f) {
      log_line(dead_before ? "mini replay skipped: a cut or replay inside the window"
                           : "mini replay skipped: the ring does not cover the window");
      dst->state.store(ClipState::Free, std::memory_order_release);
      continue;
    }
    log_line("mini replay ready: " + std::to_string(n) + " frames, " + std::to_string(span) + " s");
    dst->seq = ++clip_seq_;
    dst->state.store(ClipState::Ready, std::memory_order_release);
    // Only this thread writes clips, so reading it here is safe while present
    // plays it, and after present frees it.
    if (const int keep = pipe_.config().load().replay_log_n) save_clip(*dst, keep);
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
  jpeg_ = std::make_unique<Jpeg>();
  {
    // Round trip a small gradient once, so a broken codec turns the log off
    // here and not on the first shot.
    constexpr int kN = 64;
    std::vector<std::uint8_t> px(kN * kN * 4), back(kN * kN * 4), j;
    for (int y = 0; y < kN; ++y)
      for (int x = 0; x < kN; ++x) {
        std::uint8_t* p = &px[static_cast<std::size_t>(y * kN + x) * 4];
        p[0] = static_cast<std::uint8_t>(x * 4);
        p[1] = static_cast<std::uint8_t>(y * 4);
        p[2] = static_cast<std::uint8_t>(2 * (x + y));
        p[3] = 255;
      }
    int worst = 255;
    if (jpeg_->encode(px.data(), kN, kN, 0.8f, j) && jpeg_->decode(j.data(), j.size(), kN, kN, back.data())) {
      worst = 0;
      for (std::size_t i = 0; i < px.size(); ++i)
        if (i % 4 != 3) worst = std::max(worst, std::abs(static_cast<int>(px[i]) - static_cast<int>(back[i])));
    }
    if (worst > 48) {
      jpeg_.reset();
      log_line("replay log off: JPEG self-check failed");
    } else {
      log_line("replay log: JPEG codec ok (" + std::to_string(j.size()) + " bytes, max error " + std::to_string(worst) + ")");
    }
  }
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
    serve_clips();
    resolve_goals();
    if (m_.event_id && !m_.lineup_ok && now_s() - last_poll_s_ >= 120) fetch_summary(false);
    if (dirty_) publish();
  }
}

}  // namespace touchfc
