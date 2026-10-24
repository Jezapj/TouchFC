#include "threads_win.hpp"
#include "log.hpp"
#include "win_prelude.hpp"

#include <avrt.h>

#include <string>
#include <vector>

namespace touchfc {
namespace {

DWORD g_mmcss_task = 0;
HANDLE g_mmcss = nullptr;

}  // namespace

void opt_out_power_throttling() {
  PROCESS_POWER_THROTTLING_STATE state{};
  state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
  state.StateMask = 0;
  if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state))) {
    log_line("SetProcessInformation(ProcessPowerThrottling) failed");
  }
}

void set_pipeline_thread() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
}

void set_telemetry_thread() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
}

void begin_present_mmcss() {
  g_mmcss = AvSetMmThreadCharacteristicsW(L"Playback", &g_mmcss_task);
  if (!g_mmcss) log_line("AvSetMmThreadCharacteristicsW failed");
}

void end_present_mmcss() {
  if (g_mmcss) {
    AvRevertMmThreadCharacteristics(g_mmcss);
    g_mmcss = nullptr;
  }
}

bool pin_current_thread_to_one_ccd() {
  ULONG bytes = 0;
  GetSystemCpuSetInformation(nullptr, 0, &bytes, GetCurrentProcess(), 0);
  if (bytes == 0) {
    log_line("GetSystemCpuSetInformation returned no data");
    return false;
  }
  std::vector<unsigned char> buf(bytes);
  if (!GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buf.data()), bytes,
                                  &bytes, GetCurrentProcess(), 0)) {
    log_line("GetSystemCpuSetInformation failed");
    return false;
  }

  int counts[64] = {};
  for (ULONG off = 0; off + sizeof(DWORD) <= buf.size();) {
    auto* info = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buf.data() + off);
    if (info->Size == 0) break;
    if (info->Type == CpuSetInformation) {
      const unsigned llc = info->CpuSet.LastLevelCacheIndex;
      if (llc < 64) ++counts[llc];
    }
    off += info->Size;
  }
  int best = -1;
  int best_n = 0;
  for (int i = 0; i < 64; ++i) {
    if (counts[i] > best_n) {
      best_n = counts[i];
      best = i;
    }
  }
  if (best < 0) return false;

  bool core_seen[256] = {};
  std::vector<ULONG> ids;
  for (ULONG off = 0; off + sizeof(DWORD) <= buf.size();) {
    auto* info = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buf.data() + off);
    if (info->Size == 0) break;
    if (info->Type == CpuSetInformation && info->CpuSet.LastLevelCacheIndex == static_cast<BYTE>(best)) {
      const unsigned core = info->CpuSet.CoreIndex;
      if (core < 256 && !core_seen[core]) {
        core_seen[core] = true;
        ids.push_back(info->CpuSet.Id);
      }
    }
    off += info->Size;
  }
  if (ids.empty()) return false;
  if (!SetThreadSelectedCpuSets(GetCurrentThread(), ids.data(), static_cast<ULONG>(ids.size()))) {
    log_line("SetThreadSelectedCpuSets failed");
    return false;
  }
  log_line("pinned thread to L3 group " + std::to_string(best) + " (" + std::to_string(ids.size()) +
           " cores)");
  return true;
}

}  // namespace touchfc
