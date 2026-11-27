/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/drivers/display.h>
#include <pbl/services/compositor/compositor.h>
#include <pbl/services/compositor/compositor_display.h>

#include <applib/graphics/framebuffer.h>
#include <clar.h>
#include <kernel/event_loop.h>
#include <kernel/events.h>

// Where the display's completion comes from: KernelMain, an interrupt or another thread

static bool s_in_isr;
static bool s_on_kernel_main;

bool pbl_in_isr(void) {
  return s_in_isr;
}

bool launcher_task_is_current_task(void) {
  return s_on_kernel_main;
}

static int s_event_count;
static int s_event_isr_count;
static PebbleEvent s_event;

void event_put(PebbleEvent *event) {
  s_event_count++;
  s_event = *event;
}

void event_put_isr(PebbleEvent *event) {
  s_event_isr_count++;
  s_event = *event;
}

// The display, which completes when the test says

const struct pbl_display *const DISPLAY = nullptr;

static bool s_sync;
static pbl_display_callback_t s_cb;
static void *s_cb_arg;

int pbl_display_update(const struct pbl_display *disp, const struct pbl_display_buffer *buf,
                       uint16_t y0, uint16_t y1, pbl_display_callback_t cb, void *arg) {
  s_cb = cb;
  s_cb_arg = arg;
  if (s_sync) {
    cb(0, arg);
  }
  return 0;
}

bool pbl_display_update_in_progress(const struct pbl_display *disp) {
  return s_cb != nullptr;
}

static FrameBuffer s_fb;
static bool s_dirty;

FrameBuffer *compositor_get_framebuffer(void) {
  return &s_fb;
}

static uint32_t s_pixels[16];

#if PBL_BW
uint32_t *framebuffer_get_line(FrameBuffer *f, uint8_t y) {
  return s_pixels;
}
#else
uint8_t *framebuffer_get_line(FrameBuffer *f, uint16_t y) {
  return (uint8_t *)s_pixels;
}
#endif

bool framebuffer_is_dirty(FrameBuffer *f) {
  return s_dirty;
}

void framebuffer_reset_dirty(FrameBuffer *f) {
  s_dirty = false;
}

void compositor_app_framebuffer_mirror_invalidate(void) {
}

static int s_done_count;
static bool s_done_success;

static void prv_done(bool success) {
  s_done_count++;
  s_done_success = success;
}

static void prv_complete(int status) {
  pbl_display_callback_t cb = s_cb;

  s_cb = nullptr;
  cb(status, s_cb_arg);
}

void test_compositor_display__initialize(void) {
  s_in_isr = false;
  s_on_kernel_main = true;
  s_event_count = 0;
  s_event_isr_count = 0;
  s_event = (PebbleEvent){};
  s_sync = false;
  s_cb = nullptr;
  s_fb = (FrameBuffer){.size = {PBL_DISPLAY_WIDTH, PBL_DISPLAY_HEIGHT}};
  s_fb.dirty_rect = GRect(0, 0, PBL_DISPLAY_WIDTH, PBL_DISPLAY_HEIGHT);
  s_dirty = true;
  s_done_count = 0;
  s_done_success = false;
}

void test_compositor_display__synchronous_completes_at_once(void) {
  s_sync = true;
  compositor_display_update(prv_done);

  cl_assert_equal_i(s_done_count, 1);
  cl_assert(s_done_success);
  cl_assert(!s_dirty);
  cl_assert_equal_i(s_event_count + s_event_isr_count, 0);
}

void test_compositor_display__interrupt_completes_on_kernel_main(void) {
  compositor_display_update(prv_done);
  s_in_isr = true;
  s_on_kernel_main = false;
  prv_complete(0);

  cl_assert_equal_i(s_done_count, 0);
  cl_assert_equal_i(s_event_isr_count, 1);
  cl_assert_equal_i(s_event.type, PEBBLE_CALLBACK_EVENT);

  s_in_isr = false;
  s_on_kernel_main = true;
  s_event.callback.callback(s_event.callback.data);
  cl_assert_equal_i(s_done_count, 1);
  cl_assert(s_done_success);
  cl_assert(!s_dirty);
}

void test_compositor_display__thread_completes_on_kernel_main(void) {
  compositor_display_update(prv_done);
  // The display's own thread
  s_on_kernel_main = false;
  prv_complete(0);

  cl_assert_equal_i(s_done_count, 0);
  cl_assert(s_dirty);
  cl_assert_equal_i(s_event_count, 1);
  cl_assert_equal_i(s_event_isr_count, 0);
  cl_assert_equal_i(s_event.type, PEBBLE_CALLBACK_EVENT);

  s_on_kernel_main = true;
  s_event.callback.callback(s_event.callback.data);
  cl_assert_equal_i(s_done_count, 1);
  cl_assert(s_done_success);
  cl_assert(!s_dirty);
}

void test_compositor_display__thread_reports_failure(void) {
  compositor_display_update(prv_done);
  s_on_kernel_main = false;
  prv_complete(-5);

  s_on_kernel_main = true;
  s_event.callback.callback(s_event.callback.data);
  cl_assert_equal_i(s_done_count, 1);
  cl_assert(!s_done_success);
}
