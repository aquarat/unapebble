/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <apps/system/sport/fit.h>
#include <apps/system/sport/journal.h>
#include <clar.h>

// Synthetic journals encoded as FIT files, checked here for their framing and, with
// SPORT_FIT_OUT set to a directory, written there as sport_fit_*.fit for check_fit.py to decode
// with public decoders.

#define START_UTC     1795860000U // 2026-11-28 10:00:00 UTC
#define ORIGIN_LAT_E7 473769000
#define ORIGIN_LON_E7 85417000
// The GNSS's time is this far ahead of the RTC's.
#define GNSS_AHEAD_S 3

static SportJournalRecord s_records[2048];
static size_t s_count;

static uint8_t s_file[256 * 1024];
static size_t s_file_len;

static void prv_add(SportJournalRecord rec) {
  sport_journal_seal(&rec);
  cl_assert(sport_journal_check(&rec));
  s_records[s_count++] = rec;
}

static void prv_event(uint8_t event, uint32_t time_s) {
  SportJournalRecord rec = {.type = SportJournalType_Event, .time_s = time_s};
  rec.event = event;
  prv_add(rec);
}

static void prv_stats(uint8_t event, uint8_t info, uint16_t laps, uint32_t time_s, uint32_t timer_s,
                      uint32_t distance_cm) {
  SportJournalRecord rec = {.type = SportJournalType_Event, .time_s = time_s};
  rec.stats = (SportJournalStats){
    .event = event,
    .info = info,
    .laps = laps,
    .timer_s = timer_s,
    .distance_cm = distance_cm,
    .max_speed_cm_s = 300,
    .ascent_dm = 120,
    .descent_dm = 0,
    .avg_cadence = 170,
  };
  prv_add(rec);
}

static void prv_time(uint32_t time_s) {
  SportJournalRecord rec = {.type = SportJournalType_Time, .time_s = time_s};
  rec.time.gnss_utc = START_UTC + time_s + GNSS_AHEAD_S;
  rec.time.rtc_utc = START_UTC + time_s;
  prv_add(rec);
}

// North at 3 m/s from the origin; @p fix false for a second without a position.
static void prv_sample(uint32_t time_s, uint8_t state, uint32_t distance_cm, bool fix,
                       uint8_t cadence) {
  SportJournalRecord rec = {.type = SportJournalType_Sample, .time_s = time_s};
  SportJournalSample *s = &rec.sample;

  rec.flags = state | SportJournalSampleFlag_Elevation;
  s->distance_cm = distance_cm;
  s->speed_cm_s = (state & SportJournalSampleFlag_Running) ? 300 : 0;
  // 408 m, climbing 2 cm a second.
  s->elevation = (uint16_t)((40800 + 2 * time_s + 50000) / 20);
  s->sats = fix ? 8 : 0;
  s->hdop_x10 = fix ? 9 : 0;
  if (fix) {
    rec.flags |= SportJournalSampleFlag_Fix;
    // 1e-7 degrees of latitude are 11.1195 mm.
    s->lat_e7 = ORIGIN_LAT_E7 + (int32_t)((int64_t)distance_cm * 10 * 10000 / 111195);
    s->lon_e7 = ORIGIN_LON_E7;
  }
  if (cadence) {
    rec.flags |= SportJournalSampleFlag_Cadence;
    s->cadence = cadence;
  }
  prv_add(rec);
}

static void prv_write(const void *data, size_t len, void *ctx) {
  cl_assert(s_file_len + len <= sizeof(s_file));
  memcpy(&s_file[s_file_len], data, len);
  s_file_len += len;
}

