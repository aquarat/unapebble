/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "journal.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

uint16_t sport_crc16(uint16_t crc, const void *data, size_t len) {
  static const uint16_t s_table[16] = {
    0x0000, 0xcc01, 0xd801, 0x1400, 0xf001, 0x3c00, 0x2800, 0xe401,
    0xa001, 0x6c00, 0x7800, 0xb401, 0x5000, 0x9c01, 0x8801, 0x4400,
  };
  const uint8_t *p = data;

  for (size_t i = 0; i < len; i++) {
    uint16_t tmp = s_table[crc & 0xf];
    crc = (crc >> 4) & 0x0fff;
    crc = crc ^ tmp ^ s_table[p[i] & 0xf];
    tmp = s_table[crc & 0xf];
    crc = (crc >> 4) & 0x0fff;
    crc = crc ^ tmp ^ s_table[(p[i] >> 4) & 0xf];
  }

  return crc;
}

void sport_journal_seal(void *block) {
  uint8_t *p = block;
  const uint16_t crc = sport_crc16(0, p, SPORT_JOURNAL_RECORD_SIZE - 2);

  memcpy(&p[SPORT_JOURNAL_RECORD_SIZE - 2], &crc, sizeof(crc));
}

bool sport_journal_check(const void *block) {
  const uint8_t *p = block;
  uint16_t crc;

  memcpy(&crc, &p[SPORT_JOURNAL_RECORD_SIZE - 2], sizeof(crc));

  return crc == sport_crc16(0, p, SPORT_JOURNAL_RECORD_SIZE - 2);
}

void sport_journal_segment_path(char *path, uint32_t start_utc, uint16_t segment) {
  snprintf(path, SPORT_JOURNAL_PATH_LEN, "/wk%08" PRIx32 ".%u", start_utc, segment);
}

void sport_journal_summary_path(char *path, uint32_t start_utc) {
  snprintf(path, SPORT_JOURNAL_PATH_LEN, "/wk%08" PRIx32 ".s", start_utc);
}

bool sport_journal_parse_first_segment(const char *name, uint32_t *start_utc) {
  uint32_t v = 0;

  if (strlen(name) != 12 || strncmp(name, "wk", 2) != 0 || strcmp(&name[10], ".0") != 0) {
    return false;
  }
  for (size_t i = 2; i < 10; i++) {
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
  *start_utc = v;

  return true;
}

const char *sport_journal_sport_name(uint8_t sport) {
  switch (sport) {
    case SportJournalSport_Run:
      return "Run";
    case SportJournalSport_Bike:
      return "Bike";
    case SportJournalSport_Walk:
      return "Walk";
    default:
      return "Activity";
  }
}

// Elevation change, in 0.2 m units, counted as a climb: 1 m, the engine's smoothed elevation.
#define CLIMB_UNITS 5

void sport_journal_scan_init(SportJournalScan *scan, uint32_t distance_cm) {
  *scan = (SportJournalScan){.first_distance_cm = distance_cm};
}

static void prv_scan_sample(SportJournalScan *scan, const SportJournalRecord *rec) {
  const SportJournalSample *s = &rec->sample;

  scan->samples++;
  scan->last_flags = rec->flags;
  if (s->distance_cm >= scan->first_distance_cm) {
    scan->distance_cm = s->distance_cm - scan->first_distance_cm;
  }
  if (rec->flags & SportJournalSampleFlag_Fix) {
    if (!scan->has_position) {
      scan->has_position = true;
      scan->first_lat_e7 = s->lat_e7;
      scan->first_lon_e7 = s->lon_e7;
    }
    scan->last_lat_e7 = s->lat_e7;
    scan->last_lon_e7 = s->lon_e7;
  }

  const bool running = rec->flags & SportJournalSampleFlag_Running;

  if ((rec->flags & SportJournalSampleFlag_Elevation) &&
      s->elevation != SPORT_JOURNAL_NO_ELEVATION) {
    if (!scan->has_elevation) {
      scan->has_elevation = true;
      scan->elevation_ref = s->elevation;
    } else if (s->elevation >= scan->elevation_ref + CLIMB_UNITS) {
      if (running) {
        scan->ascent_dm += (uint32_t)(s->elevation - scan->elevation_ref) * 2;
      }
      scan->elevation_ref = s->elevation;
    } else if (s->elevation + CLIMB_UNITS <= scan->elevation_ref) {
      if (running) {
        scan->descent_dm += (uint32_t)(scan->elevation_ref - s->elevation) * 2;
      }
      scan->elevation_ref = s->elevation;
    }
  }

  if (!running) {
    return;
  }
  scan->timer_s++;
  if (s->speed_cm_s > scan->max_speed_cm_s) {
    scan->max_speed_cm_s = s->speed_cm_s;
  }
  if ((rec->flags & SportJournalSampleFlag_Hr) && s->hr_bpm > 0) {
    scan->hr_sum += s->hr_bpm;
    scan->hr_count++;
    if (s->hr_bpm > scan->hr_max) {
      scan->hr_max = s->hr_bpm;
    }
  }
  if ((rec->flags & SportJournalSampleFlag_Cadence) && s->cadence > 0) {
    scan->cadence_sum += s->cadence;
    scan->cadence_count++;
  }
}

void sport_journal_scan_add(SportJournalScan *scan, const SportJournalRecord *rec) {
  if (scan->records == 0) {
    scan->first_time_s = rec->time_s;
  }
  scan->records++;
  if (rec->time_s > scan->last_time_s) {
    scan->last_time_s = rec->time_s;
  }

  switch (rec->type) {
    case SportJournalType_Sample:
      prv_scan_sample(scan, rec);
      break;
    case SportJournalType_Time:
      if (rec->time.gnss_utc >= rec->time_s) {
        scan->has_time = true;
        scan->utc_start = rec->time.gnss_utc - rec->time_s;
      }
      break;
    case SportJournalType_Event:
      if (rec->event == SportJournalEvent_Lap) {
        scan->laps++;
      } else if (rec->event == SportJournalEvent_Stop) {
        scan->stopped = true;
        scan->stop = rec->stats;
      }
      break;
    default:
      break;
  }
}

static uint8_t prv_avg(uint32_t sum, uint32_t count) {
  return count ? (uint8_t)((sum + count / 2) / count) : 0;
}

void sport_journal_scan_stats(const SportJournalScan *scan, uint8_t event, SportJournalStats *out) {
  *out = (SportJournalStats){
    .event = event,
    .laps = scan->laps,
    .timer_s = scan->timer_s,
    .distance_cm = scan->distance_cm,
    .max_speed_cm_s =
        (uint16_t)(scan->max_speed_cm_s > UINT16_MAX ? UINT16_MAX : scan->max_speed_cm_s),
    .ascent_dm = (uint16_t)(scan->ascent_dm > UINT16_MAX ? UINT16_MAX : scan->ascent_dm),
    .descent_dm = (uint16_t)(scan->descent_dm > UINT16_MAX ? UINT16_MAX : scan->descent_dm),
    .avg_hr = prv_avg(scan->hr_sum, scan->hr_count),
    .max_hr = scan->hr_max,
    .avg_cadence = prv_avg(scan->cadence_sum, scan->cadence_count),
  };
}

void sport_journal_scan_totals(const SportJournalScan *scan, SportJournalStats *out) {
  if (scan->stopped) {
    *out = scan->stop;
    return;
  }
  sport_journal_scan_stats(scan, SportJournalEvent_Stop, out);
  out->info |= SportJournalStopFlag_Recovered;
}
