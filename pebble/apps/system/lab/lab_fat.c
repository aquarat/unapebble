/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

// The update drive as the eMMC builds read it (lib/fat_ro): its root files, the resource packs
// among them and the version check that picks one, without copying; then, on demand, the copy
// into the eMMC builds' resource bank 0, as they copy it.

#include "../../../../drivers/flash/blk_nor.h"
#include "lab.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include <pbl/boot_trace/boot_trace.h>
#include <pbl/crc/crc.h>
#include <pbl/drivers/mmc.h>
#include <pbl/fat_ro/fat_ro.h>
#include <pbl/kernel/time.h>
#include <pbl/util/math.h>
#include <pbl/util/size.h>

#include <resource/resource.h>
#include <resource/resource_storage_impl.h>
#include <resource/resource_version.auto.h>

#define MAX_FILES 32U
#define CHUNK     4096U

struct pack {
  struct fat_ro_file file;
  ResourceManifest manifest;
  bool is_pack;
};

static struct {
  struct fat_ro *fs;
  struct pack *packs;
  uint32_t num_files;
  int match;
  uint8_t *chunk;
  uint8_t *back;
  struct blk_nor *nor;
} s;

static int prv_read_sectors(void *ctx, uint32_t sector, void *buf, uint32_t count) {
  return lab_mmc_read(LAB_UPD_FIRST + sector, buf, count);
}

static int prv_clock(void) {
  s.fs = lab_alloc(sizeof(*s.fs));
  s.packs = lab_alloc(MAX_FILES * sizeof(*s.packs));
  if (s.fs == nullptr || s.packs == nullptr) {
    return -ENOMEM;
  }
  s.num_files = 0U;
  s.match = -1;

  return lab_emmc_clock_select(lab_emmc_clock_is_pll1_q());
}

static int prv_mount(void) {
  static const char *const s_types[] = {"?", "FAT16", "FAT32", "exFAT"};
  const uint64_t start = pbl_uptime_ms();
  int ret;

  ret = fat_ro_mount(s.fs, prv_read_sectors, nullptr, LAB_UPD_BLOCKS);
  lab_printf("mount: %d, %lu ms", ret, (unsigned long)(pbl_uptime_ms() - start));
  if (ret == 0) {
    lab_printf("%s", s_types[s.fs->type < PBL_ARRAY_SIZE(s_types) ? s.fs->type : 0]);
  }

  return ret;
}

static int prv_file_cb(struct fat_ro *fs, struct fat_ro_file *file, void *arg) {
  struct pack *p;

  if (s.num_files == MAX_FILES) {
    return 1;
  }
  p = &s.packs[s.num_files++];
  p->file = *file;
  p->is_pack = false;
  memset(&p->manifest, 0, sizeof(p->manifest));
  if (file->size >= MANIFEST_SIZE &&
      fat_ro_read(fs, &p->file, 0U, &p->manifest, sizeof(p->manifest)) == 0) {
    p->is_pack = p->manifest.num_resources > 0U &&
                 p->manifest.num_resources <= MAX_RESOURCES_FOR_SYSTEM_STORE;
  }

  return 0;
}

static int prv_list(void) {
  int ret = fat_ro_foreach_root_file(s.fs, prv_file_cb, nullptr);

  if (ret < 0) {
    return ret;
  }
  lab_printf("%lu files in the root", (unsigned long)s.num_files);
  for (uint32_t i = 0U; i < s.num_files; i++) {
    const struct pack *p = &s.packs[i];

    if (p->is_pack) {
      lab_printf("#%lu %lu B, pack?", (unsigned long)i, (unsigned long)p->file.size);
      lab_printf("  %lu res, crc 0x%08" PRIx32, (unsigned long)p->manifest.num_resources,
                 p->manifest.version.crc);
    } else {
      lab_printf("#%lu %lu B%s", (unsigned long)i, (unsigned long)p->file.size,
                 p->file.contiguous ? ", contig." : "");
    }
  }

  return 0;
}

// The version the eMMC builds of this tree want, and the one the run before noted
static int prv_check(void) {
  const struct pbl_boot_trace_boot *boot = pbl_boot_trace_boot_info();
  uint32_t noted = 0U;

  lab_printf("this build wants 0x%08" PRIx32, SYSTEM_RESOURCE_VERSION.crc);
  if (boot->ram_valid && (boot->previous.notes_set & (1U << PBL_BT_NOTE_RES_CRC)) != 0U) {
    noted = boot->previous.notes[PBL_BT_NOTE_RES_CRC];
    lab_printf("run before wanted 0x%08" PRIx32, noted);
  }

  for (uint32_t i = 0U; i < s.num_files; i++) {
    const struct pack *p = &s.packs[i];

    if (!p->is_pack) {
      continue;
    }
    if (resource_version_matches(&p->manifest.version, &SYSTEM_RESOURCE_VERSION)) {
      lab_printf("#%lu matches this build", (unsigned long)i);
      if (s.match < 0) {
        s.match = (int)i;
      }
    }
    if (noted != 0U && p->manifest.version.crc == noted) {
      lab_printf("#%lu matches the run before", (unsigned long)i);
      s.match = (int)i;
    }
  }
  if (s.match < 0) {
    lab_printf("NO matching pack: an eMMC build of this version would power off");
    return -ENOENT;
  }
  if (s.packs[s.match].file.size > LAB_B_BANK_SIZE) {
    lab_printf("pack larger than a bank");
    return -EFBIG;
  }
  lab_printf("would copy #%d, %lu B", s.match, (unsigned long)s.packs[s.match].file.size);

  return 0;
}

