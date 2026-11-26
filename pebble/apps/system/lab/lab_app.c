/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

// Lab: a list of experiments; each runs its steps one at a time from app timers, with the step's
// name on screen and in a breadcrumb before it runs, so that a step that resets the watch is the
// last thing shown and is reported on the next boot.

#include "lab.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <pbl/boot_trace/boot_trace.h>
#include <pbl/drivers/mmc.h>
#include <pbl/i18n/i18n.h>
#include <pbl/util/size.h>

#include <applib/app.h>
#include <applib/app_malloc.h>
#include <applib/app_timer.h>
#include <applib/fonts/fonts.h>
#include <applib/ui/simple_menu_layer.h>
#include <applib/ui/ui.h>
#include <apps/system_app_ids.h>
#include <process_management/app_manager.h>
#include <process_management/pebble_process_md.h>
#include <process_state/app_state/app_state.h>
#include <resource/resource_ids.auto.h>

#define TEXT_SIZE 6144U
#define LINE_SIZE 160U
// Time for the marker of a step to reach the screen before the step runs
#define MARKER_MS 120U
#define NEXT_MS   20U
#define INSET     8
// The step of a breadcrumb at the end of an experiment, and at the opening of the app
#define STEP_DONE 0xFFFFU
#define STEP_OPEN 0xFFF0U

const struct lab_experiment *const lab_experiments[] = {
  &lab_exp_boot,       &lab_exp_clock_show,     &lab_exp_clock_b,         &lab_exp_clock_hsi48,
  &lab_exp_emmc_raw,   &lab_exp_emmc_driver,    &lab_exp_emmc_supply_cut, &lab_exp_nor,
  &lab_exp_fat,        &lab_exp_copy,           &lab_exp_b_storage,       &lab_exp_b_logs,
  &lab_exp_battery,    &lab_exp_test_fault,     &lab_exp_test_assert,     &lab_exp_test_watchdog,
  &lab_exp_test_reset, &lab_exp_test_power_off,
};
const size_t lab_num_experiments = PBL_ARRAY_SIZE(lab_experiments);

#define NUM_EXPERIMENTS PBL_ARRAY_SIZE(lab_experiments)

typedef struct LabApp {
  Window menu_window;
  SimpleMenuLayer menu;
  SimpleMenuSection section;
  SimpleMenuItem items[NUM_EXPERIMENTS];

  Window result_window;
  ScrollLayer scroll;
  TextLayer text;
  char buf[TEXT_SIZE];
  size_t len;
  bool dirty;
  bool loaded;

  size_t exp;
  size_t step;
  bool running;
  bool awaiting_confirm;
  AppTimer *timer;
} LabApp;

static LabApp *s_app;

// Output

static void prv_append(const char *s, size_t n) {
  LabApp *app = s_app;

  if (n >= TEXT_SIZE / 2U) {
    n = TEXT_SIZE / 2U;
  }
  // Full: the older half goes, from a line start
  if (app->len + n + 1U > TEXT_SIZE) {
    const char *cut = memchr(&app->buf[TEXT_SIZE / 2U], '\n', app->len - TEXT_SIZE / 2U);
    size_t keep_from = (cut != nullptr) ? (size_t)(cut - app->buf) + 1U : TEXT_SIZE / 2U;

    memmove(app->buf, &app->buf[keep_from], app->len - keep_from);
    app->len -= keep_from;
  }
  memcpy(&app->buf[app->len], s, n);
  app->len += n;
  app->buf[app->len] = '\0';
  app->dirty = true;
}

void lab_printf(const char *fmt, ...) {
  char line[LINE_SIZE];
  va_list args;
  int n;

  if (s_app == nullptr) {
    return;
  }

  va_start(args, fmt);
  n = vsnprintf(line, sizeof(line) - 1U, fmt, args);
  va_end(args);
  if (n < 0) {
    return;
  }
  if ((size_t)n > sizeof(line) - 2U) {
    n = (int)sizeof(line) - 2;
  }
  line[n++] = '\n';
  prv_append(line, (size_t)n);
}

void lab_hex(const char *label, const void *buf, size_t len) {
  const uint8_t *b = buf;
  char line[LINE_SIZE];

  for (size_t i = 0U; i < len; i += 8U) {
    size_t n = (size_t)snprintf(line, sizeof(line), "%s", (i == 0U) ? label : "");

    for (size_t j = i; j < len && j < i + 8U && n + 4U < sizeof(line); j++) {
      n += (size_t)snprintf(&line[n], sizeof(line) - n, " %02x", b[j]);
    }
    lab_printf("%s", line);
  }
}

