/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

// The workouts as a read-only USB drive: a FAT volume computed as it is read (lib/vfat), of one
// FIT file for each finished workout, encoded from its journal as the host reads it
// (sport/fit_stream.c), nothing of it stored. When the USB is up and who wants it, and when the
// drive's medium is there, is decided here; the drive's commands and its reads run in the drive's
// thread (subsys/usb/msc.c).

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include <pbl/drivers/mcu.h>
#include <pbl/kernel/malloc.h>
#include <pbl/kernel/time.h>
#include <pbl/kernel/work.h>
#include <pbl/logging/logging.h>
#include <pbl/services/battery/battery_charger.h>
#include <pbl/services/time.h>
#include <pbl/services/usb_drive.h>
#include <pbl/services/usb_power.h>
#include <pbl/settings/settings.h>
#include <pbl/usb/msc.h>
#include <pbl/util/math.h>
#include <pbl/vfat/vfat.h>

#ifdef CONFIG_SHELL
#include <pbl/shell/shell.h>
#endif

#include <applib/event_service_client.h>
#include <apps/system/sport/fit_stream.h>
#include <apps/system/sport/store.h>

PBL_LOG_MODULE_DEFINE(usb_drive, CONFIG_USB_DRIVE_LOG_LEVEL);

#define SETTINGS_FILE     "usbdrive"
#define SETTINGS_FILE_LEN 512
#define SETTINGS_KEY      "enabled"

#define LABEL "UNA WATCH"

// A host configures the device within this, or there is none: a charger
#define HOST_WAIT_MS 10000U
// A change waits for the host to have stopped reading this long, then the medium is away this
// long, for the host to see it go
#define CHANGE_IDLE_MS 3000U
#define CHANGE_GONE_MS 5000U
// Sizing a slice of the files, between which the host hears the medium is becoming ready
#define SIZING_SLICE_MS 1000U

#define MAX_FILES    64U
#define MARK_EVERY   32768U
#define MAX_MARKS    80U
#define CACHE_BLOCK  4096U
#define CACHE_BLOCKS 16U
#define NUM_STREAMS  2U

// What is known of a workout, kept across plug-ins while its summary stays the same
struct prv_known {
  bool valid;
  bool sized;
  uint32_t start_utc;
  uint16_t summary_crc;
  uint32_t utc_start;
  uint32_t size;
  bool crc_known;
  uint16_t crc;
  SportFitMark *marks;
  uint16_t num_marks;
};

enum prv_state {
  STATE_NONE,
  STATE_SIZING,
  STATE_READY,
};

static atomic_bool s_enabled;
static atomic_bool s_plugged;
// No host came, or the host ejected the drive: the drive wants no USB until the next plug-in
static atomic_bool s_given_up;
static atomic_bool s_ejected;
static atomic_bool s_dirty;
static atomic_bool s_ready;
static EventServiceInfo s_battery_event_info;
static struct pbl_delayed_work s_host_check;

// The drive's thread's alone, but for the shell, which only reads
static struct {
  enum prv_state state;
  uint64_t gone_until_ms;
  struct prv_known known[MAX_FILES];
  SportStoreEntry list[MAX_FILES];
  size_t num_listed;
  size_t next;
  struct vfat_file files[MAX_FILES];
  uint8_t file_known[MAX_FILES];
  struct vfat vol;
  size_t num_files;
  uint32_t serial;
  // Statistics
  uint32_t reads;
  uint32_t hits;
  uint32_t misses;
  uint32_t records;
  uint32_t restarts;
  uint32_t sizing_ms;
} s;

static SportFitStream s_streams[NUM_STREAMS];
static int s_stream_known[NUM_STREAMS] = {-1, -1};
static uint32_t s_stream_used[NUM_STREAMS];
static SportFitMark s_scratch_marks[MAX_MARKS];

static struct {
  int known;
  uint32_t block;
  uint32_t used;
} s_tags[CACHE_BLOCKS];
static uint8_t s_cache[CACHE_BLOCKS][CACHE_BLOCK];
static uint32_t s_tick;

// Settings
////////////

static bool prv_load(void) {
  struct pbl_settings_file file = {};
  bool enabled = true;

  if (pbl_settings_open(&file, SETTINGS_FILE, SETTINGS_FILE_LEN) != 0) {
    return true;
  }
  if (pbl_settings_get_len(&file, SETTINGS_KEY, sizeof(SETTINGS_KEY)) == sizeof(enabled)) {
    (void)pbl_settings_get(&file, SETTINGS_KEY, sizeof(SETTINGS_KEY), &enabled, sizeof(enabled));
  }
  pbl_settings_close(&file);

  return enabled;
}

