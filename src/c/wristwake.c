#include <pebble.h>
#include "alarm_store.h"
#include "alarm_list.h"
#include "scheduler.h"
#include "ring.h"
#include "comms.h"
#include "theme.h"
#include "ui_common.h"
#include "i18n.h"
#include "symbols.h"
#include "smart_alarm.h"
#include "smart_window.h"

bool g_lang_de = false;

// After every alarm change: reschedule and send the state to the phone
static void prv_store_changed(void) {
  scheduler_update();
  if (scheduler_failed_mask()) {
    ui_show_message(TR("An alarm could not be scheduled.", "Ein Wecker konnte nicht geplant werden."));
  }
  comms_send_state();
}

// New theme (from the phone or day/night switch): rebuild the UI (not while an alarm
// is ringing or a smart window is being watched)
static void prv_theme_changed(void) {
  if (ring_is_active() || smart_window_is_active()) return;
  window_stack_pop_all(false);
  alarm_list_show();
}

// Every minute: switch between the day and night scheme when its time has come, and
// drive the smart alarm (both the evaluation and its waiting screen's clock) -- this
// is the only per-minute tick the app subscribes, so anything that needs one goes here.
static void prv_minute_tick(struct tm *tick_time, TimeUnits changed) {
  if (theme_update()) prv_theme_changed();
  smart_tick();
  smart_window_refresh();
}

// A smart window's slots that are due to start, right now, while the app is already
// running. Tries to open monitoring for each (the first wins; see smart_try_open).
static void prv_open_windows(uint8_t mask) {
  for (int slot = 1; slot <= ALARM_SLOTS; slot++) {
    if (!(mask & (1 << (slot - 1)))) continue;
    time_t deadline = scheduler_next_time(slot);
    if (deadline) smart_try_open(slot, deadline);
  }
}

// A wakeup fires while the app is open: either an alarm rings, a smart window starts,
// or the smart-window re-entry check runs.
static void prv_wakeup(WakeupId id, int32_t cookie) {
  if (cookie == SCHED_REENTRY_COOKIE) {
    smart_resume_if_open(false);
    scheduler_update();
    return;
  }
  scheduler_update();
  if (cookie & SCHED_WINDOW_FLAG) {
    prv_open_windows(cookie & SCHED_SLOT_MASK);
    return;
  }
  ring_show(cookie & SCHED_SLOT_MASK);
}

int main(void) {
  i18n_init();
  store_init();
  theme_init();
  symbols_init();
#ifdef PREVIEW_THEME
  // For test screenshots only: build with "#define PREVIEW_THEME 1" (Dark) or 2 (Custom).
  // Note: the theme is then stored on the watch.
  ThemeBase preview = {GColorOxfordBlue, GColorWhite, GColorOrange, GColorBlack};
  theme_set_custom(0, &preview);
  theme_set(PREVIEW_THEME);
#endif
  store_set_change_handler(prv_store_changed);
  comms_init(prv_theme_changed);
  // Touch: scroll lists and menus by swiping, open items by tapping
  app_touch_navigation_enable(true);

  // Launched by an alarm? Then show the ringing screen right away. Launched by a
  // smart window's start? Try to open it. Either way, launch_reason() only tells us
  // WHY the watch started the app this time -- a smart window open from an earlier
  // launch (re-entry, or simply this being a plain manual launch while one is live)
  // is resumed unconditionally below, from persisted state, regardless of this.
  uint8_t ring_mask = 0;
  uint8_t window_mask = 0;
  if (launch_reason() == APP_LAUNCH_WAKEUP) {
    WakeupId id;
    int32_t cookie;
    if (wakeup_get_launch_event(&id, &cookie)) {
      if (cookie & SCHED_WINDOW_FLAG) {
        window_mask = cookie & SCHED_SLOT_MASK;
      } else if (cookie != SCHED_REENTRY_COOKIE) {
        ring_mask = cookie & SCHED_SLOT_MASK;
      }
    }
  }

  // Resume a smart window that was open when the app last stopped running -- unless
  // we are about to show the ringing screen instead, which the waiting screen would
  // only be in the way of.
  smart_resume_if_open(ring_mask != 0);

  // Recompute all times on every start (handles time zone and DST changes)
  scheduler_update();
  wakeup_service_subscribe(prv_wakeup);
  tick_timer_service_subscribe(MINUTE_UNIT, prv_minute_tick);

  // Safety net: "ringing" is stored, but this start is not an alarm, so the last ringing
  // ended without snooze or stop (e.g. the app was closed before the status could be saved)
  if (!ring_mask && comms_status() == ALARM_STATUS_RINGING) {
    int slot = comms_status_slot();
    if (slot >= 1 && slot <= ALARM_SLOTS) ring_end_one_time(slot);   // rings only once
    comms_set_status(ALARM_STATUS_IDLE, 0);
  }

  if (ring_mask) {
    ring_show(ring_mask);   // after snooze/stop the app closes again
  } else {
    if (window_mask) prv_open_windows(window_mask);
    // Neither of the above may have put anything on screen itself (e.g. a window
    // resumed above, or just opened, fired immediately and is already ringing) --
    // ring_is_active() catches that case too, since smart_try_open/smart_tick can
    // call ring_show() on the spot.
    if (!smart_window_is_active() && !ring_is_active()) alarm_list_show();
  }
  app_event_loop();
}
