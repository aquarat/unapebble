/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pbl/kernel/compiler.h>

//! The activity journal, in the filesystem:
//! - "/wk<start>.<n>": segments of a SportJournalHeader and SPORT_JOURNAL_PER_SEGMENT records,
//!   created at their full size and filled in order; unwritten records read erased (0xff);
//! - "/wk<start>.s": a SportJournalSummary, written when the activity is closed;
//! with <start> the RTC's UTC start in 8 hex digits. Little-endian. Headers and records are 32
//! bytes, so none straddles a 512-byte block of the eMMC, and each ends with a CRC-16.

#define SPORT_JOURNAL_MAGIC         0x314a4b57U // "WKJ1"
#define SPORT_JOURNAL_SUMMARY_MAGIC 0x31534b57U // "WKS1"
#define SPORT_JOURNAL_VERSION       1
#define SPORT_JOURNAL_RECORD_SIZE   32
#define SPORT_JOURNAL_PER_SEGMENT   1024
#define SPORT_JOURNAL_SEGMENT_SIZE  (SPORT_JOURNAL_RECORD_SIZE * (1 + SPORT_JOURNAL_PER_SEGMENT))
//! An activity ends after this many segments, about 24 h at 1 Hz.
#define SPORT_JOURNAL_MAX_SEGMENTS 85
#define SPORT_JOURNAL_PATH_LEN     sizeof("/wk00000000.65535")
//! No elevation.
#define SPORT_JOURNAL_NO_ELEVATION 0xffffU

//! Sport recorded in the journal; never renumber.
typedef enum SportJournalSport {
  SportJournalSport_Run = 1,
  SportJournalSport_Bike = 2,
  SportJournalSport_Walk = 3,
} SportJournalSport;

typedef enum SportJournalType {
  SportJournalType_Sample = 1,
  SportJournalType_Event = 2,
  SportJournalType_Time = 3,
  //! Erased flash: nothing written yet.
  SportJournalType_Erased = 0xff,
} SportJournalType;

typedef enum SportJournalSampleFlag {
  SportJournalSampleFlag_Fix = 1 << 0,
  //! Timer running: the second counts.
  SportJournalSampleFlag_Running = 1 << 1,
  SportJournalSampleFlag_Paused = 1 << 2,
  SportJournalSampleFlag_AutoPaused = 1 << 3,
  SportJournalSampleFlag_Elevation = 1 << 4,
  SportJournalSampleFlag_Hr = 1 << 5,
  SportJournalSampleFlag_Cadence = 1 << 6,
  SportJournalSampleFlag_Power = 1 << 7,
} SportJournalSampleFlag;

typedef enum SportJournalEvent {
  SportJournalEvent_Start = 1,
  SportJournalEvent_Pause = 2,
  SportJournalEvent_Resume = 3,
  SportJournalEvent_AutoPause = 4,
  SportJournalEvent_AutoResume = 5,
  //! A lap closed, with its statistics.
  SportJournalEvent_Lap = 6,
  //! The activity ended, with its totals.
  SportJournalEvent_Stop = 7,
  //! Recording carried on after a restart; the activity is paused.
  SportJournalEvent_Restarted = 8,
} SportJournalEvent;

//! Stop event flags.
typedef enum SportJournalStopFlag {
  //! Closed from the journal after the recording was cut short.
  SportJournalStopFlag_Recovered = 1 << 0,
  //! The storage or the time limit ended the recording.
  SportJournalStopFlag_Truncated = 1 << 1,
} SportJournalStopFlag;

typedef struct PBL_PACKED SportJournalHeader {
  uint32_t magic;
  uint8_t version;
  uint8_t sport;
  uint16_t record_size;
  uint32_t start_utc;
  uint16_t segment;
  uint16_t capacity;
  uint8_t reserved[14];
  uint16_t crc;
} SportJournalHeader;

typedef struct PBL_PACKED SportJournalSample {
  int32_t lat_e7;
  int32_t lon_e7;
  //! Cumulative distance counted.
  uint32_t distance_cm;
  uint16_t speed_cm_s;
  //! 0.2 m above -500 m, the FIT altitude's scale, or SPORT_JOURNAL_NO_ELEVATION.
  uint16_t elevation;
  uint8_t hr_bpm;
  //! Steps a minute, or revolutions a minute on a bike.
  uint8_t cadence;
  uint8_t sats;
  uint8_t hdop_x10;
  uint16_t power_w;
} SportJournalSample;

//! Statistics of a lap (SportJournalEvent_Lap) or of the activity (SportJournalEvent_Stop).
typedef struct PBL_PACKED SportJournalStats {
  uint8_t event;
  //! Lap: the SportLapTrigger. Stop: SportJournalStopFlag bits.
  uint8_t info;
  //! Lap: its number. Stop: the laps closed.
  uint16_t laps;
  uint32_t timer_s;
  uint32_t distance_cm;
  uint16_t max_speed_cm_s;
  uint16_t ascent_dm;
  uint16_t descent_dm;
  uint8_t avg_hr;
  uint8_t max_hr;
  uint8_t avg_cadence;
  uint8_t reserved;
} SportJournalStats;