static void prv_refresh(void) {
  LabApp *app = s_app;
  const GRect bounds = layer_get_bounds_by_value(window_get_root_layer(&app->result_window));
  GSize size;

  if (!app->dirty || !app->loaded) {
    return;
  }
  app->dirty = false;

  text_layer_set_size(&app->text, GSize(bounds.size.w, 20000));
  text_layer_set_text(&app->text, app->buf);
  size = text_layer_get_content_size(app_state_get_graphics_context(), &app->text);
  size.w = bounds.size.w;
  size.h += 2 * INSET;
  text_layer_set_size(&app->text, size);
  scroll_layer_set_content_size(&app->scroll, size);
  // While it runs, the last lines show
  if (app->running && size.h > bounds.size.h) {
    scroll_layer_set_content_offset(&app->scroll, GPoint(0, bounds.size.h - size.h), false);
  }
}

// Buffers: from the firmware's own RAM, in SRAM1, where the eMMC builds' buffers are too, rather
// than the app heap at the top of SRAM5, so that the SDMMC's DMA reaches them as it reaches theirs

#define ARENA_SIZE (96U * 1024U)

static uint8_t s_arena[ARENA_SIZE] __attribute__((aligned(8)));
static size_t s_arena_used;

void *lab_alloc(size_t size) {
  void *p;

  size = (size + 7U) & ~(size_t)7U;
  if (size > ARENA_SIZE - s_arena_used) {
    lab_printf("No memory for %u bytes", (unsigned int)size);
    return nullptr;
  }
  p = &s_arena[s_arena_used];
  s_arena_used += size;
  memset(p, 0, size);

  return p;
}

static void prv_free_all(void) {
  s_arena_used = 0U;
}

// The eMMC, guarded

bool lab_gui_writable(uint32_t lba, uint32_t count) {
  return lba >= LAB_GUI_FIRST && lba < LAB_GUI_END && count <= LAB_GUI_END - lba;
}

int lab_mmc_read(uint32_t lba, void *buf, uint32_t count) {
  return pbl_mmc_read(MMC, lba, buf, count);
}

int lab_mmc_write(uint32_t lba, const void *buf, uint32_t count) {
  if (!lab_gui_writable(lba, count)) {
    lab_printf("REFUSED: write %lu+%lu not in GUI", (unsigned long)lba, (unsigned long)count);
    return -EACCES;
  }

  return pbl_mmc_write(MMC, lba, buf, count);
}

// The runner

static void prv_next(void *data);

static void prv_finish(bool failed) {
  LabApp *app = s_app;
  const struct lab_experiment *exp = lab_experiments[app->exp];

  if (exp->cleanup != nullptr) {
    exp->cleanup();
  }
  prv_free_all();
  app->running = false;
  app->timer = nullptr;
  pbl_boot_trace_crumb(PBL_BT_LAB + app->exp, STEP_DONE);
  if (exp->num_steps > 1U || failed) {
    lab_printf(failed ? "== Stopped at step %u ==" : "== Done ==", (unsigned int)app->step + 1U);
  }
  prv_refresh();
}

static void prv_run_step(void *data) {
  LabApp *app = s_app;
  const struct lab_step *step = &lab_experiments[app->exp]->steps[app->step];
  int ret;

  app->timer = nullptr;
  ret = step->fn();
  if (ret < 0) {
    lab_printf("  FAILED: %d", ret);
    prv_finish(true);
    return;
  }
  if (ret == LAB_SKIP) {
    lab_printf("  skipped");
  }
  app->step++;
  prv_refresh();
  app->timer = app_timer_register(NEXT_MS, prv_next, nullptr);
}

static void prv_next(void *data) {
  LabApp *app = s_app;
  const struct lab_experiment *exp = lab_experiments[app->exp];

  app->timer = nullptr;
  if (app->step >= exp->num_steps) {
    prv_finish(false);
    return;
  }

  // On screen and kept before it runs
  if (exp->num_steps > 1U) {
    lab_printf("[%u] %s", (unsigned int)app->step + 1U, exp->steps[app->step].name);
  }
  pbl_boot_trace_crumb(PBL_BT_LAB + app->exp, app->step + 1U);
  prv_refresh();
  app->timer = app_timer_register(MARKER_MS, prv_run_step, nullptr);
}

static void prv_start(void) {
  LabApp *app = s_app;

  app->awaiting_confirm = false;
  app->running = true;
  app->step = 0U;
  // Once the window is up
  app->timer = app_timer_register(MARKER_MS, prv_next, nullptr);
}

// The result window

static void prv_select(ClickRecognizerRef recognizer, void *context) {
  LabApp *app = s_app;

  if (app->running) {
    return;
  }
  if (app->awaiting_confirm) {
    lab_printf("%s", "");
    prv_start();
    return;
  }
  // Again
  app->len = 0U;
  app->buf[0] = '\0';
  lab_printf("%s", lab_experiments[app->exp]->title);
  prv_start();
}

static void prv_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select);
}