static void prv_store(bool enabled) {
  struct pbl_settings_file file = {};

  if (pbl_settings_open(&file, SETTINGS_FILE, SETTINGS_FILE_LEN) != 0) {
    PBL_LOG_ERR("Cannot keep the setting");
    return;
  }
  (void)pbl_settings_set(&file, SETTINGS_KEY, sizeof(SETTINGS_KEY), &enabled, sizeof(enabled));
  pbl_settings_close(&file);
}

// The workouts' files
///////////////////////

static void prv_cache_clear(void) {
  for (size_t i = 0; i < CACHE_BLOCKS; i++) {
    s_tags[i].known = -1;
  }
}

static void prv_forget(struct prv_known *k) {
  const int idx = (int)(k - s.known);

  for (size_t i = 0; i < NUM_STREAMS; i++) {
    if (s_stream_known[i] == idx) {
      sport_fit_stream_close(&s_streams[i]);
      s_stream_known[i] = -1;
    }
  }
  pbl_free(k->marks);
  *k = (struct prv_known){0};
}

static struct prv_known *prv_known_for(const SportStoreEntry *e) {
  struct prv_known *free_slot = NULL;

  for (size_t i = 0; i < MAX_FILES; i++) {
    struct prv_known *k = &s.known[i];

    if (k->valid && k->start_utc == e->start_utc) {
      if (k->summary_crc == e->summary.crc) {
        return k;
      }
      prv_forget(k);
      free_slot = k;
      break;
    }
    if (!k->valid && free_slot == NULL) {
      free_slot = k;
    }
  }
  if (free_slot != NULL) {
    *free_slot = (struct prv_known){
      .valid = true,
      .start_utc = e->start_utc,
      .summary_crc = e->summary.crc,
    };
  }

  return free_slot;
}

// The finished workouts this firmware reads, oldest first; what is known of others is dropped
static void prv_list(void) {
  const size_t n = PBL_MIN(sport_store_list(s.list, MAX_FILES), (size_t)MAX_FILES);
  bool keep[MAX_FILES] = {false};

  s.num_listed = 0;
  for (size_t i = 0; i < n; i++) {
    const SportStoreEntry *e = &s.list[i];
    struct prv_known *k;

    if (!e->closed || !e->readable) {
      continue;
    }
    k = prv_known_for(e);
    if (k == NULL) {
      continue;
    }
    keep[k - s.known] = true;
    s.list[s.num_listed++] = *e;
  }
  for (size_t i = 0; i < MAX_FILES; i++) {
    if (s.known[i].valid && !keep[i]) {
      prv_forget(&s.known[i]);
    }
  }
  s.next = 0;
  prv_cache_clear();
}

static struct prv_known *prv_known_of(uint32_t start_utc) {
  for (size_t i = 0; i < MAX_FILES; i++) {
    if (s.known[i].valid && s.known[i].start_utc == start_utc) {
      return &s.known[i];
    }
  }

  return NULL;
}

// The size of a file, a pass over its journal, with marks along it
static void prv_size(struct prv_known *k) {
  static SportFitStream s_sizing;
  uint16_t used = 0;
  int ret = sport_fit_stream_open(&s_sizing, k->start_utc, s_scratch_marks, MAX_MARKS, MARK_EVERY);

  sport_fit_stream_close(&s_sizing);
  if (ret != 0) {
    PBL_LOG_WRN("Workout %08" PRIx32 " not sized: %d", k->start_utc, ret);
    return;
  }
  for (uint16_t i = 0; i < MAX_MARKS; i++) {
    if (s_scratch_marks[i].offset != 0) {
      used = i + 1;
    }
  }
  k->marks = used != 0 ? pbl_malloc(sizeof(SportFitMark) * used) : NULL;
  if (k->marks != NULL) {
    memcpy(k->marks, s_scratch_marks, sizeof(SportFitMark) * used);
    k->num_marks = used;
  }
  k->size = s_sizing.size;
  k->utc_start = s_sizing.info.utc_start;
  k->sized = true;
}

