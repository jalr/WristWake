#include "smart_window.h"
#include "alarm_store.h"
#include "theme.h"
#include "ui_common.h"
#include "i18n.h"

// Redrawn from wristwake.c's single app-wide minute tick (smart_window_refresh) --
// not its own tick_timer_service subscription, which would replace that one handler
// app-wide (PebbleOS allows exactly one) and silently stop the day/night theme switch
// for as long as this screen is open.

static Window *s_window;
static Layer *s_layer;
static int s_slot;
static time_t s_deadline;

static void prv_draw(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  const Theme *t = theme();
  graphics_context_set_fill_color(ctx, t->bg);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  time_t now = time(NULL);
  struct tm *lt = localtime(&now);
  char time_buf[8];
  const char *ampm;
  ui_format_time(time_buf, sizeof(time_buf), lt->tm_hour, lt->tm_min, false, &ampm);
  graphics_context_set_text_color(ctx, t->fg);
  graphics_draw_text(ctx, time_buf, fonts_get_system_font(FONT_KEY_LECO_42_NUMBERS),
                     GRect(0, 18, b.size.w, 50), GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  if (ampm) {
    graphics_draw_text(ctx, ampm, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                       GRect(0, 68, b.size.w, 22), GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  }

  graphics_context_set_text_color(ctx, t->dim);
  graphics_draw_text(ctx, TR("Watching for\nlight sleep", "Beobachtet\nleichten Schlaf"),
                     fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                     GRect(8, 98, b.size.w - 16, 56), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  const Alarm *a = store_get(s_slot);
  if (a && a->used && a->name[0]) {
    graphics_draw_text(ctx, a->name, fonts_get_system_font(FONT_KEY_GOTHIC_18),
                       GRect(8, 154, b.size.w - 16, 24), GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentCenter, NULL);
  }

  struct tm *dtm = localtime(&s_deadline);
  char dt[8];
  const char *dampm;
  ui_format_time(dt, sizeof(dt), dtm->tm_hour, dtm->tm_min, true, &dampm);
  char rings[40];
  snprintf(rings, sizeof(rings), "%s %s%s%s", TR("Rings by", "Klingelt bis"), dt,
           dampm ? " " : "", dampm ? dampm : "");
  graphics_context_set_text_color(ctx, t->fg);
  graphics_draw_text(ctx, rings, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(8, b.size.h - 34, b.size.w - 16, 28), GTextOverflowModeFill,
                     GTextAlignmentCenter, NULL);
}

// No reaction to any button: a stray press in your sleep must not be able to end
// monitoring or dismiss the screen (same reasoning as ring.c's ringing screen).
static void prv_no_click(ClickRecognizerRef r, void *c) {}

static void prv_clicks(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, prv_no_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_no_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_no_click);
  window_single_click_subscribe(BUTTON_ID_BACK, prv_no_click);
}

static void prv_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_layer, prv_draw);
  layer_add_child(root, s_layer);
}

static void prv_unload(Window *window) {
  layer_destroy(s_layer);
  s_layer = NULL;
  window_destroy(window);
  s_window = NULL;
}

void smart_window_show(int slot, time_t deadline) {
  s_slot = slot;
  s_deadline = deadline;
  if (s_window) {
    if (s_layer) layer_mark_dirty(s_layer);
    return;
  }
  s_window = window_create();
  window_set_background_color(s_window, theme()->bg);
  window_set_click_config_provider(s_window, prv_clicks);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_load,
    .unload = prv_unload,
  });
  window_stack_push(s_window, true);
}

void smart_window_hide(void) {
  if (s_window) window_stack_remove(s_window, false);
}

bool smart_window_is_active(void) {
  return s_window != NULL;
}

void smart_window_refresh(void) {
  if (s_layer) layer_mark_dirty(s_layer);
}
