/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pbl/fs/fs.h>
#include <pbl/util/size.h>

#include <apps/system/sport/engine.h>
#include <apps/system/sport/fit.h>
#include <apps/system/sport/fit_stream.h>
#include <apps/system/sport/journal.h>
#include <apps/system/sport/recorder.h>
#include <apps/system/sport/store.h>
#include <clar.h>
#include <fake_pfs.h>
#include <fake_rtc.h>
#include <fake_spi_flash.h>

// FIT files read at any offset, as a USB drive's host reads them: the same bytes as the export,
// in any order, at a bounded cost a read.

#define START_UTC  1795860000U
#define MARK_EVERY 4096U
#define NUM_MARKS  64U

static SportEngine s_engine;
static const SportEngineConfig s_cfg = {
  .max_speed_mm_s = 11000,
  .max_hdop_x10 = 50,
  .min_sats = 4,
  .jitter_mm = 3000,
};

static uint8_t s_fit[256 * 1024];
static size_t s_fit_len;
static uint8_t s_buf[256 * 1024];
static SportFitStream s_stream;
static SportFitMark s_marks[NUM_MARKS];

static SportSample prv_sample(void) {
  return (SportSample){
    .flags = SportSampleFlag_Fix | SportSampleFlag_GnssSpeed | SportSampleFlag_Cadence,
    .sats = 8,
    .hdop_x10 = 9,
    .lat_e7 = 473769000 + (int32_t)s_engine.elapsed_s * 270,
    .lon_e7 = 85417000,
    .gnss_speed_mm_s = 3000,
    .cadence = 170,
  };
}

static void prv_run(uint32_t seconds) {
  for (uint32_t i = 0; i < seconds; i++) {
    const SportSample s = prv_sample();
    sport_engine_tick(&s_engine, &s);
    sport_recorder_sample(&s_engine, &s, s_engine.elapsed_s);
  }
}

static void prv_fit_write(const void *data, size_t len, void *ctx) {
  cl_assert(s_fit_len + len <= sizeof(s_fit));
  memcpy(&s_fit[s_fit_len], data, len);
  s_fit_len += len;
}

// An activity over three segments, with laps, a pause and the GNSS's time, and its export
static void prv_record(uint32_t start, uint32_t seconds) {
  sport_engine_init(&s_engine, &s_cfg);
  sport_engine_start(&s_engine);
  cl_assert(sport_recorder_start(SportJournalSport_Run, start));
  prv_run(10);
  sport_recorder_time(start + 10 + 42, start + 10, 10);
  for (uint32_t t = 10; t < seconds; t += 300) {
    prv_run(250);
    sport_engine_pause(&s_engine, true);
    sport_recorder_event(SportJournalEvent_Pause, s_engine.elapsed_s);
    prv_run(20);
    sport_engine_pause(&s_engine, false);
    sport_recorder_event(SportJournalEvent_Resume, s_engine.elapsed_s);
    prv_run(30);
    if (sport_engine_lap(&s_engine)) {
      sport_recorder_lap(sport_engine_last_lap(&s_engine), s_engine.elapsed_s);
    }
  }
  sport_engine_stop(&s_engine);
  sport_recorder_lap(sport_engine_last_lap(&s_engine), s_engine.elapsed_s);
  sport_recorder_finish(&s_engine, s_engine.elapsed_s);

  s_fit_len = 0;
  cl_assert_equal_i(sport_store_export_fit(start, prv_fit_write, NULL), s_fit_len);
}

void test_sport_fit_stream__initialize(void) {
  fake_spi_flash_init(0, 0x1000000);
  fake_pfs_mount();
  pbl_fs_format("/");
  sport_recorder_test_lose_buffer();
  srand(1);
}

void test_sport_fit_stream__cleanup(void) {
  sport_fit_stream_close(&s_stream);
  fake_spi_flash_cleanup();
}

static void prv_check(uint32_t offset, uint32_t len) {
  cl_must_pass(sport_fit_stream_read(&s_stream, offset, s_buf, len));
  if (memcmp(s_buf, &s_fit[offset], len) != 0) {
    cl_fail("bytes differ from the export's");
  }
}