static void prv_name(struct vfat_file *f, size_t n, const SportStoreEntry *e, uint32_t utc) {
  const time_t t = (time_t)utc;
  struct pbl_tm tm;
  char sport[8];
  const char *name = sport_journal_sport_name(e->sport);
  size_t i;

  for (i = 0; i + 1 < sizeof(sport) && name[i] != '\0'; i++) {
    sport[i] = (name[i] >= 'A' && name[i] <= 'Z') ? (char)(name[i] - 'A' + 'a') : name[i];
  }
  sport[i] = '\0';
  pbl_localtime_r(&t, &tm);
  f->date = vfat_date((unsigned int)tm.tm_year + 1900U, (unsigned int)tm.tm_mon + 1U,
                      (unsigned int)tm.tm_mday);
  f->time = vfat_time((unsigned int)tm.tm_hour, (unsigned int)tm.tm_min, (unsigned int)tm.tm_sec);

  // Two workouts of a sport in the same minute: the later ones numbered
  for (unsigned int num = 1; num < 100; num++) {
    bool taken = false;
    char suffix[12] = "";

    if (num > 1) {
      snprintf(suffix, sizeof(suffix), "-%u", num);
    }
    snprintf(f->name, sizeof(f->name), "%04d-%02d-%02d-%02d%02d-%s%s.fit", tm.tm_year + 1900,
             tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, sport, suffix);
    for (size_t j = 0; j < n; j++) {
      if (strcmp(s.files[j].name, f->name) == 0) {
        taken = true;
      }
    }
    if (!taken) {
      return;
    }
  }
}

static int prv_file_read(void *ctx, size_t file, uint32_t offset, void *buf, uint32_t len);

static void prv_layout(void) {
  const time_t t = pbl_realtime();
  size_t n = 0;
  struct pbl_tm tm;
  int ret;

  for (size_t i = 0; i < s.num_listed; i++) {
    struct prv_known *k = prv_known_of(s.list[i].start_utc);

    if (k == NULL || !k->sized) {
      continue;
    }
    memset(&s.files[n], 0, sizeof(s.files[n]));
    prv_name(&s.files[n], n, &s.list[i], k->utc_start);
    s.files[n].size = k->size;
    s.file_known[n] = (uint8_t)(k - s.known);
    n++;
  }

  pbl_localtime_r(&t, &tm);
  const struct vfat_params p = {
    .label = LABEL,
    .serial = s.serial,
    .date = vfat_date((unsigned int)tm.tm_year + 1900U, (unsigned int)tm.tm_mon + 1U,
                      (unsigned int)tm.tm_mday),
    .time = vfat_time((unsigned int)tm.tm_hour, (unsigned int)tm.tm_min, (unsigned int)tm.tm_sec),
    .read = prv_file_read,
  };
  ret = vfat_layout(&s.vol, &p, s.files, n);
  if (ret < 0) {
    PBL_LOG_ERR("No volume: %d", ret);
    ret = vfat_layout(&s.vol, &p, s.files, 0);
  }
  s.num_files = ret > 0 ? (size_t)ret : 0;
  if (s.num_files < n) {
    PBL_LOG_WRN("%u of %u workouts left out of the drive", (unsigned int)(n - s.num_files),
                (unsigned int)n);
  }
  PBL_LOG_INFO("USB drive: %u workouts", (unsigned int)s.num_files);
}

static SportFitStream *prv_stream(int known) {
  const struct prv_known *k = &s.known[known];
  size_t lru = 0;

  for (size_t i = 0; i < NUM_STREAMS; i++) {
    if (s_stream_known[i] == known) {
      s_stream_used[i] = ++s_tick;
      return &s_streams[i];
    }
    if (s_stream_used[i] < s_stream_used[lru]) {
      lru = i;
    }
  }

  sport_fit_stream_close(&s_streams[lru]);
  sport_fit_stream_reopen(&s_streams[lru], k->start_utc, k->utc_start, k->size, k->crc_known,
                          k->crc, k->marks, k->num_marks, MARK_EVERY);
  s_stream_known[lru] = known;
  s_stream_used[lru] = ++s_tick;

  return &s_streams[lru];
}

