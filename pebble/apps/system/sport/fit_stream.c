/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "fit_stream.h"

#include <errno.h>
#include <string.h>

// What the encoder writes: kept from skip_to, after what the stage holds
static void prv_out(const void *data, size_t len, void *ctx) {
  SportFitStream *s = ctx;
  const uint8_t *d = data;
  uint32_t o = s->stage_off + s->stage_len;

  if (len >= 2) {
    s->last[0] = d[len - 2];
    s->last[1] = d[len - 1];
  } else if (len == 1) {
    s->last[0] = s->last[1];
    s->last[1] = d[0];
  }

  if (s->stage_len == 0 && o < s->skip_to) {
    const uint32_t drop = s->skip_to - o < len ? s->skip_to - o : (uint32_t)len;

    d += drop;
    len -= drop;
    s->stage_off += drop;
  }
  if (len == 0) {
    return;
  }
  if (s->stage_len + len > sizeof(s->stage)) {
    s->overflow = true;
    return;
  }
  memcpy(&s->stage[s->stage_len], d, len);
  s->stage_len += (uint32_t)len;
}

// A mark where the encoding stands, between two records, for the window of mark_every bytes it is
// in, unless that window has one, as good
static void prv_maybe_mark(SportFitStream *s) {
  const uint32_t k = s->mark_every ? s->enc.size / s->mark_every : 0;
  SportFitMark *m;

  if (k == 0 || k > s->num_marks) {
    return;
  }
  m = &s->marks[k - 1];
  if (m->offset != 0 && (m->crc_valid || !s->crc_valid)) {
    return;
  }
  *m = (SportFitMark){
    .offset = s->enc.size,
    .segment = s->reader.segment,
    .index = s->reader.index,
    .crc_valid = s->crc_valid,
    .enc = s->enc,
  };
}

static void prv_stop(SportFitStream *s) {
  sport_journal_reader_close(&s->reader);
  s->live = false;
}

// Carry on from a mark, or from the start without one
static int prv_start(SportFitStream *s, const SportFitMark *m) {
  int ret;

  prv_stop(s);
  s->stage_len = 0;
  s->overflow = false;
  s->ended = false;
  if (m != NULL) {
    ret = sport_journal_reader_seek(&s->reader, s->start_utc, m->segment, m->index);
    if (ret != 0) {
      return ret;
    }
    s->enc = m->enc;
    s->enc.info = s->info;
    s->enc.write = prv_out;
    s->enc.ctx = s;
    s->crc_valid = m->crc_valid;
    s->stage_off = m->offset;
  } else {
    ret = sport_journal_reader_open(&s->reader, s->start_utc);
    if (ret != 0) {
      return ret;
    }
    s->crc_valid = true;
    s->stage_off = 0;
    sport_fit_begin(&s->enc, &s->info, s->size, prv_out, s);
  }
  s->live = true;
  s->restarts++;

  return 0;
}

// The CRC at the end, from a pass that has it, into what the stage holds of it
static void prv_patch_crc(SportFitStream *s) {
  for (uint32_t i = 0; i < 2; i++) {
    const uint32_t at = s->size - 2 + i;

    if (at >= s->stage_off && at < s->stage_off + s->stage_len) {
      s->stage[at - s->stage_off] = (uint8_t)(s->crc >> (8 * i));
    }
  }
}

// Encode the next record, or the end
static int prv_step(SportFitStream *s) {
  SportJournalRecord rec;

  if (s->ended) {
    return -EIO;
  }
  if (sport_journal_reader_next(&s->reader, &rec) == 1) {
    sport_fit_add(&s->enc, &rec);
    s->records++;
    prv_maybe_mark(s);
  } else {
    if (sport_fit_end(&s->enc) != s->size) {
      prv_stop(s);
      return -EIO;
    }
    s->ended = true;
    sport_journal_reader_close(&s->reader);
    if (s->crc_valid) {
      s->crc = (uint16_t)(s->last[0] | (s->last[1] << 8));
      s->crc_known = true;
    } else if (s->crc_known) {
      prv_patch_crc(s);
    }
  }

  return s->overflow ? -EIO : 0;
}

