#include "sleep_eval.h"
#include <stddef.h>

// A contiguous run of at least this many minutes above the wake threshold is treated
// as a wake episode (arousal), not ordinary sleep movement, and is excluded from the
// ranking population wholesale. Matches PebbleSykeroAlarm's measured value (longest
// naturally occurring elevated run over 200 simulated nights was 7 minutes).
#define SE_WAKE_RUN_MINUTES 8
// A minute is "elevated" (a candidate wake-episode minute) once its vmc exceeds this
// multiple of the resting median plus the configured margin.
#define SE_WAKE_THRESHOLD_MULT 4

void se_default_cfg(SleepEvalCfg *out, uint8_t percentile, uint8_t required_minutes) {
  if (out == NULL) {
    return;
  }
  if (percentile < 70) percentile = 70;
  if (percentile > 99) percentile = 99;
  if (required_minutes < 1) required_minutes = 1;
  if (required_minutes > 5) required_minutes = 5;
  out->percentile = percentile;
  out->required_minutes = required_minutes;
  out->settle_minutes = 20;
  out->min_margin = 25;
  out->orient_bonus = 400;
  out->orient_step = 2;
}

uint32_t se_merge_sessions(const SleepSpan *spans, int n, uint32_t max_gap_s,
                           SleepSpan *gaps, int max_gaps, int *n_gaps) {
  if (n_gaps != NULL) {
    *n_gaps = 0;
  }
  if (spans == NULL || n <= 0) {
    return 0;
  }
  // The contract is NEWEST-FIRST. A violated contract must not be guessed at: walking
  // an oldest-first list would take spans[0] (the oldest) as the onset and then absorb
  // every later span as an "overlap", returning the whole night with NO gaps recorded.
  int first = -1;
  uint32_t prev_end = 0;
  for (int i = 0; i < n; i++) {
    if (spans[i].end <= spans[i].start) {
      continue;
    }
    if (first < 0) {
      first = i;
    } else if (spans[i].end > prev_end) {
      // Not newest-first. Do not guess: the newest session alone is the safe answer.
      uint32_t newest = 0;
      for (int k = 0; k < n; k++) {
        if (spans[k].end > spans[k].start && spans[k].start > newest) {
          newest = spans[k].start;
        }
      }
      return newest;
    }
    prev_end = spans[i].end;
  }
  if (first < 0) {
    return 0;   // nothing well-formed to anchor on
  }
  uint32_t onset = spans[first].start;
  for (int i = first + 1; i < n; i++) {
    if (spans[i].end <= spans[i].start) {
      continue;   // malformed span: ignore rather than let it end the night
    }
    if (spans[i].end >= onset) {
      // Overlapping, nested, or adjacent with no gap at all: absorb it, no awake stretch.
      if (spans[i].start < onset) {
        onset = spans[i].start;
      }
      continue;
    }
    uint32_t gap = onset - spans[i].end;
    if (gap > max_gap_s) {
      break;   // a real break between two nights (or a nap): stop here
    }
    // The gap MUST be reportable before merging across it, or the awake minutes would
    // get pulled into the ranking population.
    if (gaps == NULL || n_gaps == NULL || *n_gaps >= max_gaps) {
      break;
    }
    gaps[*n_gaps].start = spans[i].end;
    gaps[*n_gaps].end = onset;
    (*n_gaps)++;
    onset = spans[i].start;
  }
  return onset;
}

void se_mark_awake(SleepMinute *samples, int count, uint32_t first_utc,
                   const SleepSpan *gaps, int n_gaps) {
  if (samples == NULL || gaps == NULL || count <= 0) {
    return;
  }
  for (int gi = 0; gi < n_gaps; gi++) {
    uint32_t gs = gaps[gi].start, ge = gaps[gi].end;
    if (ge <= gs) {
      continue;
    }
    for (int i = 0; i < count; i++) {
      uint32_t ms = first_utc + (uint32_t)i * 60u;
      if (ms < ge && ms + 60u > gs) {
        samples[i].is_invalid = true;
      }
    }
  }
}

// Sorting scratch. Static, not a local: 720 uint16 is 1440 bytes against a ~2 KB app
// stack. The event loop is single-threaded so a non-reentrant buffer is safe.
static uint16_t s_sorted[SE_MAX_SAMPLES];