// A block of a file, from the cache or encoded into it
static const uint8_t *prv_block(int known, uint32_t block, int *err) {
  struct prv_known *k = &s.known[known];
  size_t victim = 0;
  SportFitStream *st;
  uint32_t off = block * CACHE_BLOCK;
  uint32_t len = PBL_MIN(k->size - off, CACHE_BLOCK);
  uint32_t records;
  uint32_t restarts;

  for (size_t i = 0; i < CACHE_BLOCKS; i++) {
    if (s_tags[i].known == known && s_tags[i].block == block) {
      s_tags[i].used = ++s_tick;
      s.hits++;
      return s_cache[i];
    }
    // The least recently used goes, an empty one first
    if (s_tags[victim].known >= 0 &&
        (s_tags[i].known < 0 || s_tags[i].used < s_tags[victim].used)) {
      victim = i;
    }
  }

  s.misses++;
  st = prv_stream(known);
  records = st->records;
  restarts = st->restarts;
  *err = sport_fit_stream_read(st, off, s_cache[victim], len);
  s.records += st->records - records;
  s.restarts += st->restarts - restarts;
  k->crc_known = st->crc_known;
  k->crc = st->crc;
  if (*err != 0) {
    PBL_LOG_WRN("Workout %08" PRIx32 " at %" PRIu32 ": %d", k->start_utc, off, *err);
    s_tags[victim].known = -1;
    return NULL;
  }
  s_tags[victim].known = known;
  s_tags[victim].block = block;
  s_tags[victim].used = ++s_tick;

  return s_cache[victim];
}

static int prv_file_read(void *ctx, size_t file, uint32_t offset, void *buf, uint32_t len) {
  const int known = s.file_known[file];
  uint8_t *b = buf;

  while (len > 0) {
    const uint32_t block = offset / CACHE_BLOCK;
    const uint32_t in = offset % CACHE_BLOCK;
    const uint32_t n = PBL_MIN(len, CACHE_BLOCK - in);
    int err = 0;
    const uint8_t *data = prv_block(known, block, &err);

    if (data == NULL) {
      return err != 0 ? err : -EIO;
    }
    memcpy(b, &data[in], n);
    b += n;
    offset += n;
    len -= n;
  }

  return 0;
}

// The medium, in the drive's thread
/////////////////////////////////////

static int prv_status(uint32_t *num_blocks, void *ctx) {
  const uint64_t now = pbl_uptime_ms();
  uint64_t start;

  if (!atomic_load(&s_enabled) || atomic_load(&s_ejected)) {
    return -ENOMEDIUM;
  }
  if (now < s.gone_until_ms) {
    return -ENOMEDIUM;
  }

  // The workouts changed: once the host stopped reading, the medium goes away a while, then comes
  // back with them
  if (atomic_load(&s_dirty)) {
    if (s.state == STATE_READY) {
      if ((uint32_t)now - pbl_usb_msc_last_read_ms() < CHANGE_IDLE_MS) {
        goto ready;
      }
      s.gone_until_ms = now + CHANGE_GONE_MS;
      s.state = STATE_NONE;
      return -ENOMEDIUM;
    }
    s.state = STATE_NONE;
  }

  if (s.state == STATE_NONE) {
    atomic_store(&s_dirty, false);
    prv_list();
    s.state = STATE_SIZING;
    s.sizing_ms = 0;
  }
  if (s.state == STATE_SIZING) {
    start = pbl_uptime_ms();
    while (s.next < s.num_listed && pbl_uptime_ms() - start < SIZING_SLICE_MS) {
      struct prv_known *k = prv_known_of(s.list[s.next].start_utc);

      if (k != NULL && !k->sized) {
        prv_size(k);
      }
      s.next++;
    }
    s.sizing_ms += (uint32_t)(pbl_uptime_ms() - start);
    if (s.next < s.num_listed) {
      return -EAGAIN;
    }
    prv_layout();
    s.state = STATE_READY;
  }

ready:
  *num_blocks = vfat_num_sectors(&s.vol);
  return 0;
}

static int prv_read(uint32_t lba, void *buf, uint32_t count, void *ctx) {
  if (s.state != STATE_READY) {
    return -EIO;
  }
  s.reads++;

  return vfat_read(&s.vol, lba, buf, count);
}

static void prv_apply(void *data);

static void prv_eject(void *ctx) {
  PBL_LOG_INFO("USB drive ejected");
  atomic_store(&s_ejected, true);
  atomic_store(&s_given_up, true);
  (void)pbl_sys_work_call(prv_apply, nullptr);
}

static const struct pbl_usb_msc_medium s_medium = {
  .status = prv_status,
  .read = prv_read,
  .eject = prv_eject,
};

// When the drive wants the USB
////////////////////////////////

static void prv_apply(void *data) {
  const bool want = atomic_load(&s_ready) && atomic_load(&s_enabled) && atomic_load(&s_plugged) &&
                    !atomic_load(&s_given_up);

  usb_power_request(USB_POWER_DRIVE, want);
}

