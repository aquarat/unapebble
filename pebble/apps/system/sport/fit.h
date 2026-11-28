/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "journal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//! FIT activity files from the journal: file_id, timer events, a record for each second the timer
//! ran, laps, the session and the activity. Pure code, fed the journal's records in order.
//!
//! The header gives the size of the data, so a file is encoded twice: once with no output, which
//! sizes it, then for real with that size.
//!
//! @code{.c}
//! SportFitEncoder enc;
//! sport_fit_begin(&enc, &info, 0, NULL, NULL);
//! ... sport_fit_add(&enc, &rec) for each record ...
//! const uint32_t size = sport_fit_end(&enc);
//! sport_fit_begin(&enc, &info, size, write, ctx);
//! ... the records again ...
//! sport_fit_end(&enc);
//! @endcode

//! FIT time of the Unix epoch's 631065600 s: FIT counts from 1989-12-31 00:00 UTC.
#define SPORT_FIT_EPOCH_OFFSET 631065600U

#define SPORT_FIT_HEADER_SIZE 14

typedef void (*SportFitWrite)(const void *data, size_t len, void *ctx);

typedef struct SportFitInfo {
  //! SportJournalSport.
  uint8_t sport;
  //! UTC of the start, Unix seconds.
  uint32_t utc_start;
  //! Local time minus UTC, seconds, for the activity's local timestamp.
  int32_t utc_offset_s;
  uint32_t serial_number;
} SportFitInfo;

typedef struct SportFitEncoder {
  SportFitInfo info;
  SportFitWrite write;
  void *ctx;
  uint32_t size;
  uint16_t crc;
  //! Local message types defined so far, one bit each.
  uint8_t defined;
  bool started;
  bool timer_running;
  uint16_t laps;
  uint32_t lap_start_s;
  uint32_t last_distance_cm;
  SportJournalScan lap;
  SportJournalScan session;
} SportFitEncoder;

//! Start a file: the header, with @p total_size from a sizing pass (0 in it), and file_id.
void sport_fit_begin(SportFitEncoder *enc, const SportFitInfo *info, uint32_t total_size,
                     SportFitWrite write, void *ctx);

//! Add a record of the journal, already checked.
void sport_fit_add(SportFitEncoder *enc, const SportJournalRecord *rec);

//! End the file: the last lap if open, the session, the activity, the CRC. Returns its size.
uint32_t sport_fit_end(SportFitEncoder *enc);

//! Semicircles of a coordinate in 1e-7 degrees.
int32_t sport_fit_semicircles(int32_t e7);
