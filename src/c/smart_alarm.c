#include "smart_alarm.h"
#include "sleep_eval.h"
#include "health_read.h"
#include "smart_window.h"
#include "scheduler.h"
#include "ring.h"

#define KEY_SMART_WINDOW 7   // persisted SmartState; see alarm_store.c's key comment

// The one window this app ever monitors at a time, and the minimal state needed to
// resume it if the app is killed and relaunched mid-window. window_start is NOT
// persisted: it is re-derived from the alarm's own (still-stored) smart_window_min on
// every resume, which is simpler than keeping a second copy in sync and self-corrects
// if the setting changes mid-window (smart_tick notices and stands down -- see below).
typedef struct {
  int8_t   slot;       // -1 = none
  uint32_t deadline;
} SmartState;

static int8_t  s_slot = -1;
static time_t  s_deadline;
static time_t  s_window_start;

static void prv_persist(void) {
  SmartState st = { .slot = s_slot, .deadline = (uint32_t)s_deadline };
  persist_write_data(KEY_SMART_WINDOW, &st, sizeof(st));
}

static void prv_clear(void) {
  s_slot = -1;
  s_deadline = 0;
  s_window_start = 0;
  prv_persist();
}

bool smart_wanted(const Alarm *a) {
  return a && a->used && a->enabled && a->smart_window_min > 0;
}

time_t smart_window_start(const Alarm *a, time_t deadline) {
  if (!smart_wanted(a) || deadline <= 0) return 0;
  return deadline - (time_t)a->smart_window_min * 60;
}

bool smart_is_open(void) {
  return s_slot >= 0;
}

int smart_open_slot(void) {
  return s_slot;
}

time_t smart_open_deadline(void) {
  return s_deadline;
}

// How easily the smart alarm fires on movement, for each sensitivity level. Within
// se_default_cfg's clamp range (percentile 70..99, required minutes 1..5): Low sits at
// the hard end of both (fires only on a strong, sustained movement), High at the easy
// end (fires readily), Medium between the two.
static void prv_sens_params(SmartSensitivity sens, uint8_t *percentile, uint8_t *required_minutes) {
  switch (sens) {
    case SMART_SENS_LOW:
      *percentile = 97;
      *required_minutes = 5;
      break;
    case SMART_SENS_HIGH:
      *percentile = 80;
      *required_minutes = 2;
      break;
    case SMART_SENS_MEDIUM:
    default:
      *percentile = 90;
      *required_minutes = 3;
      break;
  }
}

void smart_try_open(int slot, time_t deadline) {
  if (smart_is_open()) return;   // single-active policy
  const Alarm *a = store_get(slot);
  if (!smart_wanted(a) || deadline <= time(NULL)) return;
  s_slot = (int8_t)slot;
  s_deadline = deadline;
  s_window_start = smart_window_start(a, deadline);
  prv_persist();
  smart_window_show(slot, deadline);
  scheduler_update();   // arms the re-entry wakeup now that a window is open
  smart_tick();         // evaluate right away rather than waiting a full minute
}

void smart_resume_if_open(bool ring_pending) {
  SmartState st;
  bool have = persist_read_data(KEY_SMART_WINDOW, &st, sizeof(st)) == sizeof(st);
  if (!have || st.slot < 0) {
    return;
  }
  time_t now = time(NULL);
  const Alarm *a = store_get(st.slot);
  if (ring_pending || !smart_wanted(a) || (time_t)st.deadline <= now) {
    // Gone, turned off, past its deadline already (the deadline wakeup -- or a missed
    // one -- has decided its fate), or this launch is about to show the ring screen
    // for some alarm anyway, which the waiting screen would only be in the way of.
    prv_clear();
    return;
  }
  s_slot = st.slot;
  s_deadline = (time_t)st.deadline;
  s_window_start = smart_window_start(a, s_deadline);
  smart_window_show(s_slot, s_deadline);
  smart_tick();
}

void smart_tick(void) {
  if (!smart_is_open()) return;
  const Alarm *a = store_get(s_slot);
  if (!smart_wanted(a)) {
    // The setting changed (or the alarm was turned off/deleted) mid-window: stand
    // down gracefully and let the independently-scheduled deadline wakeup, if any,
    // handle it.
    smart_close();
    return;
  }
  time_t now = time(NULL);
  if (now >= s_deadline) {
    // The race with the independently-scheduled deadline wakeup: ring now rather than
    // rely on a wakeup that may already have been missed by a few seconds.
    int slot = s_slot;
    smart_close();
    ring_show(1 << (slot - 1));
    return;
  }

  static SleepMinute s_samples[SE_MAX_SAMPLES];
  HistoryRead hr = hr_read_night(s_samples, SE_MAX_SAMPLES, s_window_start);
  if (!hr.available) {
    return;   // no usable health data right now; keep waiting for the deadline
  }

  uint8_t percentile, required_minutes;
  prv_sens_params((SmartSensitivity)a->smart_sensitivity, &percentile, &required_minutes);
  SleepEvalCfg cfg;
  se_default_cfg(&cfg, percentile, required_minutes);
  SleepEvalResult r = se_evaluate(s_samples, hr.count, hr.window_start, hr.is_restful, &cfg);
  if (r.fire) {
    int slot = s_slot;
    time_t deadline = s_deadline;
    store_set_served_at(slot, deadline);   // the hard deadline must not ring this again
    smart_close();
    ring_show(1 << (slot - 1));
  }
}

void smart_close(void) {
  if (!smart_is_open()) return;
  prv_clear();
  smart_window_hide();
  scheduler_update();   // drops the re-entry wakeup now that nothing is open
}