// Shell sort. Hand-rolled rather than qsort(): the safe-to-call libc surface on the
// firmware is narrow and qsort is not part of it.
static void prv_sort(uint16_t *a, int n) {
  for (int gap = n / 2; gap > 0; gap /= 2) {
    for (int i = gap; i < n; i++) {
      uint16_t v = a[i];
      int j = i;
      while (j >= gap && a[j - gap] > v) {
        a[j] = a[j - gap];
        j -= gap;
      }
      a[j] = v;
    }
  }
}

static uint8_t prv_nibble_dist(uint8_t a, uint8_t b) {
  // Both nibbles are a 16-step wrap-around quantisation, so the distance is circular.
  int d = (int)a - (int)b;
  if (d < 0) d = -d;
  if (d > 8) d = 16 - d;
  return (uint8_t)d;
}

static bool prv_orientation_changed(uint8_t cur, uint8_t ref, uint8_t step) {
  uint8_t cy = cur & 0x0F, cp = (cur >> 4) & 0x0F;
  uint8_t ry = ref & 0x0F, rp = (ref >> 4) & 0x0F;
  return prv_nibble_dist(cy, ry) >= step || prv_nibble_dist(cp, rp) >= step;
}

// Most common orientation over samples[from,to), or the last valid one if there is no
// clear mode. 256 buckets is small enough to count directly.
static uint8_t prv_orientation_mode(const SleepMinute *s, int from, int to) {
  static uint8_t counts[256];
  for (int i = 0; i < 256; i++) {
    counts[i] = 0;
  }
  uint8_t best = 0;
  int best_n = -1;
  for (int i = from; i < to; i++) {
    if (s[i].is_invalid) {
      continue;
    }
    uint8_t o = s[i].orientation;
    if (counts[o] < 255) {
      counts[o]++;
    }
    if ((int)counts[o] > best_n) {
      best_n = counts[o];
      best = o;
    }
  }
  return best;
}

// Fills s_sorted[0..n) with every valid vmc in samples[from,to), capped at
// SE_MAX_SAMPLES, and returns n.
static int prv_fill(const SleepMinute *samples, int from, int to) {
  int n = 0;
  for (int i = from; i < to; i++) {
    if (!samples[i].is_invalid && n < SE_MAX_SAMPLES) {
      s_sorted[n++] = samples[i].vmc;
    }
  }
  return n;
}

