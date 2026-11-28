/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

// Written from the FIT protocol's public description: header, definition and data messages, the
// profile's message and field numbers, the CRC.

#include "fit.h"

#include <string.h>

#define PROTOCOL_VERSION 0x20
#define PROFILE_VERSION  2100

enum {
  BASE_ENUM = 0x00,
  BASE_UINT8 = 0x02,
  BASE_UINT16 = 0x84,
  BASE_SINT32 = 0x85,
  BASE_UINT32 = 0x86,
  BASE_UINT32Z = 0x8c,
};

enum {
  MESG_FILE_ID = 0,
  MESG_SESSION = 18,
  MESG_LAP = 19,
  MESG_RECORD = 20,
  MESG_EVENT = 21,
  MESG_ACTIVITY = 34,
};

enum {
  LOCAL_FILE_ID,
  LOCAL_EVENT,
  LOCAL_RECORD,
  LOCAL_LAP,
  LOCAL_SESSION,
  LOCAL_ACTIVITY,
};

enum {
  FIT_SPORT_GENERIC = 0,
  FIT_SPORT_RUNNING = 1,
  FIT_SPORT_CYCLING = 2,
  FIT_SPORT_WALKING = 11,
};

enum {
  FIT_EVENT_TIMER = 0,
  FIT_EVENT_SESSION = 8,
  FIT_EVENT_LAP = 9,
  FIT_EVENT_ACTIVITY = 26,
};

enum {
  FIT_EVENT_TYPE_START = 0,
  FIT_EVENT_TYPE_STOP = 1,
  FIT_EVENT_TYPE_STOP_ALL = 4,
};

enum {
  FIT_LAP_TRIGGER_MANUAL = 0,
  FIT_LAP_TRIGGER_DISTANCE = 2,
  FIT_LAP_TRIGGER_SESSION_END = 7,
};

#define FIT_MANUFACTURER_DEVELOPMENT 255
#define FIT_FILE_ACTIVITY            4

#define INVALID_UINT8  0xffU
#define INVALID_UINT16 0xffffU
#define INVALID_UINT32 0xffffffffU
#define INVALID_SINT32 0x7fffffff

typedef struct FitField {
  uint8_t num;
  uint8_t size;
  uint8_t base;
} FitField;

static const FitField s_file_id[] = {
  {0, 1, BASE_ENUM},    {1, 2, BASE_UINT16}, {2, 2, BASE_UINT16},
  {3, 4, BASE_UINT32Z}, {4, 4, BASE_UINT32},
};

static const FitField s_event[] = {
  {253, 4, BASE_UINT32},
  {0, 1, BASE_ENUM},
  {1, 1, BASE_ENUM},
};

static const FitField s_record[] = {
  {253, 4, BASE_UINT32}, {0, 4, BASE_SINT32}, {1, 4, BASE_SINT32}, {2, 2, BASE_UINT16},
  {3, 1, BASE_UINT8},    {4, 1, BASE_UINT8},  {5, 4, BASE_UINT32}, {6, 2, BASE_UINT16},
  {7, 2, BASE_UINT16},   {53, 1, BASE_UINT8},
};

static const FitField s_lap[] = {
  {253, 4, BASE_UINT32}, {2, 4, BASE_UINT32},  {3, 4, BASE_SINT32},  {4, 4, BASE_SINT32},
  {5, 4, BASE_SINT32},   {6, 4, BASE_SINT32},  {7, 4, BASE_UINT32},  {8, 4, BASE_UINT32},
  {9, 4, BASE_UINT32},   {13, 2, BASE_UINT16}, {14, 2, BASE_UINT16}, {15, 1, BASE_UINT8},
  {16, 1, BASE_UINT8},   {17, 1, BASE_UINT8},  {21, 2, BASE_UINT16}, {22, 2, BASE_UINT16},
  {0, 1, BASE_ENUM},     {1, 1, BASE_ENUM},    {24, 1, BASE_ENUM},   {25, 1, BASE_ENUM},
  {254, 2, BASE_UINT16},
};

