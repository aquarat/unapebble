/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pbl/fat_ro/fat_ro.h>
#include <pbl/vfat/vfat.h>

#include <clar.h>

// The volume as hosts read it: the boot sector's layout, the FATs' cluster chains, the directory
// with its long names and aliases, the files' data, read back with lib/fat_ro and decoded here.
// With VFAT_IMAGE set, the volume of test_vfat__image is also written to that file, for fsck.fat
// and other readers.

#define SS VFAT_SECTOR_SIZE

static struct vfat s_vol;
static struct vfat_file s_files[100];
static unsigned int s_reads;
static unsigned int s_read_bytes;

static uint16_t prv_get16(const uint8_t *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t prv_get32(const uint8_t *p) {
  return (uint32_t)prv_get16(p) | ((uint32_t)prv_get16(&p[2]) << 16);
}

static uint8_t prv_content(size_t file, uint32_t offset) {
  return (uint8_t)(file * 37U + offset * 7U + (offset >> 9));
}

static int prv_data(void *ctx, size_t file, uint32_t offset, void *buf, uint32_t len) {
  uint8_t *b = buf;

  cl_assert(file < s_vol.num_files || ctx != NULL);
  cl_assert(offset + len <= s_files[file].size);
  for (uint32_t i = 0U; i < len; i++) {
    b[i] = prv_content(file, offset + i);
  }
  s_reads++;
  s_read_bytes += len;

  return 0;
}

static int prv_failing(void *ctx, size_t file, uint32_t offset, void *buf, uint32_t len) {
  return -EIO;
}

static int prv_fat_ro_read(void *ctx, uint32_t sector, void *buf, uint32_t count) {
  return vfat_read(&s_vol, sector, buf, count);
}

static void prv_sector(uint32_t lba, uint8_t *b) {
  cl_must_pass(vfat_read(&s_vol, lba, b, 1U));
}

static void prv_file(size_t i, const char *name, uint32_t size) {
  memset(&s_files[i], 0, sizeof(s_files[i]));
  snprintf(s_files[i].name, sizeof(s_files[i].name), "%s", name);
  s_files[i].size = size;
  s_files[i].date = vfat_date(2026, 11, 28);
  s_files[i].time = vfat_time(10, 45, 30);
}

static int prv_layout(size_t n) {
  const struct vfat_params p = {
    .label = "UNA WATCH",
    .serial = 0x12345678U,
    .date = vfat_date(2026, 11, 28),
    .time = vfat_time(12, 0, 0),
    .read = prv_data,
  };

  return vfat_layout(&s_vol, &p, s_files, n);
}

struct boot {
  uint32_t spc;
  uint32_t reserved;
  uint32_t fats;
  uint32_t root_entries;
  uint32_t total;
  uint32_t fat_size;
  uint32_t root;
  uint32_t data;
  uint32_t clusters;
};

static struct boot prv_boot(void) {
  uint8_t b[SS];
  struct boot bs;

  prv_sector(0, b);
  bs.spc = b[13];
  bs.reserved = prv_get16(&b[14]);
  bs.fats = b[16];
  bs.root_entries = prv_get16(&b[17]);
  bs.total = prv_get16(&b[19]) != 0U ? prv_get16(&b[19]) : prv_get32(&b[32]);
  bs.fat_size = prv_get16(&b[22]);
  bs.root = bs.reserved + bs.fats * bs.fat_size;
  bs.data = bs.root + bs.root_entries * 32U / SS;
  bs.clusters = (bs.total - bs.data) / bs.spc;

  return bs;
}

static uint16_t prv_fat(const struct boot *bs, uint32_t fat, uint32_t c) {
  uint8_t b[SS];

  prv_sector(bs->reserved + fat * bs->fat_size + c * 2U / SS, b);
  return prv_get16(&b[(c * 2U) % SS]);
}

// A directory entry of the root, by its index
static void prv_entry(const struct boot *bs, uint32_t e, uint8_t d[32]) {
  uint8_t b[SS];

  prv_sector(bs->root + e * 32U / SS, b);
  memcpy(d, &b[(e * 32U) % SS], 32);
}

static uint8_t prv_checksum(const uint8_t *sfn) {
  uint8_t sum = 0U;

  for (int i = 0; i < 11; i++) {
    sum = (uint8_t)(((sum & 1U) << 7) + (sum >> 1) + sfn[i]);
  }
  return sum;
}

struct dirent {
  char name[256];
  uint8_t sfn[11];
  uint8_t attr;
  uint16_t cluster;
  uint32_t size;
  uint16_t date;
  uint16_t time;
};

// The directory as a host decodes it: long names checked against their alias's checksum and
// their ordinals, until the end marker
static size_t prv_readdir(const struct boot *bs, struct dirent *out, size_t max) {
  static const uint8_t s_pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
  char lfn[256] = "";
  int expect = 0;
  uint8_t sum = 0U;
  size_t n = 0U;

  for (uint32_t e = 0U; e < bs->root_entries; e++) {
    uint8_t d[32];

    prv_entry(bs, e, d);
    if (d[0] == 0x00U) {
      break;
    }
    cl_assert(d[0] != 0xE5U);
    if (d[11] == 0x0FU) {
      const int ord = d[0] & 0x3F;

      if ((d[0] & 0x40U) != 0U) {
        memset(lfn, 0, sizeof(lfn));
        expect = ord;
        sum = d[13];
      }
      cl_assert_equal_i(ord, expect);
      cl_assert_equal_i(d[13], sum);
      cl_assert_equal_i(prv_get16(&d[26]), 0);
      for (int k = 0; k < 13; k++) {
        const uint16_t ch = prv_get16(&d[s_pos[k]]);

        if (ch != 0x0000U && ch != 0xFFFFU) {
          cl_assert(ch < 0x80U);
          lfn[(ord - 1) * 13 + k] = (char)ch;
        }
      }
      expect--;
      continue;
    }

    cl_assert(n < max);
    memset(&out[n], 0, sizeof(out[n]));
    memcpy(out[n].sfn, d, 11);
    out[n].attr = d[11];
    out[n].cluster = prv_get16(&d[26]);
    out[n].size = prv_get32(&d[28]);
    out[n].date = prv_get16(&d[24]);
    out[n].time = prv_get16(&d[22]);
    if (lfn[0] != '\0') {
      cl_assert_equal_i(expect, 0);
      cl_assert_equal_i(prv_checksum(d), sum);
      memcpy(out[n].name, lfn, sizeof(lfn));
      lfn[0] = '\0';
    } else {
      memcpy(out[n].name, d, 11);
    }
    n++;
  }

  return n;
}

void test_vfat__initialize(void) {
  memset(s_files, 0, sizeof(s_files));
  s_reads = 0U;
  s_read_bytes = 0U;
}

void test_vfat__boot_sector_and_geometry(void) {
  uint8_t b[SS];
  struct boot bs;

  cl_assert_equal_i(prv_layout(0), 0);
  prv_sector(0, b);
  bs = prv_boot();

  cl_assert_equal_i(b[0], 0xEB);
  cl_assert_equal_i(b[2], 0x90);
  cl_assert_equal_i(prv_get16(&b[11]), SS);
  cl_assert_equal_i(bs.spc, 8);
  cl_assert_equal_i(bs.fats, 2);
  cl_assert_equal_i(bs.root_entries, 512);
  cl_assert_equal_i(bs.total, 262144);
  cl_assert_equal_i(vfat_num_sectors(&s_vol), 262144);
  cl_assert_equal_i(b[21], 0xF8);
  cl_assert_equal_i(b[38], 0x29);
  cl_assert_equal_i(prv_get32(&b[39]), 0x12345678U);
  cl_assert(memcmp(&b[43], "UNA WATCH  ", 11) == 0);
  cl_assert(memcmp(&b[54], "FAT16   ", 8) == 0);
  cl_assert(memcmp(&b[3], "MSWIN4.1", 8) == 0);
  cl_assert_equal_i(b[510], 0x55);
  cl_assert_equal_i(b[511], 0xAA);

  // FAT16 by its cluster count, a FAT that holds every cluster, the data on a cluster boundary
  cl_assert(bs.clusters >= 4085U && bs.clusters < 65525U);
  cl_assert(bs.fat_size * SS / 2U >= bs.clusters + 2U);
  cl_assert_equal_i(bs.data % bs.spc, 0);
  cl_assert_equal_i(prv_fat(&bs, 0, 0), 0xFFF8);
  cl_assert_equal_i(prv_fat(&bs, 0, 1), 0xFFFF);
  cl_assert_equal_i(prv_fat(&bs, 1, 0), 0xFFF8);
  cl_assert_equal_i(prv_fat(&bs, 0, 2), 0);

  // The label in the root directory too
  {
    uint8_t d[32];

    prv_entry(&bs, 0, d);
    cl_assert(memcmp(d, "UNA WATCH  ", 11) == 0);
    cl_assert_equal_i(d[11], 0x08);
    prv_entry(&bs, 1, d);
    cl_assert_equal_i(d[0], 0);
  }

  // Past the end
  cl_assert_equal_i(vfat_read(&s_vol, 262144, b, 1), -EINVAL);
  cl_assert_equal_i(vfat_read(&s_vol, 262143, b, 2), -EINVAL);
  cl_must_pass(vfat_read(&s_vol, 262143, b, 1));
}

void test_vfat__sizes_that_make_no_fat16(void) {
  struct vfat_params p = {.read = prv_data, .total_sectors = 8192U};

  // 4 MiB of 4 KiB clusters: FAT12's
  cl_assert_equal_i(vfat_layout(&s_vol, &p, s_files, 0), -EINVAL);
  // 1 GiB of them: FAT32's
  p.total_sectors = 2097152U;
  cl_assert_equal_i(vfat_layout(&s_vol, &p, s_files, 0), -EINVAL);
  // But 1 GiB of 32 KiB clusters is FAT16
  p.sectors_per_cluster = 64U;
  cl_assert_equal_i(vfat_layout(&s_vol, &p, s_files, 0), 0);
  p.sectors_per_cluster = 3U;
  cl_assert_equal_i(vfat_layout(&s_vol, &p, s_files, 0), -EINVAL);
  // The smallest FAT16 volume of 512-byte clusters
  p.sectors_per_cluster = 1U;
  p.total_sectors = 4085U + 1U + 2U * 16U + 32U;
  cl_assert_equal_i(vfat_layout(&s_vol, &p, s_files, 0), 0);
  cl_assert(s_vol.num_clusters >= 4085U);
}

void test_vfat__files_long_names_and_chains(void) {
  struct dirent ents[8];
  struct boot bs;
  size_t n;

  prv_file(0, "2026-11-28-1045-run.fit", 95123U);
  prv_file(1, "2026-11-28-1210-walk.fit", 4096U);
  prv_file(2, "2026-11-29-0700-bike.fit", 1U);
  prv_file(3, "README.TXT", 600U);
  prv_file(4, "empty.fit", 0U);
  prv_file(5, "a name with spaces and a long tail to need four entries.fit", 513U);
  cl_assert_equal_i(prv_layout(6), 6);
  bs = prv_boot();

  n = prv_readdir(&bs, ents, 8);
  cl_assert_equal_i(n, 7);
  cl_assert_equal_i(ents[0].attr, 0x08);
  cl_assert_equal_s(ents[1].name, "2026-11-28-1045-run.fit");
  cl_assert(memcmp(ents[1].sfn, "2026-1~1FIT", 11) == 0);
  cl_assert_equal_s(ents[2].name, "2026-11-28-1210-walk.fit");
  cl_assert(memcmp(ents[2].sfn, "2026-1~2FIT", 11) == 0);
  cl_assert(memcmp(ents[3].sfn, "2026-1~3FIT", 11) == 0);
  // An 8.3 name needs no long name
  cl_assert(memcmp(ents[4].name, "README  TXT", 11) == 0);
  cl_assert(memcmp(ents[5].sfn, "EMPTY~1 FIT", 11) == 0);
  cl_assert_equal_i(ents[5].cluster, 0);
  cl_assert_equal_i(ents[5].size, 0);
  cl_assert_equal_s(ents[6].name, "a name with spaces and a long tail to need four entries.fit");
  cl_assert(memcmp(ents[6].sfn, "ANAMEW~1FIT", 11) == 0);
  cl_assert_equal_i(ents[1].date, vfat_date(2026, 11, 28));
  cl_assert_equal_i(ents[1].time, vfat_time(10, 45, 30));
  cl_assert_equal_i(ents[1].attr, 0);

  // Each file's clusters one after the other, the chain ending at its last, both FATs alike
  {
    const uint32_t cb = bs.spc * SS;
    uint32_t next = 2U;

    for (size_t i = 1U; i < n; i++) {
      const uint32_t clusters = (ents[i].size + cb - 1U) / cb;
      uint32_t c = ents[i].cluster;

      cl_assert_equal_i(ents[i].size, s_files[i - 1U].size);
      if (clusters == 0U) {
        cl_assert_equal_i(c, 0);
        continue;
      }
      cl_assert_equal_i(c, next);
      for (uint32_t k = 0U; k < clusters; k++) {
        const uint16_t v = prv_fat(&bs, 0, c);

        cl_assert_equal_i(v, prv_fat(&bs, 1, c));
        cl_assert_equal_i(v, k + 1U == clusters ? 0xFFFF : c + 1U);
        c = v;
      }
      next += clusters;
    }
    // Free from there
    cl_assert_equal_i(prv_fat(&bs, 0, next), 0);
    cl_assert_equal_i(prv_fat(&bs, 0, bs.clusters + 1U), 0);
  }
}

static size_t s_found;

static int prv_check_file(struct fat_ro *fs, struct fat_ro_file *file, void *arg) {
  static uint8_t s_buf[100000];
  size_t i = s_found++;

  cl_assert(i < s_vol.num_files);
  cl_assert_equal_i(file->size, s_files[i].size);
  if (file->size == 0U) {
    return 0;
  }
  cl_must_pass(fat_ro_read(fs, file, 0, s_buf, file->size));
  for (uint32_t k = 0U; k < file->size; k++) {
    if (s_buf[k] != prv_content(i, k)) {
      cl_fail("content");
    }
  }
  // And in pieces that straddle the sectors and the clusters
  if (file->size > 5000U) {
    cl_must_pass(fat_ro_read(fs, file, 4000U, s_buf, 1000U));
    for (uint32_t k = 0U; k < 1000U; k++) {
      cl_assert_equal_i(s_buf[k], prv_content(i, 4000U + k));
    }
  }

  return 0;
}

void test_vfat__read_back_by_a_fat_reader(void) {
  struct fat_ro fs;

  prv_file(0, "2026-11-28-1045-run.fit", 95123U);
  prv_file(1, "x.fit", 0U);
  prv_file(2, "2026-11-28-1210-walk.fit", 4096U);
  prv_file(3, "2026-11-29-0700-bike.fit", 513U);
  cl_assert_equal_i(prv_layout(4), 4);

  cl_must_pass(fat_ro_mount(&fs, prv_fat_ro_read, NULL, vfat_num_sectors(&s_vol)));
  cl_assert_equal_i(fs.type, FAT_RO_FAT16);
  s_found = 0U;
  cl_must_pass(fat_ro_foreach_root_file(&fs, prv_check_file, NULL));
  cl_assert_equal_i(s_found, 4);
}

void test_vfat__data_sectors(void) {
  static uint8_t one[64 * SS];
  static uint8_t many[64 * SS];
  struct boot bs;
  uint32_t lba;

  prv_file(0, "a.fit", 3U * SS + 10U);
  prv_file(1, "b.fit", 40000U);
  cl_assert_equal_i(prv_layout(2), 2);
  bs = prv_boot();

  // A file's data in one call of the callback, what is past its end in the cluster zeros
  s_reads = 0U;
  cl_must_pass(vfat_read(&s_vol, bs.data, many, 8));
  cl_assert_equal_i(s_reads, 1);
  cl_assert_equal_i(s_read_bytes, 3U * SS + 10U);
  for (uint32_t k = 0U; k < 8U * SS; k++) {
    cl_assert_equal_i(many[k], k < 3U * SS + 10U ? prv_content(0, k) : 0);
  }
  cl_assert_equal_i(vfat_file_at(&s_vol, bs.data), 0);
  cl_assert_equal_i(vfat_file_at(&s_vol, bs.data + 7U), 0);
  cl_assert_equal_i(vfat_file_at(&s_vol, bs.data + 8U), 1);
  cl_assert_equal_i(vfat_file_at(&s_vol, bs.data + 8U + 80U), -1);
  cl_assert_equal_i(vfat_file_at(&s_vol, bs.root), -1);

  // Any range reads as its sectors one by one, across the regions and the files
  for (lba = 0U; lba < bs.data + 40U; lba += 37U) {
    cl_must_pass(vfat_read(&s_vol, lba, many, 64));
    for (uint32_t k = 0U; k < 64U; k++) {
      cl_must_pass(vfat_read(&s_vol, lba + k, &one[k * SS], 1));
    }
    cl_assert(memcmp(one, many, sizeof(one)) == 0);
  }

  // What the callback returns
  s_vol.p.read = prv_failing;
  cl_assert_equal_i(vfat_read(&s_vol, bs.data, many, 1), -EIO);
  cl_must_pass(vfat_read(&s_vol, bs.root, many, 1));
}

void test_vfat__what_does_not_fit_is_left_out(void) {
  struct vfat_params p = {.read = prv_data, .total_sectors = 4085U * 8U + 512U};
  char name[40];

  // 39 files of 3 entries take 117 of the root's 512: all fit by entries; of 1 MiB each, 16 fit
  // the 4140 clusters of the volume
  for (size_t i = 0U; i < 39U; i++) {
    snprintf(name, sizeof(name), "2026-11-%02u-1045-run.fit", (unsigned int)(i % 28U + 1U));
    prv_file(i, name, 1024U * 1024U);
  }
  cl_assert_equal_i(vfat_layout(&s_vol, &p, s_files, 39), 16);
  cl_assert_equal_i(s_vol.num_clusters, 4140);

  // Long names, by entries: 63 characters take 5 long name entries and the alias each, so 85
  // fit the 511 entries after the label
  memset(s_files, 0, sizeof(s_files));
  for (size_t i = 0U; i < 90U; i++) {
    memset(s_files[i].name, 'x', VFAT_NAME_MAX);
    s_files[i].name[0] = (char)('A' + i % 26U);
    s_files[i].name[1] = (char)('a' + i / 26U);
    s_files[i].size = 100U;
  }
  cl_assert_equal_i(prv_layout(90), 85);
  cl_assert_equal_i(s_vol.num_entries, 1U + 85U * 6U);
}

void test_vfat__aliases_stay_unique(void) {
  struct dirent ents[40];
  struct boot bs;
  size_t n;

  for (size_t i = 0U; i < 12U; i++) {
    char name[40];

    snprintf(name, sizeof(name), "2026-11-28-10%02u-run.fit", (unsigned int)i);
    prv_file(i, name, 10U);
  }
  cl_assert_equal_i(prv_layout(12), 12);
  bs = prv_boot();
  n = prv_readdir(&bs, ents, 40);
  cl_assert_equal_i(n, 13);
  cl_assert(memcmp(ents[9].sfn, "2026-1~9FIT", 11) == 0);
  cl_assert(memcmp(ents[10].sfn, "2026-~10FIT", 11) == 0);
  for (size_t i = 1U; i < n; i++) {
    for (size_t j = i + 1U; j < n; j++) {
      cl_assert(memcmp(ents[i].sfn, ents[j].sfn, 11) != 0);
    }
  }
}

void test_vfat__bad_names(void) {
  prv_file(0, "a/b.fit", 1U);
  cl_assert_equal_i(prv_layout(1), -EINVAL);
  prv_file(0, "", 1U);
  cl_assert_equal_i(prv_layout(1), -EINVAL);
  prv_file(0, "trailing.", 1U);
  cl_assert_equal_i(prv_layout(1), -EINVAL);
  prv_file(0, "what?.fit", 1U);
  cl_assert_equal_i(prv_layout(1), -EINVAL);
  prv_file(0, "fine name.fit", 1U);
  cl_assert_equal_i(prv_layout(1), 1);
}

void test_vfat__dates(void) {
  cl_assert_equal_i(vfat_date(1980, 1, 1), 0x21);
  cl_assert_equal_i(vfat_date(2026, 11, 28), ((2026 - 1980) << 9) | (11 << 5) | 28);
  cl_assert_equal_i(vfat_date(1970, 1, 1), 0x21);
  cl_assert_equal_i(vfat_time(23, 59, 59), (23 << 11) | (59 << 5) | 29);
}

// A volume like the watch's, written out for fsck.fat, 7z or a loop mount when VFAT_IMAGE is set
void test_vfat__image(void) {
  const char *path = getenv("VFAT_IMAGE");
  static uint8_t buf[256 * SS];
  FILE *f;

  prv_file(0, "2026-11-28-1045-run.fit", 95123U);
  prv_file(1, "2026-11-28-1210-walk.fit", 4096U);
  prv_file(2, "2026-11-29-0700-bike.fit", 513U);
  prv_file(3, "2026-11-29-0700-bike-2.fit", 70000U);
  cl_assert_equal_i(prv_layout(4), 4);
  if (path == NULL) {
    return;
  }

  f = fopen(path, "wb");
  cl_assert(f != NULL);
  for (uint32_t lba = 0U; lba < vfat_num_sectors(&s_vol); lba += 256U) {
    cl_must_pass(vfat_read(&s_vol, lba, buf, 256U));
    cl_assert_equal_i(fwrite(buf, SS, 256U, f), 256U);
  }
  fclose(f);
}
