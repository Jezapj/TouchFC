#pragma once

#include <cstdint>
#include <type_traits>

namespace touchfc {

// Pre-match context and the live scoreboard. Written by T7 (services), read by
// T3 and present through a triple buffer, so it is a fixed-size POD.
struct PlayerInfo {
  char name[28]{};
  char pos[6]{};        // provider position, e.g. "CD-L"
  std::uint8_t jersey = 0;
  std::uint8_t starter = 0;
  std::uint8_t out = 0;  // injured or suspended
  std::uint8_t subbed = 0;  // 1 off, 2 on (manual substitutions)
  std::uint16_t apps = 0;  // season, before the match
  std::uint16_t goals = 0;
  std::uint16_t assists = 0;
  std::uint16_t shots = 0;
};

inline constexpr int kSquadMax = 30;

// Provider's post-match team stats, for the M13 validation CSV.
struct ProviderStats {
  float possession_pct = -1.f;
  std::int32_t passes = -1;
  std::int32_t passes_ok = -1;
  std::int32_t shots = -1;
};

struct TeamInfo {
  char name[32]{};
  char code[6]{};       // provider abbreviation, matches most scorebugs
  char form[6]{};       // last five, most recent last, e.g. "WWDLW"
  char formation[10]{};
  std::uint32_t provider_id = 0;
  std::uint32_t color = 0;      // 0xRRGGBB
  std::uint32_t alt_color = 0;
  std::uint8_t has_color = 0;
  std::uint8_t n = 0;           // squad entries used
  std::int16_t score = -1;      // from the scorebug, -1 unknown
  ProviderStats post{};
  PlayerInfo squad[kSquadMax]{};
};

enum class MatchSource : std::uint8_t { None = 0, Scorebug = 1, Manual = 2, File = 3 };

struct MatchInfo {
  std::uint32_t version = 0;
  std::uint32_t event_id = 0;   // provider fixture id, 0 until found
  char league[16]{};            // provider slug, e.g. "eng.1"
  char date[10]{};              // YYYYMMDD
  MatchSource source = MatchSource::None;
  std::uint8_t teams_ok = 0;    // both sides identified
  std::uint8_t lineup_ok = 0;   // starting XIs known
  std::uint8_t pad = 0;
  std::int32_t clock_ms = -1;   // scorebug clock
  std::int32_t roi[4]{};        // scorebug x, y, w, h in frame pixels; w = 0 until found
  TeamInfo team[2]{};           // 0 home (left on the scorebug), 1 away
  char status[96]{};            // last T7 message for the control window
};

static_assert(std::is_trivially_copyable_v<MatchInfo>);

}  // namespace touchfc