static const FitField s_session[] = {
  {253, 4, BASE_UINT32}, {2, 4, BASE_UINT32},   {3, 4, BASE_SINT32},  {4, 4, BASE_SINT32},
  {7, 4, BASE_UINT32},   {8, 4, BASE_UINT32},   {9, 4, BASE_UINT32},  {14, 2, BASE_UINT16},
  {15, 2, BASE_UINT16},  {16, 1, BASE_UINT8},   {17, 1, BASE_UINT8},  {18, 1, BASE_UINT8},
  {22, 2, BASE_UINT16},  {23, 2, BASE_UINT16},  {25, 2, BASE_UINT16}, {26, 2, BASE_UINT16},
  {0, 1, BASE_ENUM},     {1, 1, BASE_ENUM},     {5, 1, BASE_ENUM},    {6, 1, BASE_ENUM},
  {28, 1, BASE_ENUM},    {254, 2, BASE_UINT16},
};

static const FitField s_activity[] = {
  {253, 4, BASE_UINT32}, {0, 4, BASE_UINT32}, {1, 2, BASE_UINT16}, {2, 1, BASE_ENUM},
  {3, 1, BASE_ENUM},     {4, 1, BASE_ENUM},   {5, 4, BASE_UINT32},
};

#define FIELDS(a) (a), (sizeof(a) / sizeof((a)[0]))

int32_t sport_fit_semicircles(int32_t e7) {
  // 2^31 semicircles are 180 degrees.
  const int64_t v = (int64_t)e7 * (1LL << 31);

  return (int32_t)(v >= 0 ? (v + 900000000LL) / 1800000000LL : (v - 900000000LL) / 1800000000LL);
}

static void prv_out(SportFitEncoder *enc, const void *data, size_t len) {
  enc->crc = sport_crc16(enc->crc, data, len);
  enc->size += (uint32_t)len;
  if (enc->write) {
    enc->write(data, len, enc->ctx);
  }
}

static void prv_define(SportFitEncoder *enc, uint8_t local, uint16_t global, const FitField *fields,
                       size_t n) {
  uint8_t hdr[6] = {0x40 | local, 0, 0, (uint8_t)global, (uint8_t)(global >> 8), (uint8_t)n};

  if (enc->defined & (1U << local)) {
    return;
  }
  enc->defined |= (uint8_t)(1U << local);
  prv_out(enc, hdr, sizeof(hdr));
  for (size_t i = 0; i < n; i++) {
    const uint8_t f[3] = {fields[i].num, fields[i].size, fields[i].base};
    prv_out(enc, f, sizeof(f));
  }
}

// A data message: values in the order of the fields, each as wide as its field.
static void prv_data(SportFitEncoder *enc, uint8_t local, const FitField *fields, size_t n,
                     const uint32_t *values) {
  const uint8_t hdr = local;

  prv_out(enc, &hdr, 1);
  for (size_t i = 0; i < n; i++) {
    uint8_t b[4];
    for (size_t j = 0; j < fields[i].size; j++) {
      b[j] = (uint8_t)(values[i] >> (8 * j));
    }
    prv_out(enc, b, fields[i].size);
  }
}

static uint32_t prv_time(const SportFitEncoder *enc, uint32_t time_s) {
  const uint32_t utc = enc->info.utc_start + time_s;

  return utc > SPORT_FIT_EPOCH_OFFSET ? utc - SPORT_FIT_EPOCH_OFFSET : 0;
}

static uint8_t prv_fit_sport(uint8_t sport) {
  switch (sport) {
    case SportJournalSport_Run:
      return FIT_SPORT_RUNNING;
    case SportJournalSport_Bike:
      return FIT_SPORT_CYCLING;
    case SportJournalSport_Walk:
      return FIT_SPORT_WALKING;
    default:
      return FIT_SPORT_GENERIC;
  }
}

