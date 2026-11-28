/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "recorder.h"

#include "store.h"

#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <string.h>

#include <pbl/fs/fs.h>
#include <pbl/kernel/malloc.h>
#include <pbl/kernel/mutex.h>
#include <pbl/kernel/reboot.h>
#include <pbl/logging/logging.h>
#include <pbl/util/math.h>

#define BUFFERED_RECORDS 32
// Free space kept in the filesystem beyond a new segment.
#define FREE_RESERVE (CONFIG_APP_SPORT_FREE_RESERVE_KIB * 1024U)
// Activities looked at when pruning.
#define MAX_SCANNED 64

_Static_assert(CONFIG_APP_SPORT_FLUSH_S < BUFFERED_RECORDS - 4, "flush interval");

typedef struct SportRecorder {
  bool active;
  bool truncated;
  uint8_t sport;
  uint32_t start_utc;
  uint16_t segment;
  //! Next record of the segment to write.
  uint16_t index;
  uint16_t buffered;
  uint16_t samples_buffered;
  SportJournalRecord buffer[BUFFERED_RECORDS];
} SportRecorder;

static PBL_MUTEX_DEFINE(s_lock);
static SportRecorder s_rec;

void sport_recorder_pack_stats(const SportStats *stats, uint8_t event, uint8_t info, uint16_t laps,
                               SportJournalStats *out) {
  *out = (SportJournalStats){
    .event = event,
    .info = info,
    .laps = laps,
    .timer_s = stats->timer_s,
    .distance_cm = stats->distance_mm / 10,
    .max_speed_cm_s = (uint16_t)PBL_MIN(stats->max_speed_mm_s / 10, UINT16_MAX),
    .ascent_dm = (uint16_t)PBL_MIN(stats->ascent_cm / 10, UINT16_MAX),
    .descent_dm = (uint16_t)PBL_MIN(stats->descent_cm / 10, UINT16_MAX),
    .avg_hr = sport_stats_avg_hr(stats),
    .max_hr = stats->hr_max,
    .avg_cadence = (uint8_t)PBL_MIN(sport_stats_avg_cadence(stats), UINT8_MAX),
  };
}

// Delete the oldest activities but the one recording until @p keep are left, or, with @p need,
// until the filesystem has that much free.
static void prv_prune(size_t keep, size_t need) {
  SportStoreEntry *entries = pbl_malloc(MAX_SCANNED * sizeof(*entries));
  size_t n;
  size_t first = 0;

  if (entries == NULL) {
    return;
  }
  n = PBL_MIN(sport_store_list(entries, MAX_SCANNED), MAX_SCANNED);

  for (size_t i = 0; i < n; i++) {
    const uint32_t start = entries[i].start_utc;
    struct pbl_fs_statvfs st;

    if (start == s_rec.start_utc && s_rec.active) {
      continue;
    }

    const size_t left = n - first;
    const bool too_many = left > keep;
    bool too_full = false;
    if (need > 0 && pbl_fs_statvfs("/", &st) == 0) {
      too_full = (size_t)st.f_bfree * st.f_frsize < need;
    }
    if (!too_many && !too_full) {
      break;
    }
    PBL_LOG_INFO("deleting activity %08" PRIx32 "%s", start, too_full ? ": storage full" : "");
    sport_store_delete(start);
    first++;
  }

  pbl_free(entries);
}

static bool prv_room_for_segment(void) {
  struct pbl_fs_statvfs st;
  const size_t need = SPORT_JOURNAL_SEGMENT_SIZE + FREE_RESERVE;

  if (pbl_fs_statvfs("/", &st) != 0) {
    return true;
  }
  if ((size_t)st.f_bfree * st.f_frsize >= need) {
    return true;
  }
  prv_prune(SIZE_MAX, need);

  return pbl_fs_statvfs("/", &st) == 0 && (size_t)st.f_bfree * st.f_frsize >= need;
}

