/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "store.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include <pbl/kernel/malloc.h>
#include <pbl/logging/logging.h>

static bool prv_header_ok(const SportJournalHeader *hdr, uint32_t start_utc, uint16_t segment) {
  return hdr->magic == SPORT_JOURNAL_MAGIC && hdr->version == SPORT_JOURNAL_VERSION &&
         hdr->record_size == SPORT_JOURNAL_RECORD_SIZE &&
         hdr->capacity == SPORT_JOURNAL_PER_SEGMENT && hdr->start_utc == start_utc &&
         hdr->segment == segment && sport_journal_check(hdr);
}

// Open a segment and check its header.
static int prv_open_segment(SportJournalReader *r, uint16_t segment) {
  char path[SPORT_JOURNAL_PATH_LEN];
  SportJournalHeader hdr;

  sport_journal_segment_path(path, r->start_utc, segment);
  if (pbl_fs_open(&r->file, path, PBL_FS_O_READ) < 0) {
    return -ENOENT;
  }
  if (pbl_fs_read(&r->file, &hdr, sizeof(hdr)) != sizeof(hdr) ||
      !prv_header_ok(&hdr, r->start_utc, segment)) {
    pbl_fs_close(&r->file);
    return -EINVAL;
  }

  r->open = true;
  r->segment = segment;
  r->sport = hdr.sport;
  r->index = 0;
  r->buffered = 0;
  r->pos = 0;

  return 0;
}

int sport_journal_reader_open(SportJournalReader *r, uint32_t start_utc) {
  *r = (SportJournalReader){.start_utc = start_utc};

  return prv_open_segment(r, 0);
}

int sport_journal_reader_seek(SportJournalReader *r, uint32_t start_utc, uint16_t segment,
                              uint16_t index) {
  int ret;

  *r = (SportJournalReader){.start_utc = start_utc};
  if (index > SPORT_JOURNAL_PER_SEGMENT) {
    return -EINVAL;
  }
  ret = prv_open_segment(r, segment);
  if (ret != 0) {
    return ret;
  }
  if (index != 0 &&
      pbl_fs_seek(&r->file,
                  (off_t)(sizeof(SportJournalHeader) + (size_t)index * sizeof(SportJournalRecord)),
                  PBL_FS_SEEK_SET) < 0) {
    sport_journal_reader_close(r);
    return -EINVAL;
  }
  r->index = index;

  return 0;
}

static bool prv_erased(const SportJournalRecord *rec) {
  const uint8_t *p = (const uint8_t *)rec;

  for (size_t i = 0; i < sizeof(*rec); i++) {
    if (p[i] != 0xff) {
      return false;
    }
  }

  return true;
}

int sport_journal_reader_next(SportJournalReader *r, SportJournalRecord *rec) {
  while (!r->end) {
    if (r->pos == r->buffered) {
      if (r->index == SPORT_JOURNAL_PER_SEGMENT) {
        // Full: the activity goes on in the next segment, if there is one.
        pbl_fs_close(&r->file);
        r->open = false;
        if (r->segment + 1 >= SPORT_JOURNAL_MAX_SEGMENTS ||
            prv_open_segment(r, (uint16_t)(r->segment + 1)) != 0) {
          r->end = true;
          break;
        }
      }

      const size_t want = SPORT_JOURNAL_PER_SEGMENT - r->index < SPORT_STORE_READ_RECORDS
                              ? SPORT_JOURNAL_PER_SEGMENT - r->index
                              : SPORT_STORE_READ_RECORDS;
      const ssize_t n = pbl_fs_read(&r->file, r->buf, want * sizeof(SportJournalRecord));
      if (n <= 0) {
        r->end = true;
        break;
      }
      r->buffered = (uint16_t)(n / sizeof(SportJournalRecord));
      r->pos = 0;
      if (r->buffered == 0) {
        r->end = true;
        break;
      }
    }

    const SportJournalRecord *cur = &r->buf[r->pos];

    if (prv_erased(cur)) {
      // The first record not written yet: the end.
      r->end = true;
      break;
    }
    r->pos++;
    r->index++;
    if (!sport_journal_check(cur)) {
      r->damaged++;
      continue;
    }
    *rec = *cur;
    return 1;
  }

  return 0;
}

