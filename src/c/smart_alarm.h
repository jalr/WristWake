#pragma once
// Smart alarm: per-alarm sleep-phase detection. Opens a monitoring window before an
// alarm's set time and rings early on detected light sleep; otherwise rings exactly
// at the set time, same as a normal alarm. At most one window is monitored at a time --
// an alarm whose window would overlap another one already open simply rings,
// unmonitored, at its own set time.
#include <pebble.h>
#include "alarm_store.h"

// Is this alarm configured for a smart window at all (window length > 0)?
bool smart_wanted(const Alarm *a);

// When this alarm's smart window would start, for an occurrence ringing at `deadline`
// at the latest. 0 if not wanted or deadline is 0.
time_t smart_window_start(const Alarm *a, time_t deadline);

// Is a window currently being monitored, and for which slot/deadline (1-8 / 0 if none)?
bool smart_is_open(void);
int smart_open_slot(void);
time_t smart_open_deadline(void);

// Begin monitoring `slot` towards `deadline` (its exact set time), and show the
// waiting screen. No-op if a window is already open, the alarm no longer wants one, or
// `deadline` is not still in the future.
void smart_try_open(int slot, time_t deadline);

// Called once at app launch. Resumes monitoring + the waiting screen from persisted
// state if a window was open and its deadline has not passed; otherwise clears it.
// Pass ring_pending = true when this launch is about to show the ringing screen
// instead (a deadline wakeup win the same launch) -- the persisted state is then just
// discarded, with no UI of its own.
void smart_resume_if_open(bool ring_pending);

// Called once a minute while a window is open. Reads Health, runs se_evaluate; on a
// fire it marks the occurrence served (so the hard deadline cannot ring it again),
// rings the open slot and closes the window. Otherwise keeps waiting for the deadline
// wakeup. No-op if nothing is open.
void smart_tick(void);

// End monitoring: drop the waiting screen, the re-entry wakeup and the persisted
// state. Safe to call when nothing is open.
void smart_close(void);
