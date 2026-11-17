#pragma once
// Waiting screen while a smart alarm's window is open: dark background, light text --
// glanced at in the dark the same way the ringing screen is. BACK does nothing, same
// reasoning as ring.c: it must not be possible to cancel by a stray press in your sleep.
#include <pebble.h>

void smart_window_show(int slot, time_t deadline);
void smart_window_hide(void);
bool smart_window_is_active(void);

// Redraws the clock if the screen is currently shown; no-op otherwise. Called from
// wristwake.c's single app-wide minute tick -- see the note in smart_window.c for why
// this screen does not subscribe its own.
void smart_window_refresh(void);
