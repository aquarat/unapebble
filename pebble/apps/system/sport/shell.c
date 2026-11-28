/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

// The workout shell command: list, inspect, export as FIT, close and delete the activities.
// tools/una/workouts.py drives it.

#ifdef CONFIG_APP_SPORT_SHELL

#include "recorder.h"
#include "store.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <pbl/crc/crc.h>
#include <pbl/kernel/malloc.h>
#include <pbl/shell/shell.h>
#include <pbl/util/base64.h>

#define MAX_LISTED 64
// Bytes a data line carries: 96 characters of base64, within the shell's line.
#define LINE_BYTES 72

static const char *prv_state(const SportStoreEntry *e, uint32_t recording) {
  if (!e->readable) {
    return "unreadable";
  }
  if (e->start_utc == recording) {
    return "recording";
  }
  if (!e->closed) {
    return "open";
  }
  if (e->summary.flags & SportJournalStopFlag_Truncated) {
    return "truncated";
  }
  if (e->summary.flags & SportJournalStopFlag_Recovered) {
    return "recovered";
  }
  return "done";
}

static int prv_list(const struct pbl_shell *sh) {
  SportStoreEntry *entries = pbl_malloc(MAX_LISTED * sizeof(*entries));
  uint32_t recording = 0;
  size_t n;

  if (entries == NULL) {
    return -ENOMEM;
  }
  (void)sport_recorder_is_active(&recording);
  n = sport_store_list(entries, MAX_LISTED);
  pbl_shell_print(sh,
                  "%u activities: id sport state utc_start gnss_time elapsed_s timer_s "
                  "distance_m laps",
                  (unsigned)n);
  for (size_t i = 0; i < n && i < MAX_LISTED; i++) {
    const SportStoreEntry *e = &entries[i];
    SportJournalScan scan;
    SportJournalStats totals;
    uint32_t utc = e->start_utc;
    uint32_t elapsed;
    bool gnss_time = false;
    uint8_t sport = e->sport;

    if (e->closed) {
      utc = e->summary.utc_start;
      gnss_time = e->summary.has_gnss_time;
      elapsed = e->summary.elapsed_s;
      totals = (SportJournalStats){
        .timer_s = e->summary.timer_s,
        .distance_cm = e->summary.distance_cm,
        .laps = e->summary.laps
      };
    } else if (e->readable && sport_store_scan(e->start_utc, &sport, &scan) == 0) {
      if (scan.has_time) {
        utc = scan.utc_start;
        gnss_time = true;
      }
      elapsed = scan.last_time_s;
      sport_journal_scan_totals(&scan, &totals);
    } else {
      elapsed = 0;
      totals = (SportJournalStats){};
    }
    pbl_shell_print(sh, "%08" PRIx32 " %s %s %" PRIu32 " %u %" PRIu32 " %" PRIu32 " %" PRIu32 " %u",
                    e->start_utc, sport_journal_sport_name(sport), prv_state(e, recording), utc,
                    gnss_time, elapsed, totals.timer_s, totals.distance_cm / 100, totals.laps);
  }
  pbl_free(entries);

  return 0;
}

static int prv_info(const struct pbl_shell *sh, uint32_t start) {
  SportJournalReader *r = pbl_malloc(sizeof(*r));
  SportJournalRecord rec;
  SportJournalScan scan;
  uint32_t by_type[4] = {};
  uint32_t fixes = 0;
  uint32_t restarts = 0;
  int ret;

  if (r == NULL) {
    return -ENOMEM;
  }
  ret = sport_journal_reader_open(r, start);
  if (ret != 0) {
    pbl_free(r);
    pbl_shell_error(sh, "no activity %08" PRIx32 " (%d)", start, ret);
    return ret;
  }
  sport_journal_scan_init(&scan, 0);
  while (sport_journal_reader_next(r, &rec) == 1) {
    sport_journal_scan_add(&scan, &rec);
    if (rec.type < 4) {
      by_type[rec.type]++;
    }
    if (rec.type == SportJournalType_Sample && (rec.flags & SportJournalSampleFlag_Fix)) {
      fixes++;
    }
    if (rec.type == SportJournalType_Event && rec.event == SportJournalEvent_Restarted) {
      restarts++;
    }
  }
  uint16_t seg;
  uint16_t idx;
  sport_journal_reader_end(r, &seg, &idx);
  sport_journal_reader_close(r);

  pbl_shell_print(sh,
                  "%s, %" PRIu32 " records (%" PRIu32 " samples, %" PRIu32 " events, %" PRIu32
                  " times), %" PRIu32 " damaged, end at segment %u record %u",
                  sport_journal_sport_name(r->sport), scan.records, by_type[1], by_type[2],
                  by_type[3], r->damaged, seg, idx);
  pbl_shell_print(sh,
                  "time %" PRIu32 "-%" PRIu32 " s, timer %" PRIu32 " s, %" PRIu32 " cm, %" PRIu32
                  " fixes, %u laps, %" PRIu32 " restarts, stopped %u",
                  scan.first_time_s, scan.last_time_s, scan.timer_s, scan.distance_cm, fixes,
                  scan.laps, restarts, scan.stopped);
  pbl_shell_print(sh,
                  "max %" PRIu32 " cm/s, ascent %" PRIu32 " dm, descent %" PRIu32
                  " dm, gnss time %u utc_start %" PRIu32,
                  scan.max_speed_cm_s, scan.ascent_dm, scan.descent_dm, scan.has_time,
                  scan.has_time ? scan.utc_start : start);
  pbl_free(r);

  return 0;
}

