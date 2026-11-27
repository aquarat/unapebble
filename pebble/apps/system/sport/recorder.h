/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "engine.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <pbl/kernel/compiler.h>

//! Activity recording, in the filesystem:
//! - "/act<start>.<n>": segments of up to SPORT_RECORD_PER_SEGMENT one-second records, each after
//!   a SportRecordHeader;
//! - "/act<start>.s": a SportSummaryHeader, the totals, then the stored laps;
//! with <start> the UTC start time in 8 hex digits. Little-endian, for an exporter (FIT, GPX) to
//! read later. The oldest activities past SPORT_RECORD_MAX_ACTIVITIES are deleted.

#define SPORT_RECORD_MAGIC          0x54525053U // "SPRT"
#define SPORT_SUMMARY_MAGIC         0x4d535053U // "SPSM"
#define SPORT_RECORD_VERSION        1
#define SPORT_RECORD_PER_SEGMENT    900
#define SPORT_RECORD_MAX_ACTIVITIES 20

typedef enum SportRecordFlag {
  SportRecordFlag_Fix = 1 << 0,
  SportRecordFlag_Moving = 1 << 1,
  SportRecordFlag_Paused = 1 << 2,
  SportRecordFlag_AutoPaused = 1 << 3,
  SportRecordFlag_Lap = 1 << 4,
  SportRecordFlag_Elevation = 1 << 5,
  SportRecordFlag_Hr = 1 << 6,
} SportRecordFlag;

typedef struct PBL_PACKED SportRecordHeader {
  uint32_t magic;
  uint8_t version;
  uint8_t sport;
  uint16_t record_size;
  uint32_t start_utc;
  //! Elapsed second of the first record.
  uint32_t first_elapsed_s;
  //! Records written, set when the segment is closed; 0xffffffff while recording.
  uint32_t count;
} SportRecordHeader;

typedef struct PBL_PACKED SportRecord {
  int32_t lat_e7;
  int32_t lon_e7;
  uint32_t distance_cm;
  uint16_t speed_cm_s;
  //! Elevation in 0.2 m above -500 m.
  uint16_t elevation;
  uint16_t power_w;
  uint8_t hr_bpm;
  uint8_t cadence;
  uint8_t flags;
  uint8_t sats;
} SportRecord;

typedef struct PBL_PACKED SportSummaryHeader {
  uint32_t magic;
  uint8_t version;
  uint8_t sport;
  uint16_t num_laps_stored;
  uint32_t start_utc;
  uint32_t elapsed_s;
  uint16_t num_laps;
  uint16_t num_segments;
} SportSummaryHeader;

//! Pack the second the engine just processed.
void sport_record_pack(const SportEngine *engine, const SportSample *sample, bool lap,
                       SportRecord *record);

//! Start recording an activity.
bool sport_recorder_start(uint8_t sport, time_t start_utc);

//! Add one second.
void sport_recorder_add(const SportRecord *record, uint32_t elapsed_s);

//! Flush and write the summary.
void sport_recorder_finish(const SportEngine *engine);

//! Drop the activity being recorded.
void sport_recorder_discard(void);
