/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

// Battery test: shows the phase or the results of the battery capacity test, which runs in the
// background (pbl/services/battery/battery_test.h), and runs its heavy load while in front.

#include <string.h>

#include <pbl/i18n/i18n.h>
#include <pbl/kernel/time.h>
#include <pbl/services/battery/battery_state.h>
#include <pbl/services/battery/battery_test.h>

#include <applib/app.h>
#include <applib/app_light.h>
#include <applib/app_malloc.h>
#include <applib/app_timer.h>
#include <applib/fonts/fonts.h>
#include <applib/ui/ui.h>
#include <process_management/pebble_process_md.h>
#include <process_state/app_state/app_state.h>
#include <resource/resource_ids.auto.h>

#define TEXT_LEN   640
#define REFRESH_MS 5000
// The heavy load: the frontlight on and the CPU busy 40 ms in 50. The app thread is above the
// background work queue, which gets the 10 ms left and runs the battery service.
#define BUSY_MS  40
#define REST_MS  10
#define CHECK_MS 1000
#define INSET    8

typedef struct BatteryTestApp {
  Window window;
  ScrollLayer scroll;
  TextLayer text;
  AppTimer *refresh;
  AppTimer *load;
  bool heavy_on;
  char shown[TEXT_LEN];
  char next[TEXT_LEN];
} BatteryTestApp;

static BatteryTestApp *s_app;

static void prv_refresh(void) {
  const GRect bounds = layer_get_bounds_by_value(window_get_root_layer(&s_app->window));
  GSize size;

  battery_test_describe(s_app->next, TEXT_LEN);
  if (strcmp(s_app->next, s_app->shown) == 0) {
    return;
  }
  strcpy(s_app->shown, s_app->next);

  text_layer_set_size(&s_app->text, GSize(bounds.size.w, 2000));
  text_layer_set_text(&s_app->text, s_app->shown);
  size = text_layer_get_content_size(app_state_get_graphics_context(), &s_app->text);
  size.w = bounds.size.w;
  size.h += 2 * INSET;
  text_layer_set_size(&s_app->text, size);
  scroll_layer_set_content_size(&s_app->scroll, size);
}

static void prv_refresh_cb(void *data) {
  prv_refresh();
  s_app->refresh = app_timer_register(REFRESH_MS, prv_refresh_cb, NULL);
}

// Only while discharging, and not near the low battery power-off: the load must never hold the
// watch at its edge
static bool prv_heavy_allowed(void) {
  const BatteryChargeState c = battery_get_charge_state();

  return battery_test_is_heavy() && (battery_test_get_phase() == BATTERY_TEST_DISCHARGE) &&
         !c.is_plugged && (c.charge_percent > CONFIG_SERVICE_BATTERY_LOW_POWER_PERCENT);
}

static void prv_set_heavy_on(bool on) {
  if (on != s_app->heavy_on) {
    s_app->heavy_on = on;
    app_light_enable(on);
    battery_test_set_heavy_running(on);
  }
}

static void prv_load_cb(void *data) {
  const bool on = prv_heavy_allowed();

  prv_set_heavy_on(on);
  if (on) {
    const uint64_t end = pbl_uptime_ms() + BUSY_MS;

    while (pbl_uptime_ms() < end) {
    }
  }
  s_app->load = app_timer_register(on ? REST_MS : CHECK_MS, prv_load_cb, NULL);
}

static void prv_select(ClickRecognizerRef recognizer, void *context) {
  switch (battery_test_get_phase()) {
    case BATTERY_TEST_IDLE:
      battery_test_start();
      break;
    case BATTERY_TEST_DONE:
      break;
    default:
      battery_test_set_heavy(!battery_test_is_heavy());
      break;
  }
  prv_refresh();
}

static void prv_select_long(ClickRecognizerRef recognizer, void *context) {
  switch (battery_test_get_phase()) {
    case BATTERY_TEST_IDLE:
      break;
    case BATTERY_TEST_DONE:
      battery_test_start();
      break;
    default:
      battery_test_cancel();
      break;
  }
  prv_refresh();
}

static void prv_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select);
  window_long_click_subscribe(BUTTON_ID_SELECT, 700, prv_select_long, NULL);
}

static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  const GRect bounds = layer_get_bounds_by_value(root);

  scroll_layer_init(&s_app->scroll, &bounds);
  scroll_layer_set_click_config_onto_window(&s_app->scroll, window);
  scroll_layer_set_callbacks(&s_app->scroll, (ScrollLayerCallbacks){
                                               .click_config_provider = prv_click_config,
                                             });
  scroll_layer_set_shadow_hidden(&s_app->scroll, true);
  layer_add_child(root, scroll_layer_get_layer(&s_app->scroll));

  text_layer_init(&s_app->text, &GRect(0, 0, bounds.size.w, 2000));
  text_layer_set_font(&s_app->text, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(&s_app->text, GTextAlignmentCenter);
  scroll_layer_add_child(&s_app->scroll, text_layer_get_layer(&s_app->text));
  text_layer_enable_screen_text_flow_and_paging(&s_app->text, INSET);

  prv_refresh_cb(NULL);
  prv_load_cb(NULL);
}

static void prv_main(void) {
  s_app = app_zalloc_check(sizeof(*s_app));
  window_init(&s_app->window, WINDOW_NAME("Battery test"));
  window_set_window_handlers(&s_app->window, &(WindowHandlers){
                                               .load = prv_window_load,
                                             });
  app_window_stack_push(&s_app->window, true);

  app_event_loop();

  app_timer_cancel(s_app->refresh);
  app_timer_cancel(s_app->load);
  prv_set_heavy_on(false);
  app_free(s_app);
  s_app = NULL;
}

const PebbleProcessMd *battery_test_app_get_info(void) {
  static const PebbleProcessMdSystem s_info = {
    .common =
        {
          .main_func = prv_main,
          .uuid =
              {0x6e, 0x3f, 0x0b, 0x52, 0x9d, 0x41, 0x4c, 0x8a, 0xb2, 0x57, 0x1c, 0xe4, 0x80, 0x3d,
               0x95, 0x7a},
        },
    .name = pbl_i18n_noop("Battery test"),
    .icon_resource_id = RESOURCE_ID_DURATION_TINY,
  };

  return (const PebbleProcessMd *)&s_info;
}
