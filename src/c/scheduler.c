#include "scheduler.h"
#include "alarm_store.h"
#include "smart_alarm.h"

// Wakeups cannot be scheduled within 30 s from now
#define MIN_LEAD_SECONDS 31
// How far ahead the smart-window re-entry wakeup is placed. Must be > 60 s:
// PebbleOS refuses a wakeup within one minute of an already-scheduled one.
#define SC_REENTRY_GAP_S (3 * 60)

static uint8_t s_failed_mask;
static time_t s_next[ALARM_SLOTS];

// Local time hour:minute on the day of "day" (local) as a UTC timestamp
static time_t prv_at(struct tm day, int hour, int minute) {
  day.tm_hour = hour;
  day.tm_min = minute;
  day.tm_sec = 0;
  day.tm_isdst = -1;
  return mktime(&day);
}

time_t scheduler_next_occurrence(int slot, time_t now) {
  const Alarm *a = store_get(slot);
  if (!a || !a->used || !a->enabled) return 0;
  struct tm today = *localtime(&now);
  for (int offset = 0; offset <= 7; offset++) {
    struct tm day = today;
    day.tm_mday += offset;
    day.tm_hour = 12;  // noon, so that mktime normalizes the day safely
    day.tm_isdst = -1;
    time_t noon = mktime(&day);
    struct tm norm = *localtime(&noon);
    if (a->days) {
      int bit = (norm.tm_wday + 6) % 7;  // tm_wday: 0 = Sunday; bit 0 = Monday
      if (!(a->days & (1 << bit))) continue;
    }
    time_t t = prv_at(norm, a->hour, a->minute);
    // Already rung early via a smart wake: do not arm this exact occurrence again,
    // or the hard deadline it was armed for would ring it a second time.
    if (t == store_served_at(slot)) continue;
    if (t >= now + MIN_LEAD_SECONDS) return t;
  }
  return 0;
}

bool scheduler_time_conflict(int slot, uint8_t hour, uint8_t minute) {
  int mine = hour * 60 + minute;
  for (int other = 1; other <= ALARM_SLOTS; other++) {
    if (other == slot) continue;
    const Alarm *a = store_get(other);
    if (!a->used || !a->enabled) continue;
    int diff = (a->hour * 60 + a->minute) - mine;
    if (diff < 0) diff = -diff;
    if (diff > 720) diff = 1440 - diff;  // across midnight
    if (diff == 1) return true;
  }
  return false;
}

void scheduler_update(void) {
  wakeup_cancel_all();
  s_failed_mask = 0;
  memset(s_next, 0, sizeof(s_next));
  time_t now = time(NULL);

  // Time per slot: the snooze time has priority over the regular one. A snoozed
  // alarm never gets a smart window of its own -- it already rang once tonight.
  time_t when[ALARM_SLOTS] = {0};
  time_t window_when[ALARM_SLOTS] = {0};
  for (int slot = 1; slot <= ALARM_SLOTS; slot++) {
    const Alarm *a = store_get(slot);
    time_t snooze = store_snooze_until(slot);
    if (snooze && (!a->used || !a->enabled || snooze < now + MIN_LEAD_SECONDS)) {
      // snooze expired (e.g. watch was off), alarm deleted or turned off
      store_set_snooze_until(slot, 0);
      snooze = 0;
    }
    if (snooze && a->used) {
      when[slot - 1] = snooze;
    } else {
      time_t occ = scheduler_next_occurrence(slot, now);
      when[slot - 1] = occ;
      if (occ && smart_wanted(a)) {
        time_t ws = smart_window_start(a, occ);
        // Also filters out the slot currently being monitored: its window already
        // started, so its own window_start lies at or before now.
        if (ws > now + MIN_LEAD_SECONDS) window_when[slot - 1] = ws;
      }
    }
  }

  // Merge identical deadline times into one wakeup
  uint8_t done = 0;
  for (int i = 0; i < ALARM_SLOTS; i++) {
    if (!when[i] || (done & (1 << i))) continue;
    uint8_t mask = 0;
    for (int j = i; j < ALARM_SLOTS; j++) {
      if (when[j] == when[i]) mask |= (1 << j);
    }
    done |= mask;
    WakeupId id = wakeup_schedule(when[i], mask, true);
    if (id < 0) {
      s_failed_mask |= mask;
      APP_LOG(APP_LOG_LEVEL_WARNING, "Wakeup for slots 0x%02x at %ld rejected (%ld)",
              mask, (long)when[i], (long)id);
      continue;
    }
    for (int j = 0; j < ALARM_SLOTS; j++) {
      if (mask & (1 << j)) s_next[j] = when[i];
    }
    APP_LOG(APP_LOG_LEVEL_INFO, "Wakeup %ld for slots 0x%02x in %ld s", (long)id, mask,
            (long)(when[i] - now));
  }

  // Merge identical smart-window-start times into one flagged wakeup. Losing one of
  // these costs nothing but the smart behaviour for that occurrence: the deadline
  // wakeup above is scheduled regardless and still rings on time.
  uint8_t wdone = 0;
  for (int i = 0; i < ALARM_SLOTS; i++) {
    if (!window_when[i] || (wdone & (1 << i))) continue;
    uint8_t mask = 0;
    for (int j = i; j < ALARM_SLOTS; j++) {
      if (window_when[j] == window_when[i]) mask |= (1 << j);
    }
    wdone |= mask;
    WakeupId id = wakeup_schedule(window_when[i], SCHED_WINDOW_FLAG | mask, true);
    if (id < 0) {
      APP_LOG(APP_LOG_LEVEL_WARNING, "Smart window for slots 0x%02x at %ld rejected (%ld)",
              mask, (long)window_when[i], (long)id);
      continue;
    }
    APP_LOG(APP_LOG_LEVEL_INFO, "Smart window %ld for slots 0x%02x in %ld s", (long)id, mask,
            (long)(window_when[i] - now));
  }

  // While a smart window is actively being monitored, keep a short re-entry wakeup
  // armed so the app is revived if something kills it mid-window. wakeup_cancel_all()
  // above just dropped it, so it must be re-armed every time this function runs, not
  // only when the window first opens.
  if (smart_is_open()) {
    wakeup_schedule(now + SC_REENTRY_GAP_S, SCHED_REENTRY_COOKIE, false);
  }
}

uint8_t scheduler_failed_mask(void) {
  return s_failed_mask;
}

time_t scheduler_next_time(int slot) {
  return (slot >= 1 && slot <= ALARM_SLOTS) ? s_next[slot - 1] : 0;
}
