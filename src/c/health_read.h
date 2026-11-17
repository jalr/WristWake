#pragma once
// Reads Pebble Health's per-minute movement history for the smart alarm.
// Adapted from Tuomas Airaksinen's (github.com/tuomas2) PebbleSykeroAlarm,
// https://github.com/Sykero-Software/PebbleSykeroAlarm -- health_read.c.
#include "sleep_eval.h"
#include <time.h>

typedef struct {
  int    count;
  int    window_start;
  bool   is_restful;
  bool   available;
  time_t first_utc;
} HistoryRead;

// Fill out[0..max) with per-minute movement history from sleep onset (or 8 h back,
// whichever is later) up to now, and report where the alarm window begins within it.
//
// Reads the firmware's own per-minute VMC via health_service_get_minute_history. No
// accelerometer subscription: the firmware already samples at 25 Hz for activity
// tracking, so this data costs no extra sensor power.
//
// Returns available=false when health cannot be used at all -- activity tracking
// switched off, or no permission.
HistoryRead hr_read_night(SleepMinute *out, int max, time_t window_start_utc);
