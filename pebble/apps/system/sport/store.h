/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "fit.h"
#include "journal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pbl/fs/fs.h>

//! The activities in the filesystem: reading journals back, listing, closing and deleting them,
//! and their FIT files.

#define SPORT_STORE_READ_RECORDS 16

//! Reads the records of an activity in order, across its segments.
typedef struct SportJournalReader {
  uint32_t start_utc;
  uint8_t sport;
  uint16_t segment;
  //! Next record of the segment.
  uint16_t index;
  uint16_t buffered;
  uint16_t pos;
  bool open;
  bool end;
  //! Records whose CRC failed, skipped.
  uint32_t damaged;
  struct pbl_fs_file file;
  SportJournalRecord buf[SPORT_STORE_READ_RECORDS];
} SportJournalReader;

//! 0 if the activity has a first segment of a format this firmware reads; -ENOENT, -EINVAL.
int sport_journal_reader_open(SportJournalReader *r, uint32_t start_utc);

//! 0 with the reader at record @p index of segment @p segment, which may be the end of a full
//! segment; -ENOENT, -EINVAL.
int sport_journal_reader_seek(SportJournalReader *r, uint32_t start_utc, uint16_t segment,
                              uint16_t index);

//! 1 with the next record, 0 at the end.
int sport_journal_reader_next(SportJournalReader *r, SportJournalRecord *rec);

//! Where the next record goes: the segment and its index there, once next() returned 0.
void sport_journal_reader_end(const SportJournalReader *r, uint16_t *segment, uint16_t *index);

void sport_journal_reader_close(SportJournalReader *r);

typedef struct SportStoreEntry {
  uint32_t start_utc;
  uint8_t sport;
  //! Whether it has a summary, written when it was closed.
  bool closed;
  //! Whether this firmware reads its format.
  bool readable;
  SportJournalSummary summary;
} SportStoreEntry;

//! The activities, oldest first, up to @p max. Returns how many there are, which may be more.
size_t sport_store_list(SportStoreEntry *entries, size_t max);

//! Read an activity's summary; false if it has none.
bool sport_store_read_summary(uint32_t start_utc, SportJournalSummary *summary);

//! Scan a whole activity. 0, or what sport_journal_reader_open() returned.
int sport_store_scan(uint32_t start_utc, uint8_t *sport, SportJournalScan *scan);

//! Close an activity that has no summary, from its journal: the summary, marked recovered.
int sport_store_close(uint32_t start_utc);

//! Write the summary of an activity from its totals.
int sport_store_write_summary(uint32_t start_utc, uint8_t sport, const SportJournalScan *scan,
                              const SportJournalStats *totals);

//! Delete an activity, segments and summary.
void sport_store_delete(uint32_t start_utc);

//! Encode an activity as a FIT file, through @p write, twice if @p write is set: once to size it.
//! Returns the file's size, or a negative errno.
int sport_store_export_fit(uint32_t start_utc, SportFitWrite write, void *ctx);
