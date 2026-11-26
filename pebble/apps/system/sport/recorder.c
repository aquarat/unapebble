/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "recorder.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <pbl/fs/fs.h>
#include <pbl/logging/logging.h>
#include <pbl/util/math.h>
#include <pbl/util/size.h>

#include <applib/app_malloc.h>

// Records buffered in RAM between writes.
#define BUFFERED_RECORDS 30
#define PATH_LEN         sizeof("/act00000000.65535")
// Activities looked at when pruning.
#define MAX_SCANNED 64

typedef struct SportRecorder {
  bool active;
  uint8_t sport;
  uint32_t start_utc;
  uint16_t segment;
  uint16_t in_segment;
  uint16_t buffered;
  SportRecord buffer[BUFFERED_RECORDS];
} SportRecorder;

static SportRecorder *s_rec;

void sport_record_pack(const SportEngine *engine, const SportSample *sample, bool lap,
                       SportRecord *record) {
  int32_t alt_cm;
  uint8_t flags = 0;

  *record = (SportRecord){
    .distance_cm = engine->total.distance_mm / 10,
    .speed_cm_s = (uint16_t)PBL_MIN(sport_engine_current_speed_mm_s(engine) / 10, UINT16_MAX),
    .power_w = (sample->flags & SportSampleFlag_Power) ? sample->power_w : 0,
    .cadence = (uint8_t)PBL_MIN(sample->cadence, UINT8_MAX),
    .sats = sample->sats,
  };

  if (sample->flags & SportSampleFlag_Fix) {
    flags |= SportRecordFlag_Fix;
    record->lat_e7 = sample->lat_e7;
    record->lon_e7 = sample->lon_e7;
  }
  if (sample->flags & SportSampleFlag_Hr) {
    flags |= SportRecordFlag_Hr;
    record->hr_bpm = sample->hr_bpm;
  }
  if (sport_engine_elevation_cm(engine, &alt_cm)) {
    flags |= SportRecordFlag_Elevation;
    record->elevation = (uint16_t)PBL_CLAMP((alt_cm + 50000) / 20, 0, UINT16_MAX);
  }
  switch (engine->state) {
    case SportState_Running:
      flags |= SportRecordFlag_Moving;
      break;
    case SportState_Paused:
      flags |= SportRecordFlag_Paused;
      break;
    case SportState_AutoPaused:
      flags |= SportRecordFlag_AutoPaused;
      break;
    default:
      break;
  }
  if (lap) {
    flags |= SportRecordFlag_Lap;
  }
  record->flags = flags;
}

static void prv_path(char *path, uint32_t start_utc, const char *suffix) {
  snprintf(path, PATH_LEN, "/act%08" PRIx32 ".%s", start_utc, suffix);
}

static void prv_segment_path(char *path, uint32_t start_utc, uint16_t segment) {
  char suffix[6];

  snprintf(suffix, sizeof(suffix), "%u", segment);
  prv_path(path, start_utc, suffix);
}

static void prv_remove_activity(uint32_t start_utc) {
  char path[PATH_LEN];

  prv_path(path, start_utc, "s");
  pbl_fs_unlink(path);
  for (uint16_t seg = 0;; seg++) {
    prv_segment_path(path, start_utc, seg);
    if (pbl_fs_unlink(path) < 0) {
      break;
    }
  }
}