typedef struct FitOut {
  const struct pbl_shell *sh;
  uint32_t pos;
  uint32_t from;
  uint32_t to;
  uint32_t crc;
  uint32_t sent;
  uint8_t line[LINE_BYTES];
  uint8_t len;
  uint32_t line_off;
} FitOut;

static void prv_emit_line(FitOut *o) {
  char b64[2 * LINE_BYTES];

  if (o->len == 0) {
    return;
  }
  pbl_base64_encode(b64, sizeof(b64), o->line, o->len);
  pbl_shell_print(o->sh, "D %08" PRIx32 " %s", o->line_off, b64);
  o->len = 0;
}

static void prv_fit_write(const void *data, size_t len, void *ctx) {
  FitOut *o = ctx;
  const uint8_t *p = data;

  for (size_t i = 0; i < len; i++, o->pos++) {
    if (o->pos < o->from || o->pos >= o->to) {
      continue;
    }
    if (o->len == 0) {
      o->line_off = o->pos;
    }
    o->line[o->len++] = p[i];
    o->crc = pbl_crc32(o->crc, &p[i], 1);
    o->sent++;
    if (o->len == LINE_BYTES) {
      prv_emit_line(o);
    }
  }
}

static int prv_fit(const struct pbl_shell *sh, uint32_t start, uint32_t from, uint32_t len) {
  FitOut *o = pbl_zalloc(sizeof(*o));
  int ret;

  if (o == NULL) {
    return -ENOMEM;
  }
  *o = (FitOut){.sh = sh, .from = from, .to = len ? from + len : UINT32_MAX};
  if (o->to < from) {
    o->to = UINT32_MAX;
  }

  // The flushed journal only: what the recorder still buffers comes later.
  sport_recorder_flush();
  ret = sport_store_export_fit(start, prv_fit_write, o);
  if (ret < 0) {
    pbl_free(o);
    pbl_shell_error(sh, "no activity %08" PRIx32 " (%d)", start, ret);
    return ret;
  }
  prv_emit_line(o);
  pbl_shell_print(sh, "E %08" PRIx32 " %d %08" PRIx32 " %" PRIu32 " %08" PRIx32, start, ret, from,
                  o->sent, o->crc);
  pbl_free(o);

  return 0;
}

static bool prv_parse_id(const struct pbl_shell *sh, const char *arg, uint32_t *start) {
  char *end;
  const unsigned long v = strtoul(arg, &end, 16);

  if (*arg == '\0' || *end != '\0') {
    pbl_shell_error(sh, "expected an activity id, 8 hex digits");
    return false;
  }
  *start = (uint32_t)v;

  return true;
}

static int prv_cmd_workout(const struct pbl_shell *sh, size_t argc, char **argv) {
  uint32_t start;
  uint32_t recording = 0;

  if (argc < 2 || strcmp(argv[1], "list") == 0) {
    return prv_list(sh);
  }
  if (argc < 3 || !prv_parse_id(sh, argv[2], &start)) {
    pbl_shell_error(sh, "workout [list|info ID|fit ID [OFFSET [LEN]]|close ID|delete ID]");
    return -EINVAL;
  }
  if (strcmp(argv[1], "info") == 0) {
    return prv_info(sh, start);
  }
  if (strcmp(argv[1], "fit") == 0) {
    const uint32_t from = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 0) : 0;
    const uint32_t len = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 0) : 0;
    return prv_fit(sh, start, from, len);
  }

  const bool busy = sport_recorder_is_active(&recording) && recording == start;

  if (strcmp(argv[1], "close") == 0 || strcmp(argv[1], "delete") == 0) {
    if (busy) {
      pbl_shell_error(sh, "%08" PRIx32 " is recording", start);
      return -EBUSY;
    }
    if (argv[1][0] == 'c') {
      const int ret = sport_store_close(start);
      if (ret != 0) {
        pbl_shell_error(sh, "could not close %08" PRIx32 " (%d)", start, ret);
      }
      return ret;
    }
    sport_store_delete(start);
    pbl_shell_print(sh, "deleted %08" PRIx32, start);
    return 0;
  }

  pbl_shell_error(sh, "unknown subcommand %s", argv[1]);
  return -EINVAL;
}

PBL_SHELL_CMD_ARG_REGISTER(workout, nullptr,
                           "Recorded activities: workout [list|info ID|fit ID [OFFSET [LEN]]|"
                           "close ID|delete ID]",
                           prv_cmd_workout, 1, 4);

#endif