static int prv_read_through(void) {
  const struct pack *p = &s.packs[s.match];
  const uint64_t start = pbl_uptime_ms();
  struct fat_ro_file file = p->file;
  uint32_t crc = 0U;
  int ret;

  s.chunk = lab_alloc(CHUNK);
  if (s.chunk == nullptr) {
    return -ENOMEM;
  }
  for (uint32_t off = 0U; off < p->file.size; off += CHUNK) {
    const uint32_t n = PBL_MIN(CHUNK, p->file.size - off);

    ret = fat_ro_read(s.fs, &file, off, s.chunk, n);
    if (ret != 0) {
      lab_printf("read at %lu: %d", (unsigned long)off, ret);
      return ret;
    }
    crc = pbl_crc32(crc, s.chunk, n);
  }
  lab_printf("read all in %lu ms, crc 0x%08" PRIx32, (unsigned long)(pbl_uptime_ms() - start), crc);

  return 0;
}

static void prv_cleanup(void) {
  pbl_mmc_power_off(MMC);
  memset(&s, 0, sizeof(s));
}

static const struct lab_step s_steps[] = {
  {"kernel clock", prv_clock},
  {"mount update drive", prv_mount},
  {"list root files", prv_list},
  {"version check (dry run)", prv_check},
  {"read the pack through", prv_read_through},
};

const struct lab_experiment lab_exp_fat = {
  .title = "Update drive",
  .subtitle = "FAT, packs, version check",
  .steps = s_steps,
  .num_steps = PBL_ARRAY_SIZE(s_steps),
  .cleanup = prv_cleanup,
};

// The copy into bank 0 of the eMMC builds' window, through the NOR emulation, as they copy

static int prv_hw_read(void *ctx, uint32_t block, void *buf, uint32_t count) {
  return lab_mmc_read(LAB_WINDOW_FIRST + block, buf, count);
}

static int prv_hw_write(void *ctx, uint32_t block, const void *buf, uint32_t count) {
  if (block + count > LAB_B_BANK_SIZE / 512U) {
    return -EACCES;
  }

  return lab_mmc_write(LAB_WINDOW_FIRST + block, buf, count);
}

static const struct blk_nor_hw s_hw = {
  .read = prv_hw_read,
  .write = prv_hw_write,
};

static int prv_copy(void) {
  const struct pack *p = &s.packs[s.match];
  const uint32_t erase = PBL_DIV_ROUND_UP(p->file.size, 4096U) * 4096U;
  struct fat_ro_file file = p->file;
  uint64_t start = pbl_uptime_ms();
  int ret;

  s.nor = lab_alloc(sizeof(*s.nor));
  if (s.nor == nullptr) {
    return -ENOMEM;
  }
  blk_nor_init(s.nor, &s_hw, nullptr);
  ret = blk_nor_erase(s.nor, LAB_B_BANK0_OFFSET, erase);
  lab_printf("erase %lu KiB: %d, %lu ms", (unsigned long)(erase / 1024U), ret,
             (unsigned long)(pbl_uptime_ms() - start));
  if (ret != 0) {
    return ret;
  }

  start = pbl_uptime_ms();
  for (uint32_t off = 0U; off < p->file.size; off += CHUNK) {
    const uint32_t n = PBL_MIN(CHUNK, p->file.size - off);

    ret = fat_ro_read(s.fs, &file, off, s.chunk, n);
    if (ret == 0) {
      ret = blk_nor_write(s.nor, LAB_B_BANK0_OFFSET + off, s.chunk, n);
    }
    if (ret != 0) {
      lab_printf("at %lu: %d", (unsigned long)off, ret);
      return ret;
    }
  }
  lab_printf("copied in %lu ms", (unsigned long)(pbl_uptime_ms() - start));

  return 0;
}

static int prv_verify(void) {
  const struct pack *p = &s.packs[s.match];
  struct fat_ro_file file = p->file;
  int ret;

  s.back = lab_alloc(CHUNK);
  if (s.back == nullptr) {
    return -ENOMEM;
  }
  for (uint32_t off = 0U; off < p->file.size; off += CHUNK) {
    const uint32_t n = PBL_MIN(CHUNK, p->file.size - off);

    ret = fat_ro_read(s.fs, &file, off, s.chunk, n);
    if (ret == 0) {
      ret = blk_nor_read(s.nor, LAB_B_BANK0_OFFSET + off, s.back, n);
    }
    if (ret != 0) {
      return ret;
    }
    if (memcmp(s.chunk, s.back, n) != 0) {
      lab_printf("DIFFERS at %lu", (unsigned long)off);
      return -EIO;
    }
  }
  lab_printf("bank 0 holds the pack");

  return 0;
}

static const struct lab_step s_copy_steps[] = {
  {"kernel clock", prv_clock},
  {"mount update drive", prv_mount},
  {"list root files", prv_list},
  {"version check", prv_check},
  {"read the pack through", prv_read_through},
  {"erase bank 0 and copy", prv_copy},
  {"compare with the file", prv_verify},
};

const struct lab_experiment lab_exp_copy = {
  .title = "Copy resources",
  .subtitle = "Pack to eMMC bank 0, as B",
  .steps = s_copy_steps,
  .num_steps = PBL_ARRAY_SIZE(s_copy_steps),
  .cleanup = prv_cleanup,
  .confirm =
      "Copies the matching .pbpack of the update drive into resource bank 0 of the eMMC "
      "builds (GUI region, 200 MiB in), as they do at boot.",
};