void sport_journal_reader_end(const SportJournalReader *r, uint16_t *segment, uint16_t *index) {
  *segment = r->segment;
  *index = r->index;
}

void sport_journal_reader_close(SportJournalReader *r) {
  if (r->open) {
    pbl_fs_close(&r->file);
    r->open = false;
  }
}

bool sport_store_read_summary(uint32_t start_utc, SportJournalSummary *summary) {
  char path[SPORT_JOURNAL_PATH_LEN];
  struct pbl_fs_file file;
  bool ok;

  sport_journal_summary_path(path, start_utc);
  if (pbl_fs_open(&file, path, PBL_FS_O_READ) < 0) {
    return false;
  }
  ok = pbl_fs_read(&file, summary, sizeof(*summary)) == sizeof(*summary) &&
       summary->magic == SPORT_JOURNAL_SUMMARY_MAGIC && sport_journal_check(summary);
  pbl_fs_close(&file);

  return ok;
}

static bool prv_first_header(uint32_t start_utc, SportJournalHeader *hdr) {
  char path[SPORT_JOURNAL_PATH_LEN];
  struct pbl_fs_file file;
  bool ok;

  sport_journal_segment_path(path, start_utc, 0);
  if (pbl_fs_open(&file, path, PBL_FS_O_READ) < 0) {
    return false;
  }
  ok = pbl_fs_read(&file, hdr, sizeof(*hdr)) == sizeof(*hdr) && hdr->magic == SPORT_JOURNAL_MAGIC &&
       sport_journal_check(hdr);
  pbl_fs_close(&file);

  return ok;
}

size_t sport_store_list(SportStoreEntry *entries, size_t max) {
  struct pbl_fs_dir dir;
  struct pbl_fs_dirent *dirent = pbl_malloc(sizeof(*dirent));
  size_t n = 0;

  if (dirent == NULL) {
    return 0;
  }
  if (pbl_fs_opendir(&dir, "/") < 0) {
    pbl_free(dirent);
    return 0;
  }

  while (pbl_fs_readdir(&dir, dirent) == 0 && dirent->name[0] != '\0') {
    uint32_t start;

    if (!sport_journal_parse_first_segment(dirent->name, &start)) {
      continue;
    }
    if (n < max) {
      // Insertion, oldest first.
      size_t i = n;
      while (i > 0 && entries[i - 1].start_utc > start) {
        entries[i] = entries[i - 1];
        i--;
      }
      entries[i] = (SportStoreEntry){.start_utc = start};
    } else {
      // Keep the newest: drop the oldest kept if this one is newer.
      if (entries[0].start_utc < start) {
        size_t i = 0;
        while (i + 1 < max && entries[i + 1].start_utc < start) {
          entries[i] = entries[i + 1];
          i++;
        }
        entries[i] = (SportStoreEntry){.start_utc = start};
      }
    }
    n++;
  }
  pbl_fs_closedir(&dir);
  pbl_free(dirent);

  const size_t kept = n < max ? n : max;
  for (size_t i = 0; i < kept; i++) {
    SportStoreEntry *e = &entries[i];
    SportJournalHeader hdr;

    if (prv_first_header(e->start_utc, &hdr)) {
      e->sport = hdr.sport;
      e->readable =
          hdr.version == SPORT_JOURNAL_VERSION && hdr.record_size == SPORT_JOURNAL_RECORD_SIZE;
    }
    e->closed = sport_store_read_summary(e->start_utc, &e->summary);
  }

  return n;
}

int sport_store_scan(uint32_t start_utc, uint8_t *sport, SportJournalScan *scan) {
  SportJournalReader *r = pbl_malloc(sizeof(*r));
  SportJournalRecord rec;
  int ret;

  if (r == NULL) {
    return -ENOMEM;
  }
  sport_journal_scan_init(scan, 0);
  ret = sport_journal_reader_open(r, start_utc);
  if (ret == 0) {
    *sport = r->sport;
    while (sport_journal_reader_next(r, &rec) == 1) {
      sport_journal_scan_add(scan, &rec);
    }
    sport_journal_reader_close(r);
  }
  pbl_free(r);

  return ret;
}

