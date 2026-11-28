/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pbl/fs/fs.h>
#include <pbl/util/size.h>

#include <apps/system/sport/engine.h>
#include <apps/system/sport/fit.h>
#include <apps/system/sport/journal.h>
#include <apps/system/sport/recorder.h>
#include <apps/system/sport/store.h>
#include <clar.h>
#include <fake_pfs.h>
#include <fake_rtc.h>
#include <fake_spi_flash.h>

// The recorder and the store on PFS on a fake flash: what reaches the flash, and when; what a
// reboot keeps; resuming, closing, pruning, exporting.

#define START_UTC 1795860000U

static SportEngine s_engine;
static const SportEngineConfig s_cfg = {
  .max_speed_mm_s = 11000,
  .max_hdop_x10 = 50,
  .min_sats = 4,
  .jitter_mm = 3000,
};

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

// Run @p seconds through the engine and the recorder.
static void prv_run(uint32_t seconds) {
  for (uint32_t i = 0; i < seconds; i++) {
    const SportSample s = prv_sample();
    sport_engine_tick(&s_engine, &s);
    sport_recorder_sample(&s_engine, &s, s_engine.elapsed_s);
  }
}

static uint32_t prv_records_on_flash(uint32_t start, SportJournalScan *scan) {
  uint8_t sport;

  cl_assert_equal_i(sport_store_scan(start, &sport, scan), 0);
  return scan->records;
}

static void prv_start(uint32_t start) {
  sport_engine_init(&s_engine, &s_cfg);
  sport_engine_start(&s_engine);
  cl_assert(sport_recorder_start(SportJournalSport_Run, start));
}

void test_sport_recorder__initialize(void) {
  fake_spi_flash_init(0, 0x1000000);
  fake_pfs_mount();
  pbl_fs_format("/");
  sport_recorder_test_lose_buffer();
}

void test_sport_recorder__cleanup(void) {
  fake_spi_flash_cleanup();
}

void test_sport_recorder__samples_reach_the_flash_every_flush_interval(void) {
  SportJournalScan scan;

  prv_start(START_UTC);
  // The start event is written at once.
  cl_assert_equal_i(prv_records_on_flash(START_UTC, &scan), 1);
  prv_run(CONFIG_APP_SPORT_FLUSH_S - 1);
  cl_assert_equal_i(prv_records_on_flash(START_UTC, &scan), 1);
  prv_run(1);
  cl_assert_equal_i(prv_records_on_flash(START_UTC, &scan), 1 + CONFIG_APP_SPORT_FLUSH_S);
  // An event writes what is buffered with it.
  prv_run(3);
  sport_recorder_event(SportJournalEvent_Pause, s_engine.elapsed_s);
  cl_assert_equal_i(prv_records_on_flash(START_UTC, &scan), 1 + CONFIG_APP_SPORT_FLUSH_S + 4);
}

void test_sport_recorder__a_crash_loses_only_the_buffer_and_a_reboot_keeps_the_rest(void) {
  SportJournalScan scan;
  SportStoreEntry entries[4];

  prv_start(START_UTC);
  prv_run(95);
  // A fault: nothing more is written, the state is gone; then a reboot.
  sport_recorder_test_lose_buffer();
  fake_pfs_mount();

  cl_assert_equal_i(prv_records_on_flash(START_UTC, &scan), 1 + 90);
  cl_assert_equal_i(scan.timer_s, 90);
  cl_assert(!scan.stopped);
  cl_assert_equal_i(sport_store_list(entries, 4), 1);
  cl_assert(entries[0].readable);
  cl_assert(!entries[0].closed);

  // Carry on, then finish.
  cl_assert(sport_recorder_resume(START_UTC, 120));
  s_engine.elapsed_s = 120;
  sport_engine_pause(&s_engine, false);
  prv_run(30);
  sport_engine_stop(&s_engine);
  sport_recorder_finish(&s_engine, s_engine.elapsed_s);

  cl_assert_equal_i(sport_store_list(entries, 4), 1);
  cl_assert(entries[0].closed);
  cl_assert_equal_i(prv_records_on_flash(START_UTC, &scan), 1 + 90 + 1 + 30 + 1);
  cl_assert(scan.stopped);
}

void test_sport_recorder__an_orderly_flush_keeps_everything(void) {
  SportJournalScan scan;

  prv_start(START_UTC);
  prv_run(7);
  sport_recorder_flush();
  sport_recorder_test_lose_buffer();
  fake_pfs_mount();
  cl_assert_equal_i(prv_records_on_flash(START_UTC, &scan), 8);
}

void test_sport_recorder__segments_follow_each_other(void) {
  SportJournalScan scan;
  SportJournalReader r;
  SportJournalRecord rec;
  uint16_t seg;
  uint16_t idx;

  prv_start(START_UTC);
  prv_run(2500);
  sport_engine_stop(&s_engine);
  sport_recorder_finish(&s_engine, s_engine.elapsed_s);

  cl_assert_equal_i(prv_records_on_flash(START_UTC, &scan), 1 + 2500 + 1);
  cl_assert_equal_i(scan.timer_s, 2500);
  cl_assert_equal_i(scan.distance_cm, s_engine.total.distance_mm / 10);

  cl_assert_equal_i(sport_journal_reader_open(&r, START_UTC), 0);
  while (sport_journal_reader_next(&r, &rec) == 1) {
  }
  sport_journal_reader_end(&r, &seg, &idx);
  sport_journal_reader_close(&r);
  cl_assert_equal_i(seg, 2);
  cl_assert_equal_i(idx, 2502 - 2 * SPORT_JOURNAL_PER_SEGMENT);
  cl_assert_equal_i(r.damaged, 0);
}