static void prv_host_check(struct pbl_work *work) {
  if (atomic_load(&s_plugged) && !pbl_usb_msc_is_configured() && !atomic_load(&s_given_up)) {
    PBL_LOG_INFO("No USB host: the drive stays off until the next plug-in");
    atomic_store(&s_given_up, true);
    prv_apply(nullptr);
  }
}

static void prv_session_start(void) {
  atomic_store(&s_given_up, false);
  atomic_store(&s_ejected, false);
  (void)pbl_delayed_work_reschedule(&pbl_sys_workq, &s_host_check, PBL_MSEC(HOST_WAIT_MS));
}

static void prv_plugged(bool plugged) {
  if (plugged && !atomic_exchange(&s_plugged, true)) {
    prv_session_start();
  } else if (!plugged) {
    atomic_store(&s_plugged, false);
  }
  (void)pbl_sys_work_call(prv_apply, nullptr);
}

static void prv_battery_connection(PebbleEvent *e, void *context) {
  prv_plugged(e->battery_connection.is_connected);
}

void usb_drive_init(void) {
  uint8_t id[16];
  size_t len = sizeof(id);

  if (mcu_get_serial(id, &len) == 0) {
    for (size_t i = 0; i < len; i++) {
      s.serial = (s.serial << 5 | s.serial >> 27) ^ id[i];
    }
  }
  prv_cache_clear();
  atomic_store(&s_enabled, prv_load());
  pbl_delayed_work_init(&s_host_check, prv_host_check);
  pbl_usb_msc_init(&s_medium);
  atomic_store(&s_ready, true);

  s_battery_event_info = (EventServiceInfo){
    .type = PEBBLE_BATTERY_CONNECTION_EVENT,
    .handler = prv_battery_connection,
  };
  event_service_client_subscribe(&s_battery_event_info);
  prv_plugged(battery_is_usb_connected());
}

bool usb_drive_get_enabled(void) {
  return atomic_load(&s_enabled);
}

void usb_drive_set_enabled(bool enabled) {
  atomic_store(&s_enabled, enabled);
  prv_store(enabled);
  if (enabled) {
    prv_session_start();
    pbl_usb_msc_changed();
  }
  (void)pbl_sys_work_call(prv_apply, nullptr);
}

void usb_drive_contents_changed(void) {
  atomic_store(&s_dirty, true);
}

#ifdef CONFIG_SHELL
static const char *const s_state_names[] = {"no volume", "sizing", "ready"};

static int prv_cmd(const struct pbl_shell *sh, size_t argc, char **argv) {
  if (argc > 1) {
    if (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0) {
      usb_drive_set_enabled(strcmp(argv[1], "on") == 0);
    } else if (strcmp(argv[1], "insert") == 0) {
      prv_session_start();
      pbl_usb_msc_changed();
      (void)pbl_sys_work_call(prv_apply, nullptr);
    } else if (strcmp(argv[1], "eject") == 0) {
      prv_eject(nullptr);
    } else if (strcmp(argv[1], "changed") == 0) {
      usb_drive_contents_changed();
    } else {
      pbl_shell_error(sh, "expected on, off, insert, eject or changed");
      return -1;
    }
  }

  pbl_shell_print(
      sh, "setting %s, %s%s%s, USB %s, host %s, %s, %u workouts",
      atomic_load(&s_enabled) ? "on" : "off", atomic_load(&s_plugged) ? "plugged in" : "unplugged",
      atomic_load(&s_given_up) ? ", given up" : "", atomic_load(&s_ejected) ? ", ejected" : "",
      usb_power_is_on() ? "on" : "off", pbl_usb_msc_is_configured() ? "configured" : "none",
      s_state_names[s.state], (unsigned int)s.num_files);
  pbl_shell_print(sh,
                  "reads %" PRIu32 ", blocks %" PRIu32 " cached %" PRIu32
                  " encoded, records %" PRIu32 ", passes %" PRIu32 ", sizing %" PRIu32 " ms",
                  s.reads, s.hits, s.misses, s.records, s.restarts, s.sizing_ms);
  for (size_t i = 0; i < s.num_files && s.state == STATE_READY; i++) {
    const struct prv_known *k = &s.known[s.file_known[i]];

    pbl_shell_print(sh, "%08" PRIx32 " %s %" PRIu32 " bytes, %u marks%s", k->start_utc,
                    s.files[i].name, k->size, k->num_marks, k->crc_known ? ", CRC known" : "");
  }

  return 0;
}

PBL_SHELL_CMD_ARG_REGISTER(usbdrive, nullptr,
                           "The workouts' USB drive [on|off|insert|eject|changed]", prv_cmd, 1, 1);
#endif