static void prv_result_load(Window *window) {
  LabApp *app = s_app;
  Layer *root = window_get_root_layer(window);
  const GRect bounds = layer_get_bounds_by_value(root);

  scroll_layer_init(&app->scroll, &bounds);
  scroll_layer_set_click_config_onto_window(&app->scroll, window);
  scroll_layer_set_callbacks(&app->scroll, (ScrollLayerCallbacks){
                                             .click_config_provider = prv_click_config,
                                           });
  scroll_layer_set_shadow_hidden(&app->scroll, true);
  layer_add_child(root, scroll_layer_get_layer(&app->scroll));

  text_layer_init(&app->text, &GRect(0, 0, bounds.size.w, 20000));
  text_layer_set_font(&app->text, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(&app->text, GTextAlignmentCenter);
  scroll_layer_add_child(&app->scroll, text_layer_get_layer(&app->text));
  text_layer_enable_screen_text_flow_and_paging(&app->text, INSET);

  app->loaded = true;
  app->dirty = true;
  prv_refresh();
}

static void prv_result_unload(Window *window) {
  LabApp *app = s_app;

  if (app->timer != nullptr) {
    app_timer_cancel(app->timer);
    app->timer = nullptr;
  }
  if (app->running) {
    lab_printf("Left at step %u", (unsigned int)app->step + 1U);
    prv_finish(true);
  }
  app->loaded = false;
  text_layer_deinit(&app->text);
  scroll_layer_deinit(&app->scroll);
}

static void prv_open(size_t index) {
  LabApp *app = s_app;
  const struct lab_experiment *exp = lab_experiments[index];

  app->exp = index;
  app->len = 0U;
  app->buf[0] = '\0';
  app->dirty = true;
  lab_printf("%s", exp->title);

  window_init(&app->result_window, WINDOW_NAME("Lab result"));
  window_set_window_handlers(&app->result_window, &(WindowHandlers){
                                                    .load = prv_result_load,
                                                    .unload = prv_result_unload,
                                                  });
  app_window_stack_push(&app->result_window, true);

  if (exp->confirm != nullptr) {
    lab_printf("%s", exp->confirm);
    lab_printf("Select: run. Back: leave.");
    app->awaiting_confirm = true;
    prv_refresh();
    return;
  }
  prv_start();
}

// The menu

static void prv_menu_select(int index, void *context) {
  prv_open((size_t)index);
}

static void prv_menu_load(Window *window) {
  LabApp *app = s_app;
  Layer *root = window_get_root_layer(window);
  const GRect bounds = layer_get_bounds_by_value(root);

  for (size_t i = 0U; i < NUM_EXPERIMENTS; i++) {
    app->items[i] = (SimpleMenuItem){
      .title = lab_experiments[i]->title,
      .subtitle = (i == 0U) ? lab_boot_summary() : lab_experiments[i]->subtitle,
      .callback = prv_menu_select,
    };
  }
  app->section = (SimpleMenuSection){.items = app->items, .num_items = NUM_EXPERIMENTS};
  simple_menu_layer_init(&app->menu, &bounds, window, &app->section, 1, nullptr);
  layer_add_child(root, simple_menu_layer_get_layer(&app->menu));
}

static void prv_menu_unload(Window *window) {
  simple_menu_layer_deinit(&s_app->menu);
}

static void prv_main(void) {
  pbl_boot_trace_crumb(PBL_BT_LAB, STEP_OPEN);
  s_app = app_zalloc_check(sizeof(*s_app));
  window_init(&s_app->menu_window, WINDOW_NAME("Lab"));
  window_set_window_handlers(&s_app->menu_window, &(WindowHandlers){
                                                    .load = prv_menu_load,
                                                    .unload = prv_menu_unload,
                                                  });
  app_window_stack_push(&s_app->menu_window, false);

  // What the run before left, first
  prv_open(0U);

  app_event_loop();

  app_free(s_app);
  s_app = nullptr;
}

const PebbleProcessMd *lab_app_get_info(void) {
  static const PebbleProcessMdSystem s_info = {
    .common =
        {
          .main_func = prv_main,
          .uuid =
              {0x2c, 0x51, 0x7a, 0x04, 0x6b, 0x3e, 0x4f, 0x19, 0x9d, 0x62, 0xe0, 0x18, 0x5b, 0xa7,
               0x33, 0xc1},
        },
    .name = pbl_i18n_noop("Lab"),
    .icon_resource_id = RESOURCE_ID_DURATION_TINY,
  };

  return (const PebbleProcessMd *)&s_info;
}

#ifdef CONFIG_APP_LAB_AUTOSTART
// Not when the run before stopped while the Lab opened or showed its report: opening it again
// would only reset again
void lab_app_launch_at_boot(void) {
  const uint32_t crumb = lab_previous_place(nullptr);

  if (PBL_BOOT_TRACE_AREA(crumb) == PBL_BT_LAB && PBL_BOOT_TRACE_STEP(crumb) != STEP_DONE) {
    return;
  }
  app_manager_put_launch_app_event(&(AppLaunchEventConfig){.id = APP_ID_LAB});
}
#endif
