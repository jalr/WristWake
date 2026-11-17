#pragma once
// Smart alarm: decide whether the current minute is a good moment to ring, from the
// watch's own per-minute movement history. Adapted from Tuomas Airaksinen's
// (github.com/tuomas2) PebbleSykeroAlarm, https://github.com/Sykero-Software/PebbleSykeroAlarm
// -- sleep_eval.c's sleep-phase detection algorithm, simplified for WristWake's
// per-alarm settings.
#include <pebble.h>

// 12 h of per-minute samples is the most this module will look at.
#define SE_MAX_SAMPLES 720
// Below this many usable minutes there is no distribution worth taking a percentile of,
// so the smart alarm stands down and the deadline rings instead. Applies to the
// pre-window history first; if that alone is too thin, the whole recorded stretch
// (history + window) is retried before giving up -- a short pre-window gap is not the
// same thing as "not enough sleep data".
#define SE_MIN_USABLE  60

// One minute of the firmware's own movement history, reduced to what the algorithm needs.
typedef struct {
  uint16_t vmc;          // Vector Magnitude Counts for the minute
  uint8_t  orientation;  // yaw in the low nibble, pitch in the high nibble
  bool     is_invalid;
} SleepMinute;

typedef struct {
  uint8_t  percentile;        // 70..99; the trigger level is this percentile of
                               // the night's own vmc distribution
  uint8_t  required_minutes;  // 1..5 consecutive contributing minutes
  uint16_t settle_minutes;    // minutes after onset excluded from the baseline
  uint16_t min_margin;        // the trigger level is at least baseline + this
  uint16_t orient_bonus;      // added to the accumulator on a turn-over
  uint8_t  orient_step;       // a nibble must move at least this many of the 16
                               // steps to count as a turn-over (ignores jitter)
} SleepEvalCfg;

typedef struct {
  bool     fire;
  int      fired_index;       // index into samples, -1 when not firing
  uint16_t baseline;          // median vmc of the ranking population
  uint16_t trigger_level;
  uint32_t acc;                // accumulator value at the fire point (or final)
  bool     insufficient_data;
} SleepEvalResult;

// Fill *out with the standard configuration for a given sensitivity.
void se_default_cfg(SleepEvalCfg *out, uint8_t percentile, uint8_t required_minutes);

// ---------------------------------------------------------------- sleep spans
//
// One sleep session as the firmware reports it, in epoch seconds.
typedef struct {
  uint32_t start;
  uint32_t end;
} SleepSpan;

// The most sleep sessions one night is expected to produce.
#define SE_MAX_SESSIONS 8
// Two sessions this far apart or closer are the same night with a wake episode
// between them, not two separate sleeps.
#define SE_SESSION_MERGE_GAP_S (90u * 60u)

// Merge sleep sessions belonging to the same night and report the awake stretches
// between them.
//
// `spans` is NEWEST-FIRST, exactly as health_service_activities_iterate delivers
// with HealthIterationDirectionPast. Returns the merged onset (the oldest merged
// session's start), or 0 when there is nothing to merge. Every gap merged ACROSS is
// appended to gaps[] and *n_gaps -- the caller must feed those to se_mark_awake.
uint32_t se_merge_sessions(const SleepSpan *spans, int n, uint32_t max_gap_s,
                           SleepSpan *gaps, int max_gaps, int *n_gaps);

// Mark every minute overlapping one of `gaps` invalid, so the ranking population
// excludes it. `first_utc` is the timestamp of samples[0].
void se_mark_awake(SleepMinute *samples, int count, uint32_t first_utc,
                   const SleepSpan *gaps, int n_gaps);

// Decide whether to ring now.
//
// samples[0..count) is the night from sleep onset onwards, oldest first.
// window_start is the first index belonging to the alarm window. is_restful is the
// firmware's current sleep state (HealthActivityRestfulSleep), passed in rather than
// read here so this module stays pure.
//
// The accumulator is leaky by construction: subtracting the trigger level every
// minute is what drains it at rest, so no separate decay constant is needed. A single
// spike, however large, cannot fire -- the duration test requires required_minutes
// consecutive contributing minutes.
SleepEvalResult se_evaluate(const SleepMinute *samples, int count, int window_start,
                            bool is_restful, const SleepEvalCfg *cfg);

// Index of the first minute of a quiet stretch at least quiet_minutes long in which
// vmc never exceeds awake_above. Used only when the firmware's own sleep session is
// unavailable (it needs 60 minutes before it reports one). -1 if there is no such
// stretch.
int se_find_onset(const SleepMinute *samples, int count, uint16_t awake_above,
                  int quiet_minutes);
