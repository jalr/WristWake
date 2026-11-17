#pragma once
// Scheduling: computes the next time of every active alarm and registers it as a wakeup.
// Alarms with the same time share one wakeup.
// A snoozed alarm uses its snooze time instead of its regular time.
#include <pebble.h>

// Wakeup cookie: bits 0-7 = slots 1-8 that should ring
#define SCHED_SLOT_MASK 0xFF
// Set alongside the slot mask: these slots' SMART WINDOW starts now, nothing should
// ring yet. Never set together with a bare SCHED_SLOT_MASK cookie -- the two kinds of
// wakeup are always scheduled and dispatched separately.
#define SCHED_WINDOW_FLAG 0x100
// No slot payload: re-check the smart window that was open when this was armed, in
// case the app was killed and relaunched since. Re-armed on every scheduler_update()
// call for as long as a window is open (see smart_is_open()), dropped the moment it
// closes.
#define SCHED_REENTRY_COOKIE 0x200

// Cancel all wakeups and reschedule (on every start and after every change).
void scheduler_update(void);

// Slots (bit mask) whose time could not be registered in the last scheduler_update.
uint8_t scheduler_failed_mask(void);

// Next time (UTC) of a slot according to the last scheduling, 0 = none.
time_t scheduler_next_time(int slot);

// Next regular time of an alarm after "now" (ignoring snooze), 0 = none.
time_t scheduler_next_occurrence(int slot, time_t now);

// true if another active alarm is exactly 1 minute away from hour:minute.
// The watch does not allow such gaps (wakeups need a 1 minute window).
bool scheduler_time_conflict(int slot, uint8_t hour, uint8_t minute);