static uint32_t prv_encode(uint8_t sport, uint32_t utc_start, const char *name) {
  SportFitEncoder enc;
  const SportFitInfo info = {.sport = sport, .utc_start = utc_start, .serial_number = 1234};

  sport_fit_begin(&enc, &info, 0, NULL, NULL);
  for (size_t i = 0; i < s_count; i++) {
    sport_fit_add(&enc, &s_records[i]);
  }
  const uint32_t size = sport_fit_end(&enc);

  s_file_len = 0;
  sport_fit_begin(&enc, &info, size, prv_write, NULL);
  for (size_t i = 0; i < s_count; i++) {
    sport_fit_add(&enc, &s_records[i]);
  }
  cl_assert_equal_i(sport_fit_end(&enc), size);
  cl_assert_equal_i(s_file_len, size);

  const char *dir = getenv("SPORT_FIT_OUT");
  if (dir != NULL) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    cl_assert(f != NULL);
    cl_assert_equal_i(fwrite(s_file, 1, s_file_len, f), s_file_len);
    fclose(f);
  }

  return size;
}

static void prv_check_framing(void) {
  uint32_t data_size;

  cl_assert_equal_i(s_file[0], SPORT_FIT_HEADER_SIZE);
  cl_assert(memcmp(&s_file[8], ".FIT", 4) == 0);
  memcpy(&data_size, &s_file[4], sizeof(data_size));
  cl_assert_equal_i(data_size, s_file_len - SPORT_FIT_HEADER_SIZE - 2);
  // A CRC over the header with its CRC, and over the file with its CRC, is 0.
  cl_assert_equal_i(sport_crc16(0, s_file, SPORT_FIT_HEADER_SIZE), 0);
  cl_assert_equal_i(sport_crc16(0, s_file, s_file_len), 0);
}

void test_sport_fit__initialize(void) {
  s_count = 0;
  s_file_len = 0;
}

void test_sport_fit__crc16_is_the_protocols(void) {
  // CRC-16/ARC's check value.
  cl_assert_equal_i(sport_crc16(0, "123456789", 9), 0xbb3d);
}

void test_sport_fit__semicircles(void) {
  cl_assert_equal_i(sport_fit_semicircles(0), 0);
  cl_assert_equal_i(sport_fit_semicircles(900000000), 1 << 30);
  cl_assert_equal_i(sport_fit_semicircles(-900000000), -(1 << 30));
  cl_assert_equal_i(sport_fit_semicircles(473769000), 565228434);
}

void test_sport_fit__journal_records_are_sealed(void) {
  SportJournalRecord rec = {.type = SportJournalType_Sample, .time_s = 42};

  sport_journal_seal(&rec);
  cl_assert(sport_journal_check(&rec));
  rec.sample.distance_cm ^= 1;
  cl_assert(!sport_journal_check(&rec));
}

void test_sport_fit__names(void) {
  char path[SPORT_JOURNAL_PATH_LEN];
  uint32_t start;

  sport_journal_segment_path(path, 0x6a4b3c20, 7);
  cl_assert_equal_s(path, "/wk6a4b3c20.7");
  sport_journal_summary_path(path, 0x6a4b3c20);
  cl_assert_equal_s(path, "/wk6a4b3c20.s");
  cl_assert(sport_journal_parse_first_segment("wk6a4b3c20.0", &start));
  cl_assert_equal_i(start, 0x6a4b3c20);
  cl_assert(!sport_journal_parse_first_segment("wk6a4b3c20.1", &start));
  cl_assert(!sport_journal_parse_first_segment("wk6a4b3c20.s", &start));
  cl_assert(!sport_journal_parse_first_segment("act6a4b3c2.0", &start));
}

