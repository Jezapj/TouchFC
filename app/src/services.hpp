#pragma once

#include "touchfc/espn.hpp"
#include "touchfc/match_info.hpp"
#include "touchfc/mini_replay.hpp"
#include "touchfc/scorebug.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace touchfc {

class Pipeline;

struct ServiceCmd {
  enum class Kind : std::uint8_t { SetLeague, SetTeams, ClearTeams, Refresh, Sub, PlayLog };
  std::uint32_t log_id = 0;  // PlayLog
  Kind kind = Kind::Refresh;
  char league[24]{};
  char date[10]{};          // YYYYMMDD
  std::uint32_t home = 0;   // provider team ids
  std::uint32_t away = 0;
  std::uint8_t side = 0;    // Sub: team index, squad index off and on
  std::uint8_t off = 0;
  std::uint8_t on = 0;
};

// M18: one saved key moment, as the Replays tab lists it.
struct LogEntry {
  std::uint32_t id = 0;
  std::uint8_t kind = 0;    // ClipReq kind: 0 key pass, 1 shot, 2 manual test
  std::uint8_t team = 0;    // 1 A, 2 B
  std::uint8_t minute = 0;  // scorebug clock, 0 unknown
  std::uint8_t goal = 0;    // the score went up just after this shot
  float seconds = 0.f;
  std::uint32_t kbytes = 0;
};

class Jpeg;

// T7: slow work off the hot path. Scorebug OCR, the stats provider (HTTP,
// JSON, disk cache) and manual match setup. Publishes MatchInfo to T3 and the
// UI through the pipeline's triple buffers; never waits on T1 to T3.
class Services {
 public:
  explicit Services(Pipeline& pipe);
  ~Services();
  Services(const Services&) = delete;
  Services& operator=(const Services&) = delete;

  void start();
  void stop();

  // UI thread.
  void post(const ServiceCmd& cmd);
  std::vector<espn::TeamEntry> teams();
  std::string league();
  std::vector<LogEntry> replay_log();  // oldest first

 private:
  void add_goal(int side);
  void drop_goal(int side);
  void resolve_goals();
  void save_clip(const ClipBuffer& clip, int keep);
  void play_log(std::uint32_t id);
  struct LogClip {
    LogEntry e{};
    ClipReq req{};
    std::int64_t wall_s = 0;
    std::vector<ThumbMeta> meta;
    std::vector<std::vector<std::uint8_t>> jpg;
  };
  std::vector<LogClip> log_;  // T7 owns it; mu_ guards changes and the UI's copy
  std::uint32_t log_id_ = 0;
  std::unique_ptr<Jpeg> jpeg_;
  // Goal scorers: the last squad name OCR saw, and per goal when it was scored.
  int seen_side_ = -1, seen_idx_ = 0;
  std::int64_t seen_s_ = 0;
  std::int64_t goal_s_[kGoalsMax]{};
  int goal_tries_[kGoalsMax]{};
  std::int64_t goal_try_s_ = 0;
  void run();
  void handle(const ServiceCmd& c);
  void load_teams();
  void identify(std::uint32_t home, std::uint32_t away, MatchSource src);
  void fetch_summary(bool fresh);
  bool on_read(const char* text, const std::vector<BugWord>& words);
  void publish();
  void serve_clips();
  void status(const std::string& s);

  Pipeline& pipe_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::mutex mu_;
  std::vector<ServiceCmd> queue_;
  std::vector<espn::TeamEntry> teams_;
  std::string league_ = "eng.1";
  std::string date_;
  MatchInfo m_{};
  bool dirty_ = true;
  std::int64_t last_poll_s_ = 0;
  nlohmann::json overrides_;
  ScorebugVote vote_;
  ScoreVote score_vote_;
  std::string last_unparsed_;
  ClipReq clip_wait_[4]{};  // requests waiting for t + 1.5 s to reach the ring
  int clip_wait_n_ = 0;
  std::uint64_t clip_seq_ = 0;
};

}  // namespace touchfc