// FIT counts a runner's cadence in strides, one foot's steps.
static bool prv_cadence_in_strides(const SportFitEncoder *enc) {
  return enc->info.sport == SportJournalSport_Run || enc->info.sport == SportJournalSport_Walk;
}

static uint32_t prv_avg_cadence(const SportFitEncoder *enc, uint8_t cadence) {
  if (cadence == 0) {
    return INVALID_UINT8;
  }
  return prv_cadence_in_strides(enc) ? (uint32_t)(cadence + 1) / 2 : cadence;
}

static uint32_t prv_u8_or_invalid(uint8_t v) {
  return v ? v : INVALID_UINT8;
}

static void prv_event(SportFitEncoder *enc, uint32_t time_s, uint8_t event, uint8_t type) {
  const uint32_t v[] = {prv_time(enc, time_s), event, type};

  prv_define(enc, LOCAL_EVENT, MESG_EVENT, FIELDS(s_event));
  prv_data(enc, LOCAL_EVENT, FIELDS(s_event), v);
}

static void prv_timer(SportFitEncoder *enc, uint32_t time_s, bool start) {
  if (start == enc->timer_running) {
    return;
  }
  enc->timer_running = start;
  prv_event(enc, time_s, FIT_EVENT_TIMER, start ? FIT_EVENT_TYPE_START : FIT_EVENT_TYPE_STOP_ALL);
}

void sport_fit_begin(SportFitEncoder *enc, const SportFitInfo *info, uint32_t total_size,
                     SportFitWrite write, void *ctx) {
  const uint32_t data_size =
      total_size > SPORT_FIT_HEADER_SIZE + 2 ? total_size - SPORT_FIT_HEADER_SIZE - 2 : 0;
  uint8_t hdr[SPORT_FIT_HEADER_SIZE] = {
    SPORT_FIT_HEADER_SIZE,
    PROTOCOL_VERSION,
    (uint8_t)PROFILE_VERSION,
    (uint8_t)(PROFILE_VERSION >> 8),
    (uint8_t)data_size,
    (uint8_t)(data_size >> 8),
    (uint8_t)(data_size >> 16),
    (uint8_t)(data_size >> 24),
    '.',
    'F',
    'I',
    'T',
  };

  *enc = (SportFitEncoder){.info = *info, .write = write, .ctx = ctx};
  sport_journal_scan_init(&enc->lap, 0);
  sport_journal_scan_init(&enc->session, 0);

  const uint16_t hcrc = sport_crc16(0, hdr, 12);
  hdr[12] = (uint8_t)hcrc;
  hdr[13] = (uint8_t)(hcrc >> 8);
  prv_out(enc, hdr, sizeof(hdr));

  const uint32_t v[] = {
    FIT_FILE_ACTIVITY, FIT_MANUFACTURER_DEVELOPMENT, 1,
    info->serial_number ? info->serial_number : 0, prv_time(enc, 0)
  };
  prv_define(enc, LOCAL_FILE_ID, MESG_FILE_ID, FIELDS(s_file_id));
  prv_data(enc, LOCAL_FILE_ID, FIELDS(s_file_id), v);
}