static bool prv_create_segment(uint16_t segment) {
  char path[SPORT_JOURNAL_PATH_LEN];
  struct pbl_fs_file file;
  SportJournalHeader hdr = {
    .magic = SPORT_JOURNAL_MAGIC,
    .version = SPORT_JOURNAL_VERSION,
    .sport = s_rec.sport,
    .record_size = SPORT_JOURNAL_RECORD_SIZE,
    .start_utc = s_rec.start_utc,
    .segment = segment,
    .capacity = SPORT_JOURNAL_PER_SEGMENT,
  };
  int ret;

  memset(hdr.reserved, 0xff, sizeof(hdr.reserved));
  sport_journal_seal(&hdr);

  if (segment >= SPORT_JOURNAL_MAX_SEGMENTS || !prv_room_for_segment()) {
    PBL_LOG_WRN("activity %08" PRIx32 " ends at segment %u: %s", s_rec.start_utc, segment,
                segment >= SPORT_JOURNAL_MAX_SEGMENTS ? "time limit" : "storage full");
    return false;
  }

  sport_journal_segment_path(path, s_rec.start_utc, segment);
  ret = pbl_fs_open(&file, path, PBL_FS_O_WRITE | PBL_FS_O_CREATE);
  if (ret < 0) {
    PBL_LOG_ERR("could not create %s (%d)", path, ret);
    return false;
  }
  ret = pbl_fs_truncate(&file, SPORT_JOURNAL_SEGMENT_SIZE);
  if (ret == 0 && pbl_fs_write(&file, &hdr, sizeof(hdr)) != sizeof(hdr)) {
    ret = -EIO;
  }
  pbl_fs_close(&file);
  if (ret != 0) {
    PBL_LOG_ERR("could not create %s (%d)", path, ret);
    pbl_fs_unlink(path);
    return false;
  }

  s_rec.segment = segment;
  s_rec.index = 0;

  return true;
}

static void prv_flush(void) {
  char path[SPORT_JOURNAL_PATH_LEN];
  struct pbl_fs_file file;
  ssize_t n = -EIO;

  if (!s_rec.active || s_rec.buffered == 0) {
    return;
  }

  sport_journal_segment_path(path, s_rec.start_utc, s_rec.segment);
  if (pbl_fs_open(&file, path, PBL_FS_O_WRITE) == 0) {
    if (pbl_fs_seek(&file, (off_t)((1 + s_rec.index) * SPORT_JOURNAL_RECORD_SIZE),
                    PBL_FS_SEEK_SET) >= 0) {
      n = pbl_fs_write(&file, s_rec.buffer, s_rec.buffered * sizeof(SportJournalRecord));
    }
    pbl_fs_close(&file);
  }
  if (n != (ssize_t)(s_rec.buffered * sizeof(SportJournalRecord))) {
    PBL_LOG_ERR("could not write %u records to %s (%d)", s_rec.buffered, path, (int)n);
  }

  // Written or not, those slots are used: never program them twice.
  s_rec.index = (uint16_t)(s_rec.index + s_rec.buffered);
  s_rec.buffered = 0;
  s_rec.samples_buffered = 0;
}

static void prv_truncate(void) {
  SportJournalScan scan;
  SportJournalStats totals;
  uint8_t sport;

  prv_flush();
  s_rec.truncated = true;
  if (sport_store_scan(s_rec.start_utc, &sport, &scan) == 0) {
    sport_journal_scan_totals(&scan, &totals);
    totals.info = SportJournalStopFlag_Truncated;
    sport_store_write_summary(s_rec.start_utc, sport, &scan, &totals);
  }
}

static void prv_append(SportJournalRecord *rec, bool flush) {
  if (!s_rec.active || s_rec.truncated) {
    return;
  }

  if (s_rec.index + s_rec.buffered == SPORT_JOURNAL_PER_SEGMENT) {
    prv_flush();
    if (!prv_create_segment((uint16_t)(s_rec.segment + 1))) {
      prv_truncate();
      return;
    }
  }

  sport_journal_seal(rec);
  s_rec.buffer[s_rec.buffered++] = *rec;
  if (rec->type == SportJournalType_Sample) {
    s_rec.samples_buffered++;
  }
  if (flush || s_rec.samples_buffered >= CONFIG_APP_SPORT_FLUSH_S ||
      s_rec.buffered == BUFFERED_RECORDS) {
    prv_flush();
  }
}

