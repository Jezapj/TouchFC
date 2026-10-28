#pragma once

#include "touchfc/espn.hpp"
#include "touchfc/match_info.hpp"
#include "touchfc/scorebug.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace touchfc {

class Pipeline;

struct ServiceCmd {
  enum class Kind : std::uint8_t { SetLeague, SetTeams, ClearTeams, Refresh, Sub };
  Kind kind = Kind::Refresh;
  char league[16]{};
  char date[10]{};          // YYYYMMDD
  std::uint32_t home = 0;   // provider team ids
  std::uint32_t away = 0;
  std::uint8_t side = 0;    // Sub: team index, squad index off and on
  std::uint8_t off = 0;
  std::uint8_t on = 0;
};

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

 private:
  void run();
  void handle(const ServiceCmd& c);
  void load_teams();
  void identify(std::uint32_t home, std::uint32_t away, MatchSource src);
  void fetch_summary(bool fresh);
  void on_read(const char* text);
  void publish();
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
  std::string last_unparsed_;
};

}  // namespace touchfc
