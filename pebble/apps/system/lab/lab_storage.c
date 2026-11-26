/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

// What the eMMC builds left in their window of the GUI region, read only: the resource banks,
// the core dump storage, the filesystem's page headers, and their flash log, whose newest
// generation is the last boot of one of them.

#include "lab.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <pbl/coredump/format.h>
#include <pbl/drivers/mmc.h>
#include <pbl/kernel/compiler.h>
#include <pbl/kernel/time.h>
#include <pbl/util/build_id.h>
#include <pbl/util/math.h>
#include <pbl/util/size.h>

#include <resource/resource.h>
#include <resource/resource_storage_impl.h>
#include <resource/resource_version.auto.h>

#define BLOCK_OF(off) (LAB_WINDOW_FIRST + (off) / 512U)

static uint8_t *s_buf;

static int prv_clock(void) {
  s_buf = lab_alloc(4096U);
  if (s_buf == nullptr) {
    return -ENOMEM;
  }

  return lab_emmc_clock_select(lab_emmc_clock_is_pll1_q());
}

static int prv_read(uint32_t off, uint32_t blocks) {
  return lab_mmc_read(BLOCK_OF(off), s_buf, blocks);
}

static int prv_bank(uint32_t off, unsigned int n) {
  ResourceManifest m;
  int ret = prv_read(off, 1U);

  if (ret != 0) {
    return ret;
  }
  memcpy(&m, s_buf, sizeof(m));
  if (m.num_resources == 0xFFFFFFFFU) {
    lab_printf("bank %u: erased", n);
  } else if (m.num_resources == 0U || m.num_resources > MAX_RESOURCES_FOR_SYSTEM_STORE) {
    lab_printf("bank %u: no pack", n);
    lab_hex("", s_buf, 12U);
  } else {
    lab_printf("bank %u: %lu res, crc 0x%08" PRIx32, n, (unsigned long)m.num_resources,
               m.version.crc);
    lab_printf("  %s", resource_version_matches(&m.version, &SYSTEM_RESOURCE_VERSION)
                           ? "this build's version"
                           : "another version");
  }

  return 0;
}

static int prv_banks(void) {
  int ret = prv_bank(LAB_B_BANK0_OFFSET, 0U);

  if (ret == 0) {
    ret = prv_bank(LAB_B_BANK1_OFFSET, 1U);
  }

  return ret;
}

// The flash backend of the core dumps (subsys/coredump/backends/flash.c)
#define CD_FLASH_MAGIC 0x464C5300U

static int prv_coredump(void) {
  uint32_t w[4];
  int ret = prv_read(LAB_B_SCRATCH, 1U);

  if (ret != 0) {
    return ret;
  }
  memcpy(w, s_buf, sizeof(w));
  if (w[0] != CD_FLASH_MAGIC) {
    lab_printf("no core dump storage (0x%08" PRIx32 ")", w[0]);
    return 0;
  }
  lab_printf("storage, unused slots 0x%08" PRIx32, w[1]);
  ret = prv_read(LAB_B_SCRATCH + 4096U, 1U);
  if (ret != 0) {
    return ret;
  }
  memcpy(w, s_buf, sizeof(w));
  if (w[0] != CD_FLASH_MAGIC) {
    lab_printf("slot 0 empty");
    return 0;
  }
  lab_printf("slot 0: #%lu, %s", (unsigned long)w[1], (s_buf[8] != 0U) ? "UNREAD" : "read");
  memcpy(w, &s_buf[12], sizeof(w));
  lab_printf("image magic 0x%08" PRIx32 " %s", w[0], w[0] == PBL_COREDUMP_MAGIC ? "ok" : "BAD");
  lab_printf("time %lu", (unsigned long)w[2]);

  return 0;
}

// PFS's page headers, the first bytes of each 4 KiB page
#define PFS_MAGIC 0x50U