static void prv_event_locked(uint8_t event, uint32_t time_s) {
  SportJournalRecord rec = {
    .type = SportJournalType_Event,
    .time_s = time_s,
  };

  memset(rec.raw, 0, sizeof(rec.raw));
  rec.event = event;
  prv_append(&rec, true);
}

bool sport_recorder_start(uint8_t sport, uint32_t start_utc) {
  bool ok;

  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  prv_flush();
  s_rec = (SportRecorder){.sport = sport, .start_utc = start_utc};
  prv_prune(CONFIG_APP_SPORT_MAX_ACTIVITIES - 1, 0);
  s_rec.active = prv_create_segment(0);
  prv_event_locked(SportJournalEvent_Start, 0);
  ok = s_rec.active;
  pbl_mutex_unlock(&s_lock);

  PBL_LOG_DBG("recording %08" PRIx32 ": %d", start_utc, ok);

  return ok;
}

bool sport_recorder_resume(uint32_t start_utc, uint32_t time_s) {
  SportJournalReader *r = pbl_malloc(sizeof(*r));
  SportJournalRecord rec;
  uint16_t segment;
  uint16_t index;
  bool ok = false;

  if (r == NULL) {
    return false;
  }

  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  prv_flush();
  s_rec = (SportRecorder){.start_utc = start_utc};
  if (sport_journal_reader_open(r, start_utc) == 0) {
    s_rec.sport = r->sport;
    while (sport_journal_reader_next(r, &rec) == 1) {
    }
    sport_journal_reader_end(r, &segment, &index);
    sport_journal_reader_close(r);
    s_rec.segment = segment;
    s_rec.index = index;
    s_rec.active = true;
    prv_event_locked(SportJournalEvent_Restarted, time_s);
    ok = s_rec.active && !s_rec.truncated;
  }
  pbl_mutex_unlock(&s_lock);
  pbl_free(r);

  PBL_LOG_INFO("resuming activity %08" PRIx32 " at segment %u record %u: %d", start_utc,
               s_rec.segment, s_rec.index, ok);

  return ok;
}

void sport_recorder_abandon(void) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  prv_flush();
  s_rec.active = false;
  pbl_mutex_unlock(&s_lock);
}

bool sport_recorder_is_active(uint32_t *start_utc) {
  bool active;

  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  active = s_rec.active;
  if (active && start_utc) {
    *start_utc = s_rec.start_utc;
  }
  pbl_mutex_unlock(&s_lock);

  return active;
}

bool sport_recorder_is_truncated(void) {
  return s_rec.active && s_rec.truncated;
}

void sport_recorder_sample(const SportEngine *engine, const SportSample *sample, uint32_t time_s) {
  SportJournalRecord rec = {
    .type = SportJournalType_Sample,
    .time_s = time_s,
  };
  SportJournalSample *s = &rec.sample;
  int32_t alt_cm;
  uint8_t flags = 0;

  memset(rec.raw, 0, sizeof(rec.raw));
  s->distance_cm = engine->total.distance_mm / 10;
  s->speed_cm_s = (uint16_t)PBL_MIN(sport_engine_current_speed_mm_s(engine) / 10, UINT16_MAX);
  s->sats = sample->sats;
  s->hdop_x10 = (uint8_t)PBL_MIN(sample->hdop_x10, UINT8_MAX);
  s->elevation = SPORT_JOURNAL_NO_ELEVATION;

  if (sample->flags & SportSampleFlag_Fix) {
    flags |= SportJournalSampleFlag_Fix;
    s->lat_e7 = sample->lat_e7;
    s->lon_e7 = sample->lon_e7;
  }
  if (sample->flags & SportSampleFlag_Hr) {
    flags |= SportJournalSampleFlag_Hr;
    s->hr_bpm = sample->hr_bpm;
  }
  if (sample->flags & SportSampleFlag_Cadence) {
    flags |= SportJournalSampleFlag_Cadence;
    s->cadence = (uint8_t)PBL_MIN(sample->cadence, UINT8_MAX);
  }
  if (sample->flags & SportSampleFlag_Power) {
    flags |= SportJournalSampleFlag_Power;
    s->power_w = sample->power_w;
  }
  if (sport_engine_elevation_cm(engine, &alt_cm)) {
    flags |= SportJournalSampleFlag_Elevation;
    s->elevation = (uint16_t)PBL_CLAMP((alt_cm + 50000) / 20, 0, UINT16_MAX - 1);
  }
  switch (engine->state) {
    case SportState_Running:
      flags |= SportJournalSampleFlag_Running;
      break;
    case SportState_Paused:
      flags |= SportJournalSampleFlag_Paused;
      break;
    case SportState_AutoPaused:
      flags |= SportJournalSampleFlag_AutoPaused;
      break;
    default:
      break;
  }
  rec.flags = flags;

  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  prv_append(&rec, false);
  pbl_mutex_unlock(&s_lock);
}