static void prv_record(SportFitEncoder *enc, const SportJournalRecord *rec) {
  const SportJournalSample *s = &rec->sample;
  const bool fix = rec->flags & SportJournalSampleFlag_Fix;
  const bool cadence = (rec->flags & SportJournalSampleFlag_Cadence) && s->cadence > 0;
  uint32_t cad = INVALID_UINT8;
  uint32_t frac = INVALID_UINT8;

  if (cadence) {
    if (prv_cadence_in_strides(enc)) {
      cad = s->cadence / 2U;
      frac = (s->cadence % 2U) ? 64 : 0;
    } else {
      cad = s->cadence;
    }
  }

  const uint32_t v[] = {
    prv_time(enc, rec->time_s),
    fix ? (uint32_t)sport_fit_semicircles(s->lat_e7) : (uint32_t)INVALID_SINT32,
    fix ? (uint32_t)sport_fit_semicircles(s->lon_e7) : (uint32_t)INVALID_SINT32,
    (rec->flags & SportJournalSampleFlag_Elevation) ? s->elevation : INVALID_UINT16,
    ((rec->flags & SportJournalSampleFlag_Hr) && s->hr_bpm) ? s->hr_bpm : INVALID_UINT8,
    cad,
    s->distance_cm,
    (uint32_t)s->speed_cm_s * 10U > 0xfffeU ? 0xfffeU : (uint32_t)s->speed_cm_s * 10U,
    (rec->flags & SportJournalSampleFlag_Power) ? s->power_w : INVALID_UINT16,
    frac,
  };

  prv_define(enc, LOCAL_RECORD, MESG_RECORD, FIELDS(s_record));
  prv_data(enc, LOCAL_RECORD, FIELDS(s_record), v);
}

static uint32_t prv_pos(const SportJournalScan *scan, bool first, bool lat) {
  if (!scan->has_position) {
    return (uint32_t)INVALID_SINT32;
  }
  const int32_t e7 = first ? (lat ? scan->first_lat_e7 : scan->first_lon_e7)
                           : (lat ? scan->last_lat_e7 : scan->last_lon_e7);
  return (uint32_t)sport_fit_semicircles(e7);
}

static uint32_t prv_avg_speed(const SportJournalStats *st) {
  if (st->timer_s == 0) {
    return 0;
  }
  const uint64_t mm_s = (uint64_t)st->distance_cm * 10U / st->timer_s;
  return mm_s > 0xfffeU ? 0xfffeU : (uint32_t)mm_s;
}

static void prv_lap(SportFitEncoder *enc, const SportJournalStats *st, uint32_t end_s,
                    uint8_t trigger) {
  const uint32_t v[] = {
    prv_time(enc, end_s),
    prv_time(enc, enc->lap_start_s),
    prv_pos(&enc->lap, true, true),
    prv_pos(&enc->lap, true, false),
    prv_pos(&enc->lap, false, true),
    prv_pos(&enc->lap, false, false),
    (end_s - enc->lap_start_s) * 1000U,
    st->timer_s * 1000U,
    st->distance_cm,
    prv_avg_speed(st),
    (uint32_t)st->max_speed_cm_s * 10U > 0xfffeU ? 0xfffeU : (uint32_t)st->max_speed_cm_s * 10U,
    prv_u8_or_invalid(st->avg_hr),
    prv_u8_or_invalid(st->max_hr),
    prv_avg_cadence(enc, st->avg_cadence),
    st->ascent_dm / 10U,
    st->descent_dm / 10U,
    FIT_EVENT_LAP,
    FIT_EVENT_TYPE_STOP,
    trigger,
    prv_fit_sport(enc->info.sport),
    enc->laps,
  };

  prv_define(enc, LOCAL_LAP, MESG_LAP, FIELDS(s_lap));
  prv_data(enc, LOCAL_LAP, FIELDS(s_lap), v);
  enc->laps++;
  enc->lap_start_s = end_s;
  sport_journal_scan_init(&enc->lap, enc->last_distance_cm);
}

static uint8_t prv_lap_trigger(uint8_t engine_trigger) {
  // SportLapTrigger: 0 auto (distance), 1 manual, 2 final.
  switch (engine_trigger) {
    case 0:
      return FIT_LAP_TRIGGER_DISTANCE;
    case 1:
      return FIT_LAP_TRIGGER_MANUAL;
    default:
      return FIT_LAP_TRIGGER_SESSION_END;
  }
}