static int prv_pfs(void) {
  const uint32_t pages = LAB_B_FS_SIZE / 4096U;
  const uint64_t start = pbl_uptime_ms();
  uint32_t pfs = 0U;
  uint32_t erased = 0U;
  uint32_t zero = 0U;
  uint32_t other = 0U;
  uint32_t first_other = UINT32_MAX;
  int ret;

  for (uint32_t p = 0U; p < pages; p++) {
    ret = prv_read(LAB_B_FS_OFFSET + p * 4096U, 1U);
    if (ret != 0) {
      lab_printf("page %lu: %d", (unsigned long)p, ret);
      return ret;
    }
    const uint16_t version = (uint16_t)(s_buf[0] | (s_buf[1] << 8));

    if (p == 0U) {
      lab_hex("page 0", s_buf, 16U);
    }
    if ((version >> 8) == PFS_MAGIC && version <= 0x5001U) {
      pfs++;
    } else if (version == 0xFFFFU) {
      erased++;
    } else if (version == 0x0000U) {
      zero++;
    } else {
      other++;
      first_other = PBL_MIN(first_other, p);
    }
  }
  lab_printf("%lu pages in %lu ms", (unsigned long)pages, (unsigned long)(pbl_uptime_ms() - start));
  lab_printf("PFS %lu, FF %lu", (unsigned long)pfs, (unsigned long)erased);
  lab_printf("00 %lu, other %lu", (unsigned long)zero, (unsigned long)other);
  if (first_other != UINT32_MAX) {
    lab_printf("first other: page %lu", (unsigned long)first_other);
  }
  lab_printf("%s",
             pfs == pages ? "formatted" : (pfs == 0U ? "never formatted" : "PARTLY formatted"));

  return 0;
}

static void prv_cleanup(void) {
  pbl_mmc_power_off(MMC);
  s_buf = nullptr;
}

static const struct lab_step s_steps[] = {
  {"kernel clock", prv_clock},
  {"resource banks", prv_banks},
  {"core dump storage", prv_coredump},
  {"filesystem page headers", prv_pfs},
};

const struct lab_experiment lab_exp_b_storage = {
  .title = "B's storage",
  .subtitle = "Banks, core dump, PFS (read)",
  .steps = s_steps,
  .num_steps = PBL_ARRAY_SIZE(s_steps),
  .cleanup = prv_cleanup,
};

// The flash log (subsys/logging/backends/flash_storage.c): 4 KiB pages, each with a header

#define LOG_MAGIC     0x21474F4CU
#define LOG_PAGE_SIZE 4096U
#define LOG_PAGES     (LAB_B_DEBUG_SIZE / LOG_PAGE_SIZE)
#define MAX_LINES     60U

typedef struct PBL_PACKED {
  uint32_t magic;
  uint8_t version;
  uint8_t build_id[BUILD_ID_EXPECTED_LEN];
  uint8_t log_file_id;
  uint8_t log_chunk_id;
  uint8_t log_flags;
} LogPageHeader;

typedef struct PBL_PACKED {
  uint32_t timestamp;
  uint8_t log_level;
  uint8_t message_length;
  uint16_t line_number;
  char filename[16];
  char message[];
} LogMessage;

static struct {
  uint8_t ids[LOG_PAGES];
  uint8_t chunks[LOG_PAGES];
  bool valid[LOG_PAGES];
  char (*lines)[96];
  uint32_t num_lines;
  uint32_t total;
} *s_log;

static int prv_log_scan(void) {
  uint32_t valid = 0U;
  uint32_t erased = 0U;
  int ret;

  s_log = lab_alloc(sizeof(*s_log));
  if (s_log == nullptr) {
    return -ENOMEM;
  }
  s_log->lines = lab_alloc(MAX_LINES * sizeof(s_log->lines[0]));
  if (s_log->lines == nullptr) {
    return -ENOMEM;
  }
  for (uint32_t p = 0U; p < LOG_PAGES; p++) {
    LogPageHeader hdr;

    ret = prv_read(LAB_B_DEBUG_DB + p * LOG_PAGE_SIZE, 1U);
    if (ret != 0) {
      return ret;
    }
    memcpy(&hdr, s_buf, sizeof(hdr));
    s_log->valid[p] = hdr.magic == LOG_MAGIC && hdr.version == 1U;
    s_log->ids[p] = hdr.log_file_id;
    s_log->chunks[p] = hdr.log_chunk_id;
    valid += s_log->valid[p] ? 1U : 0U;
    erased += hdr.magic == 0xFFFFFFFFU ? 1U : 0U;
  }
  lab_printf("%lu log pages, %lu erased", (unsigned long)valid, (unsigned long)erased);

  return valid != 0U ? 0 : -ENOENT;
}

