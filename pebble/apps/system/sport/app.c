/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "engine.h"
#include "fields.h"
#include "profile.h"
#include "recorder.h"
#include "sensors.h"

#include <stdio.h>
#include <string.h>

#include <pbl/kernel/time.h>
#include <pbl/logging/logging.h>
#include <pbl/services/activity/hr_util.h>
#include <pbl/services/activity/workout_service.h>
#include <pbl/services/clock.h>
#include <pbl/services/time.h>
#include <pbl/util/math.h>
#include <pbl/util/size.h>

#include <applib/app.h>
#include <applib/app_light.h>
#include <applib/app_malloc.h>
#include <applib/tick_timer_service.h>
#include <applib/ui/ui.h>
#include <applib/ui/vibes.h>
#include <shell/prefs.h>

#define COUNTDOWN_S  3
#define LAP_POPUP_MS 5000
#define STATUS_H     22
// Round displays: keep everything inside a centred circle this wide.
#define ROUND_SAFE_DIAMETER 220
#define FIELD_LABEL_H       18
// Round displays: the status line starts where the circle is wide enough for it.
#define STATUS_TOP PBL_IF_ROUND_ELSE(16, 0)

typedef enum SportPhase {
  SportPhase_Ready,
  SportPhase_Countdown,
  SportPhase_Active,
  SportPhase_Done,
} SportPhase;

typedef struct SportApp {
  const SportProfile *profile;
  SportEngine engine;
  SportFieldContext fields;
  SportPhase phase;
  uint8_t countdown;
  uint8_t screen;
  bool start_anyway;
  bool had_fix;
  bool workout;
  bool saved;

  Window *ready_window;
  Layer *ready_layer;
  Window *active_window;
  Layer *active_layer;
  Window *pause_window;
  MenuLayer *pause_menu;
  Window *summary_window;
  MenuLayer *summary_menu;

  AppTimer *popup_timer;
  SportLap popup_lap;
} SportApp;

static SportApp *s_app;