// "act<start>.0", the first segment of an activity.
static bool prv_parse_first_segment(const char *name, uint32_t *start) {
  uint32_t v = 0;

  if (strlen(name) != 13 || strncmp(name, "act", 3) != 0 || strcmp(&name[11], ".0") != 0) {
    return false;
  }
  for (size_t i = 3; i < 11; i++) {
    const char c = name[i];
    uint32_t digit;
    if (c >= '0' && c <= '9') {
      digit = (uint32_t)(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = (uint32_t)(c - 'a' + 10);
    } else {
      return false;
    }
    v = v << 4 | digit;
  }
  *start = v;

  return true;
}

// Delete the oldest activities so that a new one fits in the limit.
static void prv_prune(void) {
  struct pbl_fs_dir dir;
  struct pbl_fs_dirent *entry = app_malloc(sizeof(*entry));
  uint32_t *starts = app_malloc(MAX_SCANNED * sizeof(*starts));
  size_t n = 0;

  if (entry == NULL || starts == NULL || pbl_fs_opendir(&dir, "/") < 0) {
    app_free(entry);
    app_free(starts);
    return;
  }

  while (pbl_fs_readdir(&dir, entry) == 0 && entry->name[0] != '\0' && n < MAX_SCANNED) {
    uint32_t start;

    // Every activity has a first segment, the summary may be missing after a crash.
    if (prv_parse_first_segment(entry->name, &start)) {
      starts[n++] = start;
    }
  }
  pbl_fs_closedir(&dir);

  while (n >= SPORT_RECORD_MAX_ACTIVITIES) {
    size_t oldest = 0;
    for (size_t i = 1; i < n; i++) {
      if (starts[i] < starts[oldest]) {
        oldest = i;
      }
    }
    PBL_LOG_DBG("deleting activity %08" PRIx32, starts[oldest]);
    prv_remove_activity(starts[oldest]);
    starts[oldest] = starts[--n];
  }

  app_free(starts);
  app_free(entry);
}

static bool prv_open_segment(uint32_t first_elapsed_s) {
  char path[PATH_LEN];
  struct pbl_fs_file file;
  const SportRecordHeader hdr = {
    .magic = SPORT_RECORD_MAGIC,
    .version = SPORT_RECORD_VERSION,
    .sport = s_rec->sport,
    .record_size = sizeof(SportRecord),
    .start_utc = s_rec->start_utc,
    .first_elapsed_s = first_elapsed_s,
    .count = UINT32_MAX,
  };

  prv_segment_path(path, s_rec->start_utc, s_rec->segment);
  if (pbl_fs_open(&file, path, PBL_FS_O_WRITE | PBL_FS_O_CREATE) < 0) {
    return false;
  }

  const int ret =
      pbl_fs_truncate(&file, (off_t)(sizeof(hdr) + SPORT_RECORD_PER_SEGMENT * sizeof(SportRecord)));
  const bool ok = ret == 0 && pbl_fs_write(&file, &hdr, sizeof(hdr)) == sizeof(hdr);

  pbl_fs_close(&file);
  if (!ok) {
    PBL_LOG_ERR("could not create %s (%d)", path, ret);
  }

  return ok;
}

static void prv_flush(void) {
  char path[PATH_LEN];
  struct pbl_fs_file file;
  const size_t first = s_rec->in_segment - s_rec->buffered;

  if (s_rec->buffered == 0) {
    return;
  }

  prv_segment_path(path, s_rec->start_utc, s_rec->segment);
  if (pbl_fs_open(&file, path, PBL_FS_O_WRITE) < 0) {
    PBL_LOG_ERR("could not open %s", path);
    s_rec->buffered = 0;
    return;
  }

  pbl_fs_seek(&file, (off_t)(sizeof(SportRecordHeader) + first * sizeof(SportRecord)),
              PBL_FS_SEEK_SET);
  pbl_fs_write(&file, s_rec->buffer, s_rec->buffered * sizeof(SportRecord));
  pbl_fs_close(&file);
  s_rec->buffered = 0;
}

// Erased flash reads 0xff, so the count is written once, over the placeholder.
static void prv_close_segment(void) {
  char path[PATH_LEN];
  struct pbl_fs_file file;
  const uint32_t count = s_rec->in_segment;

  prv_flush();
  prv_segment_path(path, s_rec->start_utc, s_rec->segment);
  if (pbl_fs_open(&file, path, PBL_FS_O_WRITE) < 0) {
    return;
  }
  pbl_fs_seek(&file, offsetof(SportRecordHeader, count), PBL_FS_SEEK_SET);
  pbl_fs_write(&file, &count, sizeof(count));
  pbl_fs_close(&file);
}

bool sport_recorder_start(uint8_t sport, time_t start_utc) {
  if (s_rec == NULL) {
    s_rec = app_zalloc(sizeof(*s_rec));
    if (s_rec == NULL) {
      return false;
    }
  }

  prv_prune();

  *s_rec = (SportRecorder){.sport = sport, .start_utc = (uint32_t)start_utc};
  s_rec->active = prv_open_segment(0);
  PBL_LOG_DBG("recording %08" PRIx32 ": %d", s_rec->start_utc, s_rec->active);

  return s_rec->active;
}

void sport_recorder_add(const SportRecord *record, uint32_t elapsed_s) {
  if (s_rec == NULL || !s_rec->active) {
    return;
  }

  if (s_rec->in_segment == SPORT_RECORD_PER_SEGMENT) {
    prv_close_segment();
    s_rec->segment++;
    s_rec->in_segment = 0;
    if (!prv_open_segment(elapsed_s)) {
      s_rec->active = false;
      return;
    }
  }

  s_rec->buffer[s_rec->buffered++] = *record;
  s_rec->in_segment++;
  if (s_rec->buffered == BUFFERED_RECORDS) {
    prv_flush();
  }
}

void sport_recorder_finish(const SportEngine *engine) {
  char path[PATH_LEN];
  struct pbl_fs_file file;

  if (s_rec == NULL) {
    return;
  }
  if (!s_rec->active) {
    app_free(s_rec);
    s_rec = NULL;
    return;
  }

  prv_close_segment();

  const uint16_t stored = (uint16_t)PBL_MIN(engine->num_laps, SPORT_ENGINE_MAX_LAPS);
  const SportSummaryHeader hdr = {
    .magic = SPORT_SUMMARY_MAGIC,
    .version = SPORT_RECORD_VERSION,
    .sport = s_rec->sport,
    .num_laps_stored = stored,
    .start_utc = s_rec->start_utc,
    .elapsed_s = engine->elapsed_s,
    .num_laps = engine->num_laps,
    .num_segments = (uint16_t)(s_rec->segment + 1),
  };

  prv_path(path, s_rec->start_utc, "s");
  if (pbl_fs_open(&file, path, PBL_FS_O_REPLACE | PBL_FS_O_CREATE) == 0) {
    pbl_fs_truncate(&file,
                    (off_t)(sizeof(hdr) + sizeof(engine->total) + stored * sizeof(SportLap)));
    pbl_fs_write(&file, &hdr, sizeof(hdr));
    pbl_fs_write(&file, &engine->total, sizeof(engine->total));
    pbl_fs_write(&file, engine->laps, stored * sizeof(SportLap));
    pbl_fs_close(&file);
  } else {
    PBL_LOG_ERR("could not write %s", path);
  }

  PBL_LOG_DBG("recorded %08" PRIx32 ", %u segments", s_rec->start_utc, hdr.num_segments);
  app_free(s_rec);
  s_rec = NULL;
}

void sport_recorder_discard(void) {
  if (s_rec == NULL) {
    return;
  }

  if (s_rec->active) {
    prv_remove_activity(s_rec->start_utc);
  }
  app_free(s_rec);
  s_rec = NULL;
}