typedef struct PBL_PACKED SportJournalTime {
  uint32_t gnss_utc;
  uint32_t rtc_utc;
  uint8_t reserved[14];
} SportJournalTime;

typedef struct PBL_PACKED SportJournalRecord {
  uint8_t type;
  //! Sample: SportJournalSampleFlag bits.
  uint8_t flags;
  uint16_t reserved;
  //! Seconds since the start, pauses included.
  uint32_t time_s;
  union PBL_PACKED {
    SportJournalSample sample;
    SportJournalStats stats;
    SportJournalTime time;
    //! The event of a record that has no statistics.
    uint8_t event;
    uint8_t raw[22];
  };
  uint16_t crc;
} SportJournalRecord;

typedef struct PBL_PACKED SportJournalSummary {
  uint32_t magic;
  uint8_t version;
  uint8_t sport;
  //! SportJournalStopFlag bits.
  uint8_t flags;
  uint8_t has_gnss_time;
  uint32_t start_utc;
  //! UTC of the start from the GNSS, start_utc without one.
  uint32_t utc_start;
  uint32_t elapsed_s;
  uint32_t timer_s;
  uint32_t distance_cm;
  uint16_t laps;
  uint16_t crc;
} SportJournalSummary;

_Static_assert(sizeof(SportJournalHeader) == SPORT_JOURNAL_RECORD_SIZE, "header size");
_Static_assert(sizeof(SportJournalRecord) == SPORT_JOURNAL_RECORD_SIZE, "record size");
_Static_assert(sizeof(SportJournalSummary) == SPORT_JOURNAL_RECORD_SIZE, "summary size");

//! CRC-16 of the FIT protocol (CRC-16/ARC), continuing from @p crc.
uint16_t sport_crc16(uint16_t crc, const void *data, size_t len);

//! Set the CRC of a header, record or summary, its last two bytes.
void sport_journal_seal(void *block);

//! Whether a header, record or summary has a good CRC.
bool sport_journal_check(const void *block);

//! "/wk<start>.<segment>".
void sport_journal_segment_path(char *path, uint32_t start_utc, uint16_t segment);

//! "/wk<start>.s".
void sport_journal_summary_path(char *path, uint32_t start_utc);

//! The start of "wk<start>.0", the first segment of an activity, by name.
bool sport_journal_parse_first_segment(const char *name, uint32_t *start_utc);

//! Name of a sport, e.g. "Run".
const char *sport_journal_sport_name(uint8_t sport);

//! Statistics gathered from records, for activities, laps and recordings cut short.
typedef struct SportJournalScan {
  //! Valid records, and samples among them.
  uint32_t records;
  uint32_t samples;
  uint32_t first_time_s;
  uint32_t last_time_s;
  //! Flags of the last sample.
  uint8_t last_flags;
  //! A time record was seen: the UTC of the start it gives.
  bool has_time;
  uint32_t utc_start;
  //! The stop event, if any.
  bool stopped;
  SportJournalStats stop;
  //! Lap events seen.
  uint16_t laps;
  //! From the samples: seconds with the timer running, and what they covered.
  uint32_t timer_s;
  uint32_t distance_cm;
  uint32_t max_speed_cm_s;
  uint32_t ascent_dm;
  uint32_t descent_dm;
  uint32_t hr_sum;
  uint32_t hr_count;
  uint8_t hr_max;
  uint32_t cadence_sum;
  uint32_t cadence_count;
  //! Cumulative distance before the first sample, for a lap's share.
  uint32_t first_distance_cm;
  //! Elevation the climbs are counted from.
  bool has_elevation;
  uint16_t elevation_ref;
  //! Last position with a fix, and the first.
  bool has_position;
  int32_t first_lat_e7;
  int32_t first_lon_e7;
  int32_t last_lat_e7;
  int32_t last_lon_e7;
} SportJournalScan;

//! Start a scan from @p distance_cm of cumulative distance, 0 for a whole activity.
void sport_journal_scan_init(SportJournalScan *scan, uint32_t distance_cm);

//! Add a record, already checked.
void sport_journal_scan_add(SportJournalScan *scan, const SportJournalRecord *rec);

//! Statistics of what was scanned, from the samples. @p event is the event to set.
void sport_journal_scan_stats(const SportJournalScan *scan, uint8_t event, SportJournalStats *out);

//! Totals of an activity: its stop event's if it has one, the samples' otherwise.
void sport_journal_scan_totals(const SportJournalScan *scan, SportJournalStats *out);