SleepEvalResult se_evaluate(const SleepMinute *samples, int count, int window_start,
                            bool is_restful, const SleepEvalCfg *cfg) {
  SleepEvalResult r = {0};
  r.fired_index = -1;
  r.insufficient_data = true;

  if (samples == NULL || cfg == NULL || count <= 0) {
    return r;
  }
  if (count > SE_MAX_SAMPLES) {
    // Keep the most recent samples: the tail is the night we care about.
    samples += (count - SE_MAX_SAMPLES);
    window_start -= (count - SE_MAX_SAMPLES);
    count = SE_MAX_SAMPLES;
  }
  if (window_start < 0) {
    window_start = 0;
  }
  if (window_start >= count) {
    // The window has not started yet; nothing to judge.
    return r;
  }

  // Collect the sleeping minutes, excluding the settling-in period: the falling-asleep
  // stretch is not representative of the night.
  int from = (int)cfg->settle_minutes;
  if (from >= count) {
    from = 0;
  }

  // The ranking population is the HISTORY strictly before the alarm window, not
  // "from..count" (which would include the window itself) -- judging the window's
  // own anomalousness against a population that already contains it is circular.
  // Falls back to from..count when the pre-window history alone is too thin.
  int pop_end = (window_start > from) ? window_start : count;

  int n = prv_fill(samples, from, pop_end);
  if (n < SE_MIN_USABLE && pop_end != count) {
    pop_end = count;
    n = prv_fill(samples, from, pop_end);
  }
  if (n < SE_MIN_USABLE) {
    return r;
  }
  r.insufficient_data = false;

  // baseline is the median of the population BEFORE wake-episode exclusion (below):
  // the exclusion threshold is derived from this baseline, so it has to come first.
  prv_sort(s_sorted, n);
  r.baseline = s_sorted[n / 2];

  // Exclude WAKE EPISODES from the ranking population: contiguous runs of at least
  // SE_WAKE_RUN_MINUTES consecutive minutes whose vmc exceeds
  // SE_WAKE_THRESHOLD_MULT x baseline + min_margin. By RUN LENGTH, not distribution
  // shape, so an ordinary right-skewed night is left untouched.
  //
  // An invalid minute is BRIDGED, not treated as a run break: it is already absent
  // from the population either way, so letting it pass through a run in progress is
  // strictly safer than breaking on it.
  uint32_t wake_threshold = (uint32_t)r.baseline * SE_WAKE_THRESHOLD_MULT + cfg->min_margin;
  int n_trim = 0;
  int run_start = -1;
  for (int i = from; i <= pop_end; i++) {
    if (i < pop_end && samples[i].is_invalid) {
      continue;   // bridge: neither extends nor breaks a run
    }
    bool elevated = (i < pop_end) && samples[i].vmc > wake_threshold;
    if (elevated) {
      if (run_start < 0) {
        run_start = i;
      }
      continue;
    }
    if (run_start >= 0) {
      int run_len = i - run_start;
      if (run_len < SE_WAKE_RUN_MINUTES) {
        // Too short to be a wake episode -- these are ordinary population samples.
        for (int j = run_start; j < i && n_trim < SE_MAX_SAMPLES; j++) {
          if (!samples[j].is_invalid) {
            s_sorted[n_trim++] = samples[j].vmc;
          }
        }
      }
      // else: a genuine wake episode -- excluded entirely.
      run_start = -1;
    }
    if (i < pop_end && n_trim < SE_MAX_SAMPLES) {
      s_sorted[n_trim++] = samples[i].vmc;   // valid (checked above) and non-elevated
    }
  }

  // If excluding wake episodes left too little to rank confidently, fall back to the
  // unfiltered population.
  if (n_trim < SE_MIN_USABLE) {
    n_trim = prv_fill(samples, from, pop_end);
  }

  prv_sort(s_sorted, n_trim);
  int pi = (int)(((uint32_t)n_trim * cfg->percentile) / 100u);
  uint32_t level = s_sorted[pi];
  uint32_t floor_level = (uint32_t)r.baseline + cfg->min_margin;
  if (level < floor_level) {
    level = floor_level;
  }
  if (level > 0xFFFF) {
    level = 0xFFFF;
  }
  r.trigger_level = (uint16_t)level;

  uint32_t acc = 0;
  int contributing = 0;
  for (int i = window_start; i < count; i++) {
    if (samples[i].is_invalid) {
      acc = 0;
      contributing = 0;
      continue;
    }
    // The orientation reference is the mode of the ten minutes IMMEDIATELY BEFORE this
    // one -- ROLLING, recomputed per window minute, so a posture that has SETTLED
    // stops contributing once the mode catches up, instead of scoring the bonus for
    // every remaining minute of the window.
    int ref_from = i - 10;
    if (ref_from < 0) {
      ref_from = 0;
    }
    uint8_t ref_orient = prv_orientation_mode(samples, ref_from,
                                              i > ref_from ? i : ref_from + 1);

    bool contributed = false;
    if (samples[i].vmc > r.trigger_level) {
      acc += (uint32_t)(samples[i].vmc - r.trigger_level);
      contributed = true;
    }
    if (prv_orientation_changed(samples[i].orientation, ref_orient, cfg->orient_step)) {
      acc += cfg->orient_bonus;
      contributed = true;
    }
    if (!contributed) {
      acc = 0;
      contributing = 0;
      continue;
    }
    contributing++;
    // Magnitude AND duration: acc >= trigger_level means a full trigger-level minute
    // of excess has built up; contributing >= required_minutes means it was sustained.
    if (acc >= r.trigger_level && contributing >= (int)cfg->required_minutes) {
      r.acc = acc;
      r.fired_index = i;
      r.fire = !is_restful;   // deep sleep vetoes an early wake
      return r;
    }
  }
  r.acc = acc;
  return r;
}

int se_find_onset(const SleepMinute *samples, int count, uint16_t awake_above,
                  int quiet_minutes) {
  if (samples == NULL || count <= 0 || quiet_minutes <= 0) {
    return -1;
  }
  int run = 0;
  for (int i = 0; i < count; i++) {
    if (!samples[i].is_invalid && samples[i].vmc <= awake_above) {
      run++;
      if (run >= quiet_minutes) {
        return i - run + 1;
      }
    } else {
      run = 0;
    }
  }
  return -1;
}