void sport_recorder_event(uint8_t event, uint32_t time_s) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  prv_event_locked(event, time_s);
  pbl_mutex_unlock(&s_lock);
}

void sport_recorder_lap(const SportLap *lap, uint32_t time_s) {
  SportJournalRecord rec = {
    .type = SportJournalType_Event,
    .time_s = time_s,
  };

  sport_recorder_pack_stats(&lap->stats, SportJournalEvent_Lap, lap->trigger, lap->number,
                            &rec.stats);
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  prv_append(&rec, true);
  pbl_mutex_unlock(&s_lock);
}

void sport_recorder_time(uint32_t gnss_utc, uint32_t rtc_utc, uint32_t time_s) {
  SportJournalRecord rec = {
    .type = SportJournalType_Time,
    .time_s = time_s,
  };

  memset(rec.raw, 0, sizeof(rec.raw));
  rec.time.gnss_utc = gnss_utc;
  rec.time.rtc_utc = rtc_utc;
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  prv_append(&rec, false);
  pbl_mutex_unlock(&s_lock);
}

void sport_recorder_flush(void) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  prv_flush();
  pbl_mutex_unlock(&s_lock);
}

void sport_recorder_finish(const SportEngine *engine, uint32_t time_s) {
  SportJournalRecord rec = {
    .type = SportJournalType_Event,
    .time_s = time_s,
  };
  SportJournalScan scan;
  uint8_t sport;

  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  if (!s_rec.active) {
    pbl_mutex_unlock(&s_lock);
    return;
  }
  sport_recorder_pack_stats(&engine->total, SportJournalEvent_Stop,
                            s_rec.truncated ? SportJournalStopFlag_Truncated : 0, engine->num_laps,
                            &rec.stats);
  prv_append(&rec, true);

  // The summary takes the GNSS's time from the journal.
  if (sport_store_scan(s_rec.start_utc, &sport, &scan) == 0) {
    sport_store_write_summary(s_rec.start_utc, s_rec.sport, &scan, &rec.stats);
  }
  PBL_LOG_DBG("recorded %08" PRIx32 ", %u segments", s_rec.start_utc, s_rec.segment + 1);
  s_rec.active = false;
  pbl_mutex_unlock(&s_lock);
}

void sport_recorder_discard(void) {
  pbl_mutex_lock(&s_lock, PBL_FOREVER);
  if (s_rec.active) {
    s_rec.buffered = 0;
    sport_store_delete(s_rec.start_utc);
    s_rec.active = false;
  }
  pbl_mutex_unlock(&s_lock);
}

#if UNITTEST
void sport_recorder_test_lose_buffer(void) {
  s_rec = (SportRecorder){};
}
#endif

// An orderly restart or power-off, the low-battery one among them, loses nothing buffered.
static void prv_sys_off(enum pbl_sys_off_mode mode, void *user_data) {
  if (pbl_mutex_lock(&s_lock, PBL_MSEC(1000)) != 0) {
    return;
  }
  prv_flush();
  pbl_mutex_unlock(&s_lock);
}

PBL_SYS_OFF_HANDLER_DEFINE(sport_recorder_restart, PBL_SYS_OFF_RESTART_PREPARE,
                           PBL_SYS_OFF_PRIO_HIGH, prv_sys_off, nullptr);
PBL_SYS_OFF_HANDLER_DEFINE(sport_recorder_power_off, PBL_SYS_OFF_POWER_OFF_PREPARE,
                           PBL_SYS_OFF_PRIO_HIGH, prv_sys_off, nullptr);