void test_sport_recorder__a_damaged_record_is_skipped(void) {
  SportJournalScan scan;
  struct pbl_fs_file file;
  char path[SPORT_JOURNAL_PATH_LEN];
  const uint8_t zero = 0;
  SportJournalReader r;
  SportJournalRecord rec;

  prv_start(START_UTC);
  prv_run(20);
  sport_recorder_flush();

  // Clear a bit of the fifth record's distance.
  sport_journal_segment_path(path, START_UTC, 0);
  cl_assert_equal_i(pbl_fs_open(&file, path, PBL_FS_O_WRITE), 0);
  pbl_fs_seek(&file, 6 * SPORT_JOURNAL_RECORD_SIZE + 16, PBL_FS_SEEK_SET);
  cl_assert_equal_i(pbl_fs_write(&file, &zero, 1), 1);
  pbl_fs_close(&file);

  cl_assert_equal_i(prv_records_on_flash(START_UTC, &scan), 20);
  cl_assert_equal_i(sport_journal_reader_open(&r, START_UTC), 0);
  while (sport_journal_reader_next(&r, &rec) == 1) {
  }
  sport_journal_reader_close(&r);
  cl_assert_equal_i(r.damaged, 1);
}

void test_sport_recorder__closing_an_unfinished_activity(void) {
  SportJournalSummary summary;

  prv_start(START_UTC);
  prv_run(40);
  sport_recorder_test_lose_buffer();
  fake_pfs_mount();

  cl_assert(!sport_store_read_summary(START_UTC, &summary));
  cl_assert_equal_i(sport_store_close(START_UTC), 0);
  cl_assert(sport_store_read_summary(START_UTC, &summary));
  cl_assert(summary.flags & SportJournalStopFlag_Recovered);
  cl_assert_equal_i(summary.timer_s, 40);
  cl_assert_equal_i(summary.sport, SportJournalSport_Run);
}

void test_sport_recorder__discarding_deletes(void) {
  SportStoreEntry entries[4];

  prv_start(START_UTC);
  prv_run(40);
  sport_recorder_discard();
  cl_assert_equal_i(sport_store_list(entries, 4), 0);
}

void test_sport_recorder__the_oldest_go_past_the_limit(void) {
  SportStoreEntry entries[CONFIG_APP_SPORT_MAX_ACTIVITIES + 4];
  const size_t max = CONFIG_APP_SPORT_MAX_ACTIVITIES;

  for (uint32_t i = 0; i < max + 2; i++) {
    prv_start(START_UTC + i * 3600);
    prv_run(2);
    sport_engine_stop(&s_engine);
    sport_recorder_finish(&s_engine, s_engine.elapsed_s);
  }
  cl_assert_equal_i(sport_store_list(entries, PBL_ARRAY_SIZE(entries)), max);
  cl_assert_equal_i(entries[0].start_utc, START_UTC + 2 * 3600);
  cl_assert_equal_i(entries[max - 1].start_utc, START_UTC + (max + 1) * 3600);
}

static uint8_t s_fit[64 * 1024];
static size_t s_fit_len;

static void prv_fit_write(const void *data, size_t len, void *ctx) {
  cl_assert(s_fit_len + len <= sizeof(s_fit));
  memcpy(&s_fit[s_fit_len], data, len);
  s_fit_len += len;
}

void test_sport_recorder__export_uses_the_gnss_time(void) {
  uint32_t t;

  prv_start(START_UTC);
  prv_run(10);
  sport_recorder_time(START_UTC + 10 + 42, START_UTC + 10, 10);
  prv_run(50);
  sport_engine_stop(&s_engine);
  sport_recorder_lap(sport_engine_last_lap(&s_engine), s_engine.elapsed_s);
  sport_recorder_finish(&s_engine, s_engine.elapsed_s);

  s_fit_len = 0;
  const int size = sport_store_export_fit(START_UTC, prv_fit_write, NULL);
  cl_assert(size > 0);
  cl_assert_equal_i(size, s_fit_len);
  cl_assert_equal_i(sport_crc16(0, s_fit, s_fit_len), 0);
  // file_id's time_created, after the header, its definition and the record header.
  memcpy(&t, &s_fit[SPORT_FIT_HEADER_SIZE + 6 + 5 * 3 + 1 + 1 + 2 + 2 + 4], sizeof(t));
  cl_assert_equal_i(t, START_UTC + 42 - SPORT_FIT_EPOCH_OFFSET);

  // The sizing pass alone.
  cl_assert_equal_i(sport_store_export_fit(START_UTC, NULL, NULL), size);

  const char *dir = getenv("SPORT_FIT_OUT");
  if (dir != NULL) {
    char path[512];
    snprintf(path, sizeof(path), "%s/recorder_export.fit", dir);
    FILE *f = fopen(path, "wb");
    cl_assert(f != NULL);
    fwrite(s_fit, 1, s_fit_len, f);
    fclose(f);
  }
}