static bool prv_has_id(uint8_t id) {
  for (uint32_t p = 0U; p < LOG_PAGES; p++) {
    if (s_log->valid[p] && s_log->ids[p] == id) {
      return true;
    }
  }

  return false;
}

static char prv_level(uint8_t level) {
  if (level == 0U) {
    return 'A';
  }
  if (level <= 1U) {
    return 'E';
  }
  if (level <= 50U) {
    return 'W';
  }
  if (level <= 100U) {
    return 'I';
  }

  return 'D';
}

static void prv_keep_line(const LogMessage *m, uint32_t len) {
  char *line = s_log->lines[s_log->num_lines % MAX_LINES];
  const uint32_t text = PBL_MIN((uint32_t)m->message_length, len - (uint32_t)sizeof(*m));
  char name[17];

  memcpy(name, m->filename, 16U);
  name[16] = '\0';
  snprintf(line, sizeof(s_log->lines[0]), "%c %lu.%03lu %s:%u %.*s", prv_level(m->log_level),
           (unsigned long)(m->timestamp / 1000U), (unsigned long)(m->timestamp % 1000U), name,
           m->line_number, (int)PBL_MIN(text, 56U), m->message);
  s_log->num_lines++;
}

static int prv_log_dump(void) {
  uint8_t newest = 0U;
  bool found = false;
  uint32_t pages = 0U;
  int ret;

  // The newest generation: one no other follows
  for (uint32_t p = 0U; p < LOG_PAGES && !found; p++) {
    if (s_log->valid[p] && !prv_has_id((uint8_t)(s_log->ids[p] + 1U))) {
      newest = s_log->ids[p];
      found = true;
    }
  }
  if (!found) {
    return -ENOENT;
  }
  lab_printf("newest generation %u", newest);

  // Its pages in the order of their chunks
  for (uint32_t chunk = 0U; chunk < 256U; chunk++) {
    for (uint32_t p = 0U; p < LOG_PAGES; p++) {
      if (!s_log->valid[p] || s_log->ids[p] != newest || s_log->chunks[p] != chunk) {
        continue;
      }
      pages++;
      ret = prv_read(LAB_B_DEBUG_DB + p * LOG_PAGE_SIZE, LOG_PAGE_SIZE / 512U);
      if (ret != 0) {
        return ret;
      }
      for (uint32_t off = sizeof(LogPageHeader); off + 2U < LOG_PAGE_SIZE;) {
        const uint8_t flags = s_buf[off];
        const uint8_t len = s_buf[off + 1U];

        if (len == 0U || len == 0xFFU || off + 2U + len > LOG_PAGE_SIZE) {
          break;
        }
        if ((flags & 1U) == 0U && len >= sizeof(LogMessage)) {
          prv_keep_line((const LogMessage *)&s_buf[off + 2U], len);
        }
        off += 2U + len;
      }
    }
  }
  lab_printf("%lu pages, %lu lines", (unsigned long)pages, (unsigned long)s_log->num_lines);
  lab_printf("-- the last %lu --", (unsigned long)PBL_MIN(s_log->num_lines, MAX_LINES));
  for (uint32_t k = (s_log->num_lines > MAX_LINES) ? s_log->num_lines - MAX_LINES : 0U;
       k < s_log->num_lines; k++) {
    lab_printf("%s", s_log->lines[k % MAX_LINES]);
  }

  return 0;
}

static void prv_log_cleanup(void) {
  prv_cleanup();
  s_log = nullptr;
}

static const struct lab_step s_log_steps[] = {
  {"kernel clock", prv_clock},
  {"log page headers", prv_log_scan},
  {"newest generation", prv_log_dump},
};

const struct lab_experiment lab_exp_b_logs = {
  .title = "B's logs",
  .subtitle = "Its last boot's flash log",
  .steps = s_log_steps,
  .num_steps = PBL_ARRAY_SIZE(s_log_steps),
  .cleanup = prv_log_cleanup,
};