// A run: 300 s, a pause of 30 s, 270 s more with a manual lap at 400 s, a final lap, a stop.
void test_sport_fit__run_with_pause_and_laps(void) {
  uint32_t dist = 0;
  SportJournalScan scan;

  prv_event(SportJournalEvent_Start, 0);
  for (uint32_t t = 1; t <= 600; t++) {
    if (t == 5) {
      prv_time(t);
    }
    if (t == 301) {
      prv_event(SportJournalEvent_Pause, t);
    }
    if (t == 331) {
      prv_event(SportJournalEvent_Resume, t);
    }
    const bool paused = t >= 301 && t <= 330;
    if (!paused) {
      dist += 300;
    }
    prv_sample(t, paused ? SportJournalSampleFlag_Paused : SportJournalSampleFlag_Running, dist,
               true, 171);
    if (t == 400) {
      // 300 s before the pause, 70 after it.
      prv_stats(SportJournalEvent_Lap, 1, 1, t, 370, 370 * 300);
    }
  }
  prv_stats(SportJournalEvent_Lap, 2, 2, 600, 200, 200 * 300);
  prv_stats(SportJournalEvent_Stop, 0, 2, 600, 570, 570 * 300);

  sport_journal_scan_init(&scan, 0);
  for (size_t i = 0; i < s_count; i++) {
    sport_journal_scan_add(&scan, &s_records[i]);
  }
  cl_assert(scan.stopped);
  cl_assert(scan.has_time);
  cl_assert_equal_i(scan.utc_start, START_UTC + GNSS_AHEAD_S);
  cl_assert_equal_i(scan.timer_s, 570);
  cl_assert_equal_i(scan.distance_cm, 570 * 300);
  cl_assert_equal_i(scan.laps, 2);

  prv_encode(SportJournalSport_Run, scan.utc_start, "sport_fit_run.fit");
  prv_check_framing();
}

// Cut short by a restart: no laps nor stop; the first minute indoors, without a fix.
void test_sport_fit__recovered_walk_without_stop(void) {
  uint32_t dist = 0;
  SportJournalScan scan;
  SportJournalStats totals;

  prv_event(SportJournalEvent_Start, 0);
  for (uint32_t t = 1; t <= 180; t++) {
    dist += 140;
    prv_sample(t, SportJournalSampleFlag_Running, dist, t > 60, 110);
  }

  sport_journal_scan_init(&scan, 0);
  for (size_t i = 0; i < s_count; i++) {
    sport_journal_scan_add(&scan, &s_records[i]);
  }
  cl_assert(!scan.stopped);
  cl_assert(!scan.has_time);
  sport_journal_scan_totals(&scan, &totals);
  cl_assert(totals.info & SportJournalStopFlag_Recovered);
  cl_assert_equal_i(totals.timer_s, 180);
  cl_assert_equal_i(totals.distance_cm, 180 * 140);
  cl_assert_equal_i(totals.avg_cadence, 110);
  // 2 cm a second for 180 s: 3.6 m in climbs of 1 m.
  cl_assert_equal_i(totals.ascent_dm, 30);

  prv_encode(SportJournalSport_Walk, START_UTC, "sport_fit_walk.fit");
  prv_check_framing();
}

// A ride resumed after a restart, with an auto-pause.
void test_sport_fit__bike_restarted(void) {
  uint32_t dist = 0;

  prv_event(SportJournalEvent_Start, 0);
  for (uint32_t t = 1; t <= 120; t++) {
    dist += 300;
    prv_sample(t, SportJournalSampleFlag_Running, dist, true, 0);
  }
  prv_event(SportJournalEvent_Restarted, 200);
  prv_event(SportJournalEvent_Resume, 210);
  for (uint32_t t = 211; t <= 300; t++) {
    if (t == 250) {
      prv_event(SportJournalEvent_AutoPause, t);
    }
    if (t == 260) {
      prv_event(SportJournalEvent_AutoResume, t);
    }
    const bool paused = t >= 250 && t < 260;
    if (!paused) {
      dist += 300;
    }
    prv_sample(t, paused ? SportJournalSampleFlag_AutoPaused : SportJournalSampleFlag_Running, dist,
               true, 0);
  }
  prv_stats(SportJournalEvent_Lap, 2, 1, 300, 200, 200 * 300);
  prv_stats(SportJournalEvent_Stop, 0, 1, 300, 200, 200 * 300);

  prv_encode(SportJournalSport_Bike, START_UTC, "sport_fit_bike.fit");
  prv_check_framing();
}