void sport_fit_add(SportFitEncoder *enc, const SportJournalRecord *rec) {
  sport_journal_scan_add(&enc->session, rec);

  switch (rec->type) {
    case SportJournalType_Sample:
      sport_journal_scan_add(&enc->lap, rec);
      enc->last_distance_cm = rec->sample.distance_cm;
      if (rec->flags & SportJournalSampleFlag_Running) {
        if (!enc->started) {
          enc->started = true;
          enc->lap_start_s = rec->time_s;
        }
        prv_timer(enc, rec->time_s, true);
        prv_record(enc, rec);
      }
      break;
    case SportJournalType_Event:
      switch (rec->event) {
        case SportJournalEvent_Start:
        case SportJournalEvent_Resume:
        case SportJournalEvent_AutoResume:
          if (!enc->started) {
            enc->started = true;
            enc->lap_start_s = rec->time_s;
          }
          prv_timer(enc, rec->time_s, true);
          break;
        case SportJournalEvent_Pause:
        case SportJournalEvent_AutoPause:
        case SportJournalEvent_Restarted:
        case SportJournalEvent_Stop:
          prv_timer(enc, rec->time_s, false);
          break;
        case SportJournalEvent_Lap:
          prv_lap(enc, &rec->stats, rec->time_s, prv_lap_trigger(rec->stats.info));
          break;
        default:
          break;
      }
      break;
    default:
      break;
  }
}

uint32_t sport_fit_end(SportFitEncoder *enc) {
  const SportJournalScan *s = &enc->session;
  const uint32_t end_s = s->last_time_s;
  SportJournalStats totals;

  prv_timer(enc, end_s, false);

  // What the laps recorded leave over, as when a recording was cut short.
  if (enc->lap.samples > 0 &&
      (enc->lap.timer_s > 0 || enc->lap.distance_cm > 0 || enc->laps == 0)) {
    SportJournalStats lap;
    sport_journal_scan_stats(&enc->lap, SportJournalEvent_Lap, &lap);
    prv_lap(enc, &lap, end_s, FIT_LAP_TRIGGER_SESSION_END);
  }

  sport_journal_scan_totals(s, &totals);

  const uint32_t session[] = {
    prv_time(enc, end_s),
    prv_time(enc, s->first_time_s),
    prv_pos(s, true, true),
    prv_pos(s, true, false),
    (end_s - s->first_time_s) * 1000U,
    totals.timer_s * 1000U,
    totals.distance_cm,
    prv_avg_speed(&totals),
    (uint32_t)totals.max_speed_cm_s * 10U > 0xfffeU ? 0xfffeU
                                                    : (uint32_t)totals.max_speed_cm_s * 10U,
    prv_u8_or_invalid(totals.avg_hr),
    prv_u8_or_invalid(totals.max_hr),
    prv_avg_cadence(enc, totals.avg_cadence),
    totals.ascent_dm / 10U,
    totals.descent_dm / 10U,
    0,
    enc->laps,
    FIT_EVENT_SESSION,
    FIT_EVENT_TYPE_STOP,
    prv_fit_sport(enc->info.sport),
    0,
    0,
    0,
  };

  prv_define(enc, LOCAL_SESSION, MESG_SESSION, FIELDS(s_session));
  prv_data(enc, LOCAL_SESSION, FIELDS(s_session), session);

  const uint32_t ts = prv_time(enc, end_s);
  const uint32_t activity[] = {
    ts,
    totals.timer_s * 1000U,
    1,
    0,
    FIT_EVENT_ACTIVITY,
    FIT_EVENT_TYPE_STOP,
    (uint32_t)((int64_t)ts + enc->info.utc_offset_s),
  };

  prv_define(enc, LOCAL_ACTIVITY, MESG_ACTIVITY, FIELDS(s_activity));
  prv_data(enc, LOCAL_ACTIVITY, FIELDS(s_activity), activity);

  const uint16_t crc = enc->crc;
  const uint8_t c[2] = {(uint8_t)crc, (uint8_t)(crc >> 8)};
  prv_out(enc, c, sizeof(c));

  return enc->size;
}