int sport_fit_stream_open(SportFitStream *s, uint32_t start_utc, SportFitMark *marks,
                          uint16_t num_marks, uint32_t mark_every) {
  SportJournalRecord rec;
  int ret;

  memset(s, 0, offsetof(SportFitStream, stage));
  s->start_utc = start_utc;
  s->marks = marks;
  s->num_marks = num_marks;
  s->mark_every = mark_every;
  memset(marks, 0, sizeof(*marks) * num_marks);

  // The size, as the export's first pass finds it, and the GNSS's time of the start
  ret = sport_journal_reader_open(&s->reader, start_utc);
  if (ret != 0) {
    return ret;
  }
  s->info = (SportFitInfo){.sport = s->reader.sport, .utc_start = start_utc};
  sport_fit_begin(&s->enc, &s->info, 0, NULL, NULL);
  while (sport_journal_reader_next(&s->reader, &rec) == 1) {
    sport_fit_add(&s->enc, &rec);
    prv_maybe_mark(s);
  }
  sport_journal_reader_close(&s->reader);
  s->size = sport_fit_end(&s->enc);
  if (s->enc.session.has_time) {
    s->info.utc_start = s->enc.session.utc_start;
  }

  return 0;
}

void sport_fit_stream_reopen(SportFitStream *s, uint32_t start_utc, uint32_t utc_start,
                             uint32_t size, bool crc_known, uint16_t crc, SportFitMark *marks,
                             uint16_t num_marks, uint32_t mark_every) {
  SportJournalReader *r = &s->reader;

  memset(s, 0, offsetof(SportFitStream, stage));
  s->start_utc = start_utc;
  s->size = size;
  s->crc_known = crc_known;
  s->crc = crc;
  s->marks = marks;
  s->num_marks = num_marks;
  s->mark_every = mark_every;
  s->info = (SportFitInfo){.utc_start = utc_start};
  if (sport_journal_reader_open(r, start_utc) == 0) {
    s->info.sport = r->sport;
    sport_journal_reader_close(r);
  }
}

// The latest mark at or before offset; with need_crc, one with the file's CRC
static const SportFitMark *prv_mark(const SportFitStream *s, uint32_t offset, bool need_crc) {
  for (uint32_t k = s->num_marks; k > 0; k--) {
    const SportFitMark *m = &s->marks[k - 1];

    if (m->offset != 0 && m->offset <= offset && (m->crc_valid || !need_crc)) {
      return m;
    }
  }

  return NULL;
}

int sport_fit_stream_read(SportFitStream *s, uint32_t offset, void *buf, uint32_t len) {
  uint8_t *b = buf;
  const bool need_crc = !s->crc_known && offset + len + 2 > s->size;
  int ret;

  if (offset > s->size || len > s->size - offset) {
    return -EINVAL;
  }

  s->skip_to = offset;
  // From where the encoding is, unless it is past the offset, has not the CRC asked for, or a mark
  // is nearer
  const SportFitMark *m = prv_mark(s, offset, need_crc);
  if (!s->live || offset < s->stage_off || (need_crc && !s->crc_valid) ||
      (m != NULL && m->offset > s->stage_off + s->stage_len)) {
    ret = prv_start(s, m);
    if (ret != 0) {
      return ret;
    }
  }

  // What the stage holds before the offset goes
  if (offset >= s->stage_off + s->stage_len) {
    s->stage_off += s->stage_len;
    s->stage_len = 0;
  } else if (offset > s->stage_off) {
    const uint32_t n = offset - s->stage_off;

    memmove(s->stage, &s->stage[n], s->stage_len - n);
    s->stage_len -= n;
    s->stage_off = offset;
  }
  s->skip_to = offset;

  while (len > 0) {
    if (s->stage_len > 0 && s->stage_off == offset) {
      const uint32_t n = s->stage_len < len ? s->stage_len : len;

      memcpy(b, s->stage, n);
      memmove(s->stage, &s->stage[n], s->stage_len - n);
      s->stage_len -= n;
      s->stage_off += n;
      b += n;
      offset += n;
      len -= n;
      s->skip_to = offset;
      continue;
    }
    ret = prv_step(s);
    if (ret != 0) {
      prv_stop(s);
      return ret;
    }
  }

  return 0;
}

void sport_fit_stream_close(SportFitStream *s) {
  prv_stop(s);
}
