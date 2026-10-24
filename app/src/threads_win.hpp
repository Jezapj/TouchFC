#pragma once

namespace touchfc {

void opt_out_power_throttling();
void set_pipeline_thread();
void set_telemetry_thread();
void begin_present_mmcss();
void end_present_mmcss();
// Pins the calling thread to one logical processor per core on a single L3.
// Returns false if the API refuses; the thread still runs.
bool pin_current_thread_to_one_ccd();

}  // namespace touchfc