void test_sport_fit_stream__in_order_as_the_export(void) {
  prv_record(START_UTC, 3000);
  cl_assert(s_fit_len > 3 * MARK_EVERY);

  cl_must_pass(sport_fit_stream_open(&s_stream, START_UTC, s_marks, NUM_MARKS, MARK_EVERY));
  cl_assert_equal_i(s_stream.size, s_fit_len);
  cl_assert_equal_i(s_stream.info.utc_start, START_UTC + 42);
  // The sizing pass left a mark in each window of the file, without its CRC
  cl_assert(s_marks[0].offset >= MARK_EVERY && s_marks[0].offset < 2 * MARK_EVERY);
  cl_assert(!s_marks[0].crc_valid);
  cl_assert_equal_i(s_marks[s_fit_len / MARK_EVERY].offset, 0);

  // Read in host-sized pieces, start to end: one pass, its CRC kept
  for (uint32_t off = 0; off < s_fit_len; off += 4096) {
    prv_check(off, off + 4096 <= s_fit_len ? 4096 : (uint32_t)s_fit_len - off);
  }
  cl_assert_equal_i(s_stream.restarts, 1);
  cl_assert(s_stream.crc_known);
  cl_assert(s_marks[0].crc_valid);
  cl_assert_equal_i(sport_crc16(0, s_fit, s_fit_len), 0);

  // Odd pieces too
  prv_check(0, 1);
  prv_check(1, 13);
  prv_check(14, 1000);
  prv_check(1014, (uint32_t)s_fit_len - 1014);

  // Nothing past the end
  cl_assert(sport_fit_stream_read(&s_stream, (uint32_t)s_fit_len - 1, s_buf, 2) < 0);
  cl_must_pass(sport_fit_stream_read(&s_stream, (uint32_t)s_fit_len, s_buf, 0));
}

void test_sport_fit_stream__any_order_at_a_bounded_cost(void) {
  prv_record(START_UTC, 3000);
  cl_must_pass(sport_fit_stream_open(&s_stream, START_UTC, s_marks, NUM_MARKS, MARK_EVERY));

  // The end first, for its CRC, then a host's random reads, of whole sectors and of odd sizes;
  // each starts at
  // most a mark's distance before what it returns, so encodes at most that much more, plus the
  // records of what it returns (some 26 bytes each)
  prv_check((uint32_t)s_fit_len - 10, 10);
  cl_assert(s_stream.crc_known);
  for (int i = 0; i < 300; i++) {
    const uint32_t len = (i % 3 == 0) ? 512 : (uint32_t)(rand() % 3000) + 1;
    const uint32_t off = (uint32_t)rand() % ((uint32_t)s_fit_len - len + 1);
    const uint32_t before = s_stream.records;

    prv_check(off, len);
    cl_assert(s_stream.records - before <= (MARK_EVERY + len) / 20 + 2);
  }
}

void test_sport_fit_stream__the_end_first(void) {
  prv_record(START_UTC, 1500);
  cl_must_pass(sport_fit_stream_open(&s_stream, START_UTC, s_marks, NUM_MARKS, MARK_EVERY));

  // The CRC needs a pass from the start, once
  prv_check((uint32_t)s_fit_len - 2, 2);
  cl_assert_equal_i(s_stream.restarts, 1);
  prv_check((uint32_t)s_fit_len - 600, 600);
  cl_assert_equal_i(s_stream.restarts, 2);
  // And what the sizing pass's marks give after that, the CRC from the first pass
  sport_fit_stream_close(&s_stream);
  s_stream.marks[0].crc_valid = false;
  prv_check((uint32_t)s_fit_len - 100, 100);
  prv_check(MARK_EVERY + 100, 100);
}

void test_sport_fit_stream__reopened_from_what_was_found(void) {
  uint32_t size;
  uint32_t utc;
  uint16_t crc;

  prv_record(START_UTC, 1500);
  cl_must_pass(sport_fit_stream_open(&s_stream, START_UTC, s_marks, NUM_MARKS, MARK_EVERY));
  prv_check(0, (uint32_t)s_fit_len);
  size = s_stream.size;
  utc = s_stream.info.utc_start;
  crc = s_stream.crc;
  sport_fit_stream_close(&s_stream);

  // No sizing pass, no marks: they come with the reads
  memset(s_marks, 0, sizeof(s_marks));
  sport_fit_stream_reopen(&s_stream, START_UTC, utc, size, true, crc, s_marks, NUM_MARKS,
                          MARK_EVERY);
  cl_assert_equal_i(s_marks[0].offset, 0);
  prv_check((uint32_t)s_fit_len - 300, 300);
  cl_assert_equal_i(s_stream.restarts, 1);
  prv_check(100, 5000);
  prv_check(MARK_EVERY * 2 + 7, 3);
  cl_assert(s_stream.restarts <= 3);
}

void test_sport_fit_stream__a_missing_activity(void) {
  cl_assert_equal_i(sport_fit_stream_open(&s_stream, START_UTC, s_marks, NUM_MARKS, MARK_EVERY),
                    -ENOENT);
}