static GColor prv_accent(void) {
  return PBL_IF_COLOR_ELSE((GColor){.argb = s_app->profile->accent_argb8}, GColorWhite);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Layout

// Area the content may use: the screen, or the centred circle on round displays.
static GRect prv_safe_rect(GRect bounds) {
#if PBL_ROUND
  const int16_t d = PBL_MIN(ROUND_SAFE_DIAMETER, PBL_MIN(bounds.size.w, bounds.size.h));
  return GRect(bounds.origin.x + (bounds.size.w - d) / 2, bounds.origin.y + (bounds.size.h - d) / 2,
               d, d);
#else
  return bounds;
#endif
}

// Width available for a band of the safe area between y0 and y1.
static GRect prv_band(GRect safe, int16_t y0, int16_t y1) {
#if PBL_ROUND
  const int32_t r = safe.size.w / 2;
  const int32_t cy = safe.origin.y + r;
  const int32_t dy = PBL_MAX(PBL_ABS(y0 - cy), PBL_ABS(y1 - cy));
  const int32_t half = dy >= r ? 0 : integer_sqrt(r * r - dy * dy);
  const int16_t x = (int16_t)(safe.origin.x + r - half);
  return GRect(x, y0, (int16_t)(2 * half), y1 - y0);
#else
  return GRect(safe.origin.x, y0, safe.size.w, y1 - y0);
#endif
}

static const char *const s_value_fonts[] = {
  FONT_KEY_LECO_60_NUMBERS_AM_PM,      FONT_KEY_LECO_42_NUMBERS,
  FONT_KEY_LECO_36_BOLD_NUMBERS,       FONT_KEY_LECO_32_BOLD_NUMBERS,
  FONT_KEY_LECO_26_BOLD_NUMBERS_AM_PM, FONT_KEY_LECO_20_BOLD_NUMBERS,
};

// Draw a value with the largest font that fits the box.
static void prv_draw_value(GContext *ctx, const char *text, GRect box) {
  GFont font = NULL;
  int16_t h = 0;

  for (size_t i = 0; i < PBL_ARRAY_SIZE(s_value_fonts); i++) {
    font = fonts_get_system_font(s_value_fonts[i]);
    h = (int16_t)fonts_get_font_height(font);
    const GSize size = graphics_text_layout_get_max_used_size(
        ctx, text, font, GRect(0, 0, 1000, 200), GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    if (h <= box.size.h && size.w <= box.size.w - 4) {
      break;
    }
  }

  // LECO glyphs sit low in their line: centre on the digits, not the line.
  const int16_t y = (int16_t)(box.origin.y + (box.size.h - h) / 2 - h / 8);
  graphics_draw_text(ctx, text, font, GRect(box.origin.x, y, box.size.w, h + 8),
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

static void prv_draw_field(GContext *ctx, SportMetric metric, GRect band) {
  char label[24];
  char value[16];
  const char *unit = sport_metric_unit(metric, s_app->fields.unit_mm);

  snprintf(label, sizeof(label), "%s%s%s", sport_metric_label(metric), unit[0] ? " " : "", unit);
  sport_metric_format(metric, &s_app->fields, value, sizeof(value));

  graphics_context_set_text_color(ctx, prv_accent());
  graphics_draw_text(ctx, label, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(band.origin.x, band.origin.y - 2, band.size.w, FIELD_LABEL_H),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  graphics_context_set_text_color(ctx, GColorWhite);
  prv_draw_value(ctx, value,
                 GRect(band.origin.x, band.origin.y + FIELD_LABEL_H - 4, band.size.w,
                       band.size.h - FIELD_LABEL_H + 4));
}

static void prv_draw_status(GContext *ctx, GRect safe) {
  const SportEngine *e = &s_app->engine;
  const SportSensorsStatus st = sport_sensors_get_status();
  const GRect band =
      prv_band(safe, safe.origin.y + STATUS_TOP, safe.origin.y + STATUS_TOP + STATUS_H);
  char text[24];
  GColor bg = GColorClear;

  if (e->state == SportState_AutoPaused) {
    snprintf(text, sizeof(text), "AUTO PAUSE");
    bg = PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorWhite);
  } else if (e->state == SportState_Paused) {
    snprintf(text, sizeof(text), "PAUSED");
    bg = PBL_IF_COLOR_ELSE(GColorRed, GColorWhite);
  } else {
    char clock[8];
    snprintf(clock, sizeof(clock), "%u:%02u", s_app->fields.hour, s_app->fields.minute);
    snprintf(text, sizeof(text), "%s  %u/%u", clock, s_app->screen + 1,
             s_app->profile->num_screens);
  }

  if (!gcolor_equal(bg, GColorClear)) {
    graphics_context_set_fill_color(ctx, bg);
    graphics_fill_rect(ctx, &band);
    graphics_context_set_text_color(ctx, GColorBlack);
  } else {
    graphics_context_set_text_color(ctx, GColorLightGray);
  }
  graphics_draw_text(ctx, text, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(band.origin.x, band.origin.y - 3, band.size.w, STATUS_H),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  // GNSS: green with a fix, red while searching.
  if (st.gnss != SportGnssStatus_None && gcolor_equal(bg, GColorClear)) {
    graphics_context_set_fill_color(ctx, st.gnss == SportGnssStatus_Fix
                                             ? PBL_IF_COLOR_ELSE(GColorGreen, GColorWhite)
                                             : PBL_IF_COLOR_ELSE(GColorRed, GColorDarkGray));
    // Bottom centre on round displays, where the status line has no room for it.
    const GPoint dot =
        PBL_IF_ROUND_ELSE(GPoint(safe.origin.x + safe.size.w / 2, safe.origin.y + safe.size.h - 6),
                          GPoint(band.origin.x + 10, band.origin.y + 10));
    graphics_fill_circle(ctx, dot, 4);
  }
}

static void prv_draw_lap_popup(GContext *ctx, GRect safe) {
  const SportStats *lap = &s_app->popup_lap.stats;
  const uint32_t unit = s_app->fields.unit_mm;
  const GRect box = grect_inset(
      prv_band(safe, safe.origin.y + safe.size.h / 2 - 56, safe.origin.y + safe.size.h / 2 + 56),
      GEdgeInsets(0, 6));
  char title[16];
  char time[16];
  char rate[16];
  char line[48];

  graphics_context_set_fill_color(ctx, prv_accent());
  graphics_fill_rect(ctx, &box);
  graphics_context_set_text_color(ctx, GColorBlack);

  snprintf(title, sizeof(title), "LAP %u", s_app->popup_lap.number);
  graphics_draw_text(ctx, title, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                     GRect(box.origin.x, box.origin.y, box.size.w, 28), GTextOverflowModeFill,
                     GTextAlignmentCenter, NULL);

  sport_format_duration(lap->timer_s, time, sizeof(time));
  graphics_draw_text(ctx, time, fonts_get_system_font(FONT_KEY_LECO_32_BOLD_NUMBERS),
                     GRect(box.origin.x, box.origin.y + 28, box.size.w, 40), GTextOverflowModeFill,
                     GTextAlignmentCenter, NULL);

  if (s_app->profile->pace) {
    sport_format_pace(sport_pace_s(sport_stats_avg_speed_mm_s(lap), unit), rate, sizeof(rate));
  } else {
    sport_format_speed(sport_stats_avg_speed_mm_s(lap), unit, rate, sizeof(rate));
  }
  char dist[16];
  sport_format_distance(lap->distance_mm, unit, dist, sizeof(dist));
  snprintf(line, sizeof(line), "%s %s  %s %s", dist, sport_metric_unit(SportMetric_Distance, unit),
           rate,
           sport_metric_unit(s_app->profile->pace ? SportMetric_Pace : SportMetric_Speed, unit));
  graphics_draw_text(ctx, line, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(box.origin.x, box.origin.y + 72, box.size.w, 24),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

static void prv_active_update_proc(Layer *layer, GContext *ctx) {
  const GRect bounds = layer_get_bounds_by_value(layer);
  const GRect safe = prv_safe_rect(bounds);
  const SportScreen *screen = &s_app->profile->screens[s_app->screen];
  const int16_t top = (int16_t)(safe.origin.y + STATUS_TOP + STATUS_H);
  const int16_t bottom = (int16_t)(safe.origin.y + safe.size.h - PBL_IF_ROUND_ELSE(14, 2));
  const int16_t row_h = (int16_t)((bottom - top) / screen->num_fields);

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, &bounds);

  prv_draw_status(ctx, safe);

  for (size_t i = 0; i < screen->num_fields; i++) {
    const int16_t y0 = (int16_t)(top + row_h * i);
    const GRect band = prv_band(safe, y0, (int16_t)(y0 + row_h));

    if (i > 0) {
      graphics_context_set_stroke_color(ctx, GColorDarkGray);
      graphics_draw_line(ctx, GPoint(band.origin.x + 8, y0),
                         GPoint(band.origin.x + band.size.w - 8, y0));
    }
    prv_draw_field(ctx, screen->fields[i], band);
  }

  if (s_app->popup_timer != NULL) {
    prv_draw_lap_popup(ctx, safe);
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Ready screen: wait for GPS, then start

static void prv_ready_update_proc(Layer *layer, GContext *ctx) {
  const GRect bounds = layer_get_bounds_by_value(layer);
  const GRect safe = prv_safe_rect(bounds);
  const SportSensorsStatus st = sport_sensors_get_status();
  const int16_t cy = (int16_t)(safe.origin.y + safe.size.h / 2);
  char line[32];
  const char *hint;

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, &bounds);

  graphics_context_set_text_color(ctx, prv_accent());
  graphics_draw_text(ctx, s_app->profile->name, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
                     prv_band(safe, cy - 80, cy - 46), GTextOverflowModeFill, GTextAlignmentCenter,
                     NULL);

  if (s_app->phase == SportPhase_Countdown) {
    snprintf(line, sizeof(line), "%u", s_app->countdown);
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, line, fonts_get_system_font(FONT_KEY_LECO_60_NUMBERS_AM_PM),
                       prv_band(safe, cy - 40, cy + 30), GTextOverflowModeFill,
                       GTextAlignmentCenter, NULL);
    hint = "BACK to cancel";
  } else {
    GColor gps_color = GColorLightGray;
    if (st.gnss == SportGnssStatus_Fix) {
      snprintf(line, sizeof(line), "GPS ready");
      gps_color = PBL_IF_COLOR_ELSE(GColorGreen, GColorWhite);
    } else if (st.gnss == SportGnssStatus_Searching) {
      snprintf(line, sizeof(line), "Locating GPS... %u", st.sats);
      gps_color = PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorWhite);
    } else if (s_app->profile->sensors & SportSensor_Gnss) {
      snprintf(line, sizeof(line), "No GPS");
    } else {
      line[0] = '\0';
    }
    graphics_context_set_text_color(ctx, gps_color);
    graphics_draw_text(ctx, line, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                       prv_band(safe, cy - 30, cy), GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentCenter, NULL);

    graphics_context_set_text_color(ctx, GColorLightGray);
    graphics_draw_text(ctx, st.baro ? "Barometer on" : "GPS elevation",
                       fonts_get_system_font(FONT_KEY_GOTHIC_18), prv_band(safe, cy + 2, cy + 26),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

    if (st.gnss == SportGnssStatus_Searching) {
      hint = s_app->start_anyway ? "SELECT again: start\nwithout GPS" : "SELECT: start anyway";
    } else {
      hint = "SELECT: start\nHold: no countdown";
    }
  }

  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, hint, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     prv_band(safe, cy + 32, cy + 80), GTextOverflowModeWordWrap,
                     GTextAlignmentCenter, NULL);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Activity control

static void prv_push_active_window(void);
static void prv_push_pause_menu(void);
static void prv_push_summary(void);

static void prv_start_activity(void) {
  const SportProfile *p = s_app->profile;

  s_app->phase = SportPhase_Active;
  sport_engine_start(&s_app->engine);

  s_app->workout = workout_service_start_workout(p->session_type);
  if (!s_app->workout) {
    PBL_LOG_WRN("workout service busy: no heart rate or steps");
  }
  sport_sensors_set_workout(s_app->workout);
  sport_recorder_start((uint8_t)p->type, pbl_realtime());

  vibes_long_pulse();
  prv_push_active_window();
  app_window_stack_remove(s_app->ready_window, false);
}

static void prv_finish(bool save) {
  if (s_app->phase != SportPhase_Active) {
    return;
  }

  sport_engine_stop(&s_app->engine);
  if (save) {
    sport_recorder_finish(&s_app->engine);
  } else {
    sport_recorder_discard();
  }
  if (s_app->workout) {
    workout_service_stop_workout();
    s_app->workout = false;
    sport_sensors_set_workout(false);
  }
  s_app->saved = save;
  s_app->phase = SportPhase_Done;
}

static void prv_set_paused(bool paused) {
  sport_engine_pause(&s_app->engine, paused);
  if (s_app->workout) {
    workout_service_pause_workout(paused);
  }
  vibes_short_pulse();
}

static void prv_popup_timer_cb(void *data) {
  s_app->popup_timer = NULL;
  if (s_app->active_layer) {
    layer_mark_dirty(s_app->active_layer);
  }
}

static void prv_show_lap(void) {
  const SportLap *lap = sport_engine_last_lap(&s_app->engine);

  if (lap == NULL) {
    return;
  }

  s_app->popup_lap = *lap;
  if (s_app->popup_timer) {
    app_timer_cancel(s_app->popup_timer);
  }
  s_app->popup_timer = app_timer_register(LAP_POPUP_MS, prv_popup_timer_cb, NULL);
  vibes_double_pulse();
  app_light_enable_interaction();
}

static void prv_update_live(const SportSample *sample, const struct pbl_tm *now) {
  SportFieldContext *f = &s_app->fields;
  const bool clock_24h = clock_is_24h_style();

  f->has_hr = sample->flags & SportSampleFlag_Hr;
  f->hr_bpm = sample->hr_bpm;
  f->hr_zone = f->has_hr ? (uint8_t)hr_util_get_hr_zone(sample->hr_bpm) : 0;
  f->has_cadence = sample->flags & SportSampleFlag_Cadence;
  f->cadence = sample->cadence;
  f->has_power = sample->flags & SportSampleFlag_Power;
  f->power_w = sample->power_w;
  f->minute = (uint8_t)now->tm_min;
  f->hour = (uint8_t)(clock_24h ? now->tm_hour : (now->tm_hour + 11) % 12 + 1);
}

static void prv_tick_handler(struct pbl_tm *tick_time, TimeUnits units_changed) {
  SportSample sample;

  sport_sensors_poll(&sample);
  prv_update_live(&sample, tick_time);

  switch (s_app->phase) {
    case SportPhase_Ready: {
      const bool fix = sport_sensors_get_status().gnss == SportGnssStatus_Fix;
      if (fix && !s_app->had_fix) {
        vibes_short_pulse();
      }
      s_app->had_fix = fix;
      layer_mark_dirty(s_app->ready_layer);
      break;
    }
    case SportPhase_Countdown:
      if (--s_app->countdown == 0) {
        prv_start_activity();
      } else {
        vibes_short_pulse();
        layer_mark_dirty(s_app->ready_layer);
      }
      break;
    case SportPhase_Active: {
      const uint32_t events = sport_engine_tick(&s_app->engine, &sample);
      SportRecord record;

      sport_record_pack(&s_app->engine, &sample, events & SportEvent_Lap, &record);
      if (s_app->workout) {
        workout_service_set_distance((int32_t)(s_app->engine.total.distance_mm / 1000));
      }
      sport_recorder_add(&record, s_app->engine.elapsed_s);

      if (events & SportEvent_Lap) {
        prv_show_lap();
      }
      if (events & (SportEvent_AutoPause | SportEvent_AutoResume)) {
        // The workout service keeps running: steps have to come in to resume on.
        vibes_short_pulse();
        PBL_LOG_DBG("auto %s", (events & SportEvent_AutoPause) ? "pause" : "resume");
      }
      if (s_app->active_layer) {
        layer_mark_dirty(s_app->active_layer);
      }
      break;
    }
    case SportPhase_Done:
      break;
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Ready window

static void prv_begin(bool countdown) {
  if (countdown) {
    s_app->phase = SportPhase_Countdown;
    s_app->countdown = COUNTDOWN_S;
    vibes_short_pulse();
    layer_mark_dirty(s_app->ready_layer);
  } else {
    prv_start_activity();
  }
}

static bool prv_may_start(void) {
  if (sport_sensors_get_status().gnss != SportGnssStatus_Searching || s_app->start_anyway) {
    return true;
  }

  s_app->start_anyway = true;
  layer_mark_dirty(s_app->ready_layer);

  return false;
}

static void prv_ready_select(ClickRecognizerRef recognizer, void *context) {
  if (s_app->phase == SportPhase_Ready && prv_may_start()) {
    prv_begin(true);
  }
}

static void prv_ready_select_long(ClickRecognizerRef recognizer, void *context) {
  if (s_app->phase == SportPhase_Ready && prv_may_start()) {
    prv_begin(false);
  } else if (s_app->phase == SportPhase_Countdown) {
    prv_start_activity();
  }
}

static void prv_ready_back(ClickRecognizerRef recognizer, void *context) {
  if (s_app->phase == SportPhase_Countdown) {
    s_app->phase = SportPhase_Ready;
    layer_mark_dirty(s_app->ready_layer);
    return;
  }
  app_window_stack_pop(true);
}

static void prv_ready_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_ready_select);
  window_long_click_subscribe(BUTTON_ID_SELECT, 0, prv_ready_select_long, NULL);
  window_single_click_subscribe(BUTTON_ID_BACK, prv_ready_back);
}

static void prv_ready_load(Window *window) {
  Layer *root = window_get_root_layer(window);

  s_app->ready_layer = layer_create(layer_get_bounds_by_value(root));
  layer_set_update_proc(s_app->ready_layer, prv_ready_update_proc);
  layer_add_child(root, s_app->ready_layer);
}

static void prv_ready_unload(Window *window) {
  layer_destroy(s_app->ready_layer);
  s_app->ready_layer = NULL;
  window_destroy(window);
  s_app->ready_window = NULL;
}

static void prv_push_ready_window(void) {
  Window *window = window_create();

  s_app->ready_window = window;
  window_set_background_color(window, GColorBlack);
  window_set_click_config_provider(window, prv_ready_click_config);
  window_set_window_handlers(window, &(WindowHandlers){
                                       .load = prv_ready_load,
                                       .unload = prv_ready_unload,
                                     });
  app_window_stack_push(window, true);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Activity window: UP/DOWN screens, SELECT stop, BACK lap

static void prv_active_up(ClickRecognizerRef recognizer, void *context) {
  const uint8_t n = s_app->profile->num_screens;

  s_app->screen = (uint8_t)((s_app->screen + n - 1) % n);
  layer_mark_dirty(s_app->active_layer);
}

static void prv_active_down(ClickRecognizerRef recognizer, void *context) {
  s_app->screen = (uint8_t)((s_app->screen + 1) % s_app->profile->num_screens);
  layer_mark_dirty(s_app->active_layer);
}

static void prv_active_select(ClickRecognizerRef recognizer, void *context) {
  if (s_app->engine.state != SportState_Paused) {
    prv_set_paused(true);
    layer_mark_dirty(s_app->active_layer);
  }
  prv_push_pause_menu();
}

static void prv_active_back(ClickRecognizerRef recognizer, void *context) {
  if (s_app->engine.state == SportState_Paused) {
    prv_push_pause_menu();
    return;
  }
  if (sport_engine_lap(&s_app->engine)) {
    prv_show_lap();
    layer_mark_dirty(s_app->active_layer);
  }
}

static void prv_active_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, prv_active_up);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_active_down);
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_active_select);
  window_single_click_subscribe(BUTTON_ID_BACK, prv_active_back);
}

static void prv_active_load(Window *window) {
  Layer *root = window_get_root_layer(window);

  s_app->active_layer = layer_create(layer_get_bounds_by_value(root));
  layer_set_update_proc(s_app->active_layer, prv_active_update_proc);
  layer_add_child(root, s_app->active_layer);
}

static void prv_active_unload(Window *window) {
  layer_destroy(s_app->active_layer);
  s_app->active_layer = NULL;
  window_destroy(window);
  s_app->active_window = NULL;
}

static void prv_push_active_window(void) {
  Window *window = window_create();

  s_app->active_window = window;
  window_set_background_color(window, GColorBlack);
  window_set_click_config_provider(window, prv_active_click_config);
  window_set_window_handlers(window, &(WindowHandlers){
                                       .load = prv_active_load,
                                       .unload = prv_active_unload,
                                     });
  app_window_stack_push(window, true);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Pause menu: resume, save, discard

enum {
  PauseRow_Resume,
  PauseRow_Save,
  PauseRow_Discard,
  PauseRowCount,
};

static uint16_t prv_pause_num_rows(MenuLayer *menu, uint16_t section, void *context) {
  return PauseRowCount;
}

static void prv_pause_draw_row(GContext *ctx, const Layer *cell, MenuIndex *index, void *context) {
  static const char *const titles[PauseRowCount] = {"Resume", "Save", "Discard"};
  char subtitle[40] = "";

  if (index->row == PauseRow_Save) {
    char t[12];
    char d[12];
    sport_format_duration(s_app->engine.total.timer_s, t, sizeof(t));
    sport_format_distance(s_app->engine.total.distance_mm, s_app->fields.unit_mm, d, sizeof(d));
    snprintf(subtitle, sizeof(subtitle), "%s  %s %s", t, d,
             sport_metric_unit(SportMetric_Distance, s_app->fields.unit_mm));
  }
  menu_cell_basic_draw(ctx, cell, titles[index->row], subtitle[0] ? subtitle : NULL, NULL);
}

static void prv_pause_select(MenuLayer *menu, MenuIndex *index, void *context) {
  switch (index->row) {
    case PauseRow_Resume:
      prv_set_paused(false);
      app_window_stack_pop(true);
      return;
    case PauseRow_Save:
    case PauseRow_Discard:
      prv_finish(index->row == PauseRow_Save);
      prv_push_summary();
      app_window_stack_remove(s_app->pause_window, false);
      app_window_stack_remove(s_app->active_window, false);
      return;
  }
}

static void prv_pause_load(Window *window) {
  Layer *root = window_get_root_layer(window);

  s_app->pause_menu = menu_layer_create(layer_get_bounds_by_value(root));
  menu_layer_set_callbacks(s_app->pause_menu, NULL,
                           &(MenuLayerCallbacks){
                             .get_num_rows = prv_pause_num_rows,
                             .draw_row = prv_pause_draw_row,
                             .select_click = prv_pause_select,
                           });
  menu_layer_set_highlight_colors(s_app->pause_menu, prv_accent(), GColorBlack);
  menu_layer_set_click_config_onto_window(s_app->pause_menu, window);
  layer_add_child(root, menu_layer_get_layer(s_app->pause_menu));
}

static void prv_pause_unload(Window *window) {
  menu_layer_destroy(s_app->pause_menu);
  s_app->pause_menu = NULL;
  window_destroy(window);
  s_app->pause_window = NULL;
}

static void prv_push_pause_menu(void) {
  Window *window;

  if (s_app->pause_window != NULL) {
    return;
  }

  window = window_create();
  s_app->pause_window = window;
  window_set_window_handlers(window, &(WindowHandlers){
                                       .load = prv_pause_load,
                                       .unload = prv_pause_unload,
                                     });
  app_window_stack_push(window, true);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Summary

enum {
  SummaryRow_Time,
  SummaryRow_Distance,
  SummaryRow_Rate,
  SummaryRow_MaxSpeed,
  SummaryRow_Hr,
  SummaryRow_Ascent,
  SummaryRow_Laps,
  SummaryRowCount,
};

static uint16_t prv_summary_num_laps(void) {
  return (uint16_t)PBL_MIN(s_app->engine.num_laps, SPORT_ENGINE_MAX_LAPS);
}

static uint16_t prv_summary_num_rows(MenuLayer *menu, uint16_t section, void *context) {
  return SummaryRowCount + prv_summary_num_laps();
}

static void prv_format_rate(const SportStats *stats, char *buf, size_t len) {
  const uint32_t unit = s_app->fields.unit_mm;
  const uint32_t speed = sport_stats_avg_speed_mm_s(stats);
  char v[12];

  if (s_app->profile->pace) {
    sport_format_pace(sport_pace_s(speed, unit), v, sizeof(v));
    snprintf(buf, len, "%s %s", v, sport_metric_unit(SportMetric_Pace, unit));
  } else {
    sport_format_speed(speed, unit, v, sizeof(v));
    snprintf(buf, len, "%s %s", v, sport_metric_unit(SportMetric_Speed, unit));
  }
}

static void prv_summary_draw_row(GContext *ctx, const Layer *cell, MenuIndex *index,
                                 void *context) {
  const SportEngine *e = &s_app->engine;
  const uint32_t unit = s_app->fields.unit_mm;
  const char *title = "";
  char sub[40];
  char v[16];

  switch (index->row) {
    case SummaryRow_Time:
      title = s_app->saved ? "Time (saved)" : "Time (discarded)";
      sport_format_duration(e->total.timer_s, sub, sizeof(sub));
      break;
    case SummaryRow_Distance:
      title = "Distance";
      sport_format_distance(e->total.distance_mm, unit, v, sizeof(v));
      snprintf(sub, sizeof(sub), "%s %s", v, sport_metric_unit(SportMetric_Distance, unit));
      break;
    case SummaryRow_Rate:
      title = s_app->profile->pace ? "Avg pace" : "Avg speed";
      prv_format_rate(&e->total, sub, sizeof(sub));
      break;
    case SummaryRow_MaxSpeed:
      title = "Max speed";
      sport_format_speed(e->total.max_speed_mm_s, unit, v, sizeof(v));
      snprintf(sub, sizeof(sub), "%s %s", v, sport_metric_unit(SportMetric_Speed, unit));
      break;
    case SummaryRow_Hr:
      title = "Avg / max HR";
      if (e->total.hr_count) {
        snprintf(sub, sizeof(sub), "%u / %u bpm", sport_stats_avg_hr(&e->total), e->total.hr_max);
      } else {
        snprintf(sub, sizeof(sub), "--");
      }
      break;
    case SummaryRow_Ascent:
      title = "Ascent";
      sport_format_elevation((int32_t)e->total.ascent_cm, unit, v, sizeof(v));
      snprintf(sub, sizeof(sub), "%s %s", v, sport_metric_unit(SportMetric_Ascent, unit));
      break;
    case SummaryRow_Laps:
      title = "Laps";
      snprintf(sub, sizeof(sub), "%u", e->num_laps);
      break;
    default: {
      const SportLap *lap = &e->laps[index->row - SummaryRowCount];
      char t[12];
      char r[20];
      static char s_title[12];
      snprintf(s_title, sizeof(s_title), "Lap %u", lap->number);
      title = s_title;
      sport_format_duration(lap->stats.timer_s, t, sizeof(t));
      prv_format_rate(&lap->stats, r, sizeof(r));
      snprintf(sub, sizeof(sub), "%s  %s", t, r);
      break;
    }
  }

  menu_cell_basic_draw(ctx, cell, title, sub, NULL);
}

static void prv_summary_load(Window *window) {
  Layer *root = window_get_root_layer(window);

  s_app->summary_menu = menu_layer_create(layer_get_bounds_by_value(root));
  menu_layer_set_callbacks(s_app->summary_menu, NULL,
                           &(MenuLayerCallbacks){
                             .get_num_rows = prv_summary_num_rows,
                             .draw_row = prv_summary_draw_row,
                           });
  menu_layer_set_highlight_colors(s_app->summary_menu, prv_accent(), GColorBlack);
  menu_layer_set_click_config_onto_window(s_app->summary_menu, window);
  layer_add_child(root, menu_layer_get_layer(s_app->summary_menu));
}

static void prv_summary_unload(Window *window) {
  menu_layer_destroy(s_app->summary_menu);
  s_app->summary_menu = NULL;
  window_destroy(window);
  s_app->summary_window = NULL;
}

static void prv_push_summary(void) {
  Window *window = window_create();

  s_app->summary_window = window;
  window_set_window_handlers(window, &(WindowHandlers){
                                       .load = prv_summary_load,
                                       .unload = prv_summary_unload,
                                     });
  app_window_stack_push(window, true);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Main

static void prv_init(const SportProfile *profile) {
  SportEngineConfig cfg = profile->engine;

  s_app = app_zalloc_check(sizeof(*s_app));
  s_app->profile = profile;
  s_app->fields.engine = &s_app->engine;
  s_app->fields.unit_mm =
      shell_prefs_get_units_distance() == UnitsDistance_Miles ? SPORT_UNIT_MI_MM : SPORT_UNIT_KM_MM;
  cfg.auto_lap_mm = profile->auto_lap_units * s_app->fields.unit_mm;
  sport_engine_init(&s_app->engine, &cfg);

  workout_service_frontend_opened();
  sport_sensors_init(profile->sensors);

  const time_t t = pbl_realtime();
  struct pbl_tm now;
  const SportSample empty = {};
  pbl_localtime_r(&t, &now);
  prv_update_live(&empty, &now);

  tick_timer_service_subscribe(SECOND_UNIT, prv_tick_handler);
  prv_push_ready_window();
}

static void prv_deinit(void) {
  tick_timer_service_unsubscribe();
  // Leaving mid-activity saves it.
  prv_finish(true);
  sport_sensors_deinit();
  workout_service_frontend_closed();
  app_free(s_app);
  s_app = NULL;
}

void sport_app_main(const SportProfile *profile) {
  prv_init(profile);
  app_event_loop();
  prv_deinit();
}