int sport_store_write_summary(uint32_t start_utc, uint8_t sport, const SportJournalScan *scan,
                              const SportJournalStats *totals) {
  char path[SPORT_JOURNAL_PATH_LEN];
  struct pbl_fs_file file;
  SportJournalSummary summary = {
    .magic = SPORT_JOURNAL_SUMMARY_MAGIC,
    .version = SPORT_JOURNAL_VERSION,
    .sport = sport,
    .flags = totals->info,
    .has_gnss_time = scan->has_time,
    .start_utc = start_utc,
    .utc_start = scan->has_time ? scan->utc_start : start_utc,
    .elapsed_s = scan->last_time_s,
    .timer_s = totals->timer_s,
    .distance_cm = totals->distance_cm,
    .laps = totals->laps,
  };
  int ret;

  sport_journal_seal(&summary);
  sport_journal_summary_path(path, start_utc);
  ret = pbl_fs_open(&file, path, PBL_FS_O_REPLACE | PBL_FS_O_CREATE);
  if (ret < 0) {
    return ret;
  }
  ret = pbl_fs_truncate(&file, sizeof(summary));
  if (ret == 0 && pbl_fs_write(&file, &summary, sizeof(summary)) != sizeof(summary)) {
    ret = -EIO;
  }
  pbl_fs_close(&file);

  return ret;
}

int sport_store_close(uint32_t start_utc) {
  SportJournalScan scan;
  SportJournalStats totals;
  uint8_t sport = 0;
  int ret = sport_store_scan(start_utc, &sport, &scan);

  if (ret != 0) {
    return ret;
  }
  sport_journal_scan_totals(&scan, &totals);
  PBL_LOG_INFO("closing activity %08" PRIx32 " from its journal: %" PRIu32 " records", start_utc,
               scan.records);

  return sport_store_write_summary(start_utc, sport, &scan, &totals);
}

void sport_store_delete(uint32_t start_utc) {
  char path[SPORT_JOURNAL_PATH_LEN];

  sport_journal_summary_path(path, start_utc);
  pbl_fs_unlink(path);
  for (uint16_t seg = 0; seg < SPORT_JOURNAL_MAX_SEGMENTS; seg++) {
    sport_journal_segment_path(path, start_utc, seg);
    if (pbl_fs_unlink(path) < 0) {
      break;
    }
  }
}

typedef struct ExportPass {
  uint32_t start_utc;
  SportJournalReader reader;
  SportFitEncoder enc;
} ExportPass;

// One pass over the journal, encoding it.
static int prv_pass(ExportPass *p, const SportFitInfo *info, uint32_t total_size,
                    SportFitWrite write, void *ctx, uint32_t *size, uint8_t *sport) {
  SportJournalRecord rec;
  int ret = sport_journal_reader_open(&p->reader, p->start_utc);

  if (ret != 0) {
    return ret;
  }
  *sport = p->reader.sport;

  SportFitInfo i = *info;
  i.sport = p->reader.sport;
  sport_fit_begin(&p->enc, &i, total_size, write, ctx);
  while (sport_journal_reader_next(&p->reader, &rec) == 1) {
    sport_fit_add(&p->enc, &rec);
  }
  sport_journal_reader_close(&p->reader);
  *size = sport_fit_end(&p->enc);

  return 0;
}

int sport_store_export_fit(uint32_t start_utc, SportFitWrite write, void *ctx) {
  ExportPass *p = pbl_zalloc(sizeof(*p));
  SportFitInfo info = {.serial_number = 0};
  uint32_t size = 0;
  uint8_t sport = 0;
  int ret;

  if (p == NULL) {
    return -ENOMEM;
  }
  p->start_utc = start_utc;

  // The first pass sizes the file and finds the GNSS's time of the start.
  info.utc_start = start_utc;
  ret = prv_pass(p, &info, 0, NULL, NULL, &size, &sport);
  if (ret == 0 && p->enc.session.has_time) {
    info.utc_start = p->enc.session.utc_start;
  }
  if (ret == 0 && write != NULL) {
    uint32_t written;
    ret = prv_pass(p, &info, size, write, ctx, &written, &sport);
    if (ret == 0 && written != size) {
      PBL_LOG_ERR("FIT size changed: %" PRIu32 " then %" PRIu32, size, written);
      ret = -EIO;
    }
  }
  pbl_free(p);

  return ret == 0 ? (int)size : ret;
}
