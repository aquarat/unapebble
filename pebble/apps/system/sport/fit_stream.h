/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "fit.h"
#include "store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//! An activity's FIT file read at any offset, as a drive's host reads it, encoded from the journal
//! as it is read: the same bytes as sport_store_export_fit(), never stored.
//!
//! Opening it sizes the file, one pass over the journal, and keeps marks along it: the encoder's
//! state and the journal's position every so many bytes. A read carries on from where the last
//! one ended, or from the last mark before it, so that no read encodes more than a mark's
//! distance it does not return. The marks of the sizing pass lack the file's CRC, which depends on
//! its header and so on its size: the last two bytes come from a pass from the start, which keeps
//! the CRC for the next reads.

#define SPORT_FIT_STREAM_STAGE 1024

//! Where an encoding can carry on from.
typedef struct SportFitMark {
  //! Bytes of the file before it, 0 for none.
  uint32_t offset;
  //! The next record of the journal.
  uint16_t segment;
  uint16_t index;
  //! Whether the encoder's CRC is the file's.
  bool crc_valid;
  SportFitEncoder enc;
} SportFitMark;

typedef struct SportFitStream {
  uint32_t start_utc;
  SportFitInfo info;
  uint32_t size;
  //! The file's CRC, once a pass from the start ended.
  bool crc_known;
  uint16_t crc;
  SportFitMark *marks;
  uint16_t num_marks;
  uint32_t mark_every;
  //! Statistics: records encoded, and passes started over from a mark or the start.
  uint32_t records;
  uint32_t restarts;
  //! The encoding under way.
  bool live;
  bool ended;
  bool crc_valid;
  SportJournalReader reader;
  SportFitEncoder enc;
  //! Encoded bytes not read yet, from stage_off; bytes before skip_to are not kept.
  uint32_t stage_off;
  uint32_t stage_len;
  uint32_t skip_to;
  //! The last two bytes encoded, the CRC at the end; a stage that overflowed.
  uint8_t last[2];
  bool overflow;
  uint8_t stage[SPORT_FIT_STREAM_STAGE];
} SportFitStream;

//! Size an activity's file: 0, -ENOENT, -EINVAL as sport_journal_reader_open(), or -EIO. @p marks
//! has room for @p num_marks marks, one each @p mark_every bytes; a longer file has marks only
//! for its start.
int sport_fit_stream_open(SportFitStream *s, uint32_t start_utc, SportFitMark *marks,
                          uint16_t num_marks, uint32_t mark_every);

//! Like sport_fit_stream_open(), from what an earlier stream of the activity found: no sizing
//! pass. @p marks are kept, those of the earlier stream or zeros: more come with the reads.
void sport_fit_stream_reopen(SportFitStream *s, uint32_t start_utc, uint32_t utc_start,
                             uint32_t size, bool crc_known, uint16_t crc, SportFitMark *marks,
                             uint16_t num_marks, uint32_t mark_every);

//! Read @p len bytes from @p offset, all within the file: 0, or a negative errno.
int sport_fit_stream_read(SportFitStream *s, uint32_t offset, void *buf, uint32_t len);

//! Close the journal; the stream can be read again, opening it as needed.
void sport_fit_stream_close(SportFitStream *s);
