/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <string.h>

#include <pbl/fat_ro/fat_ro.h>

#include <clar.h>

// Volumes as a computer or the watch's own firmware writes them, built sector by sector: exFAT,
// as UNA formats its update drive, alone or in an MBR partition, and FAT32 and FAT16, as other
// computers may. The device reads as zeros past what is kept of it.

#define SS            FAT_RO_SECTOR_SIZE
#define STORE_SECTORS 4096U
#define DEV_SECTORS   204800U

static uint8_t s_store[STORE_SECTORS * SS];
static unsigned int s_reads;
static struct fat_ro s_fs;

static int prv_read(void *ctx, uint32_t sector, void *buf, uint32_t count) {
  uint8_t *dst = buf;

  cl_assert(sector + count <= DEV_SECTORS);
  for (uint32_t i = 0U; i < count; i++) {
    if (sector + i < STORE_SECTORS) {
      memcpy(&dst[i * SS], &s_store[(sector + i) * SS], SS);
    } else {
      memset(&dst[i * SS], 0, SS);
    }
  }
  s_reads++;

  return 0;
}

static uint8_t *prv_sector(uint32_t sector) {
  cl_assert(sector < STORE_SECTORS);
  return &s_store[sector * SS];
}

static void prv_put16(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void prv_put32(uint8_t *p, uint32_t v) {
  prv_put16(p, v);
  prv_put16(&p[2], v >> 16);
}

static uint8_t prv_content(uint32_t file, uint32_t offset) {
  return (uint8_t)(file * 37U + offset * 7U + (offset >> 9));
}

// --- exFAT: 4 KiB clusters, FAT at 24, heap at 32, root at cluster 4

#define EX_SPC     8U
#define EX_FAT     24U
#define EX_HEAP    32U
#define EX_SECTORS 4000U

static uint32_t s_ex_base;
static uint32_t s_ex_dir;

static uint8_t *prv_ex_cluster(uint32_t cluster) {
  return prv_sector(s_ex_base + EX_HEAP + (cluster - 2U) * EX_SPC);
}

static void prv_ex_fat(uint32_t cluster, uint32_t next) {
  prv_put32(&prv_sector(s_ex_base + EX_FAT)[cluster * 4U], next);
}

static uint8_t *prv_ex_entry(void) {
  uint8_t *e = &prv_ex_cluster(4U)[s_ex_dir * 32U];

  s_ex_dir++;
  return e;
}

static void prv_ex_format(uint32_t base) {
  uint8_t *vbr;

  s_ex_base = base;
  s_ex_dir = 0U;
  vbr = prv_sector(base);
  vbr[0] = 0xEB;
  vbr[1] = 0x76;
  vbr[2] = 0x90;
  memcpy(&vbr[3], "EXFAT   ", 8);
  prv_put32(&vbr[64], base);
  prv_put32(&vbr[72], EX_SECTORS);
  prv_put32(&vbr[80], EX_FAT);
  prv_put32(&vbr[84], 8U);
  prv_put32(&vbr[88], EX_HEAP);
  prv_put32(&vbr[92], (EX_SECTORS - EX_HEAP) / EX_SPC);
  prv_put32(&vbr[96], 4U);
  vbr[108] = 9;
  vbr[109] = 3;
  vbr[110] = 1;
  vbr[510] = 0x55;
  vbr[511] = 0xAA;

  prv_ex_fat(0U, 0xFFFFFFF8U);
  prv_ex_fat(1U, 0xFFFFFFFFU);
  prv_ex_fat(2U, 0xFFFFFFFFU);
  prv_ex_fat(3U, 0xFFFFFFFFU);
  prv_ex_fat(4U, 0xFFFFFFFFU);

  // Label, allocation bitmap and up-case table, as mkfs writes them
  prv_ex_entry()[0] = 0x83;
  uint8_t *bitmap = prv_ex_entry();
  bitmap[0] = 0x81;
  prv_put32(&bitmap[20], 2U);
  prv_put32(&bitmap[24], 64U);
  uint8_t *upcase = prv_ex_entry();
  upcase[0] = 0x82;
  prv_put32(&upcase[20], 3U);
  prv_put32(&upcase[24], 5836U);
}

// A file set: file, stream extension and @p names name entries
static void prv_ex_file(uint32_t attr, uint32_t first, uint32_t size, bool contiguous,
                        uint32_t names, bool in_use) {
  uint8_t *file = prv_ex_entry();
  uint8_t *stream = prv_ex_entry();

  file[0] = in_use ? 0x85 : 0x05;
  file[1] = (uint8_t)(1U + names);
  prv_put16(&file[4], attr);
  stream[0] = in_use ? 0xC0 : 0x40;
  stream[1] = (uint8_t)(0x01U | (contiguous ? 0x02U : 0x00U));
  prv_put32(&stream[8], size);
  prv_put32(&stream[20], first);
  prv_put32(&stream[24], size);
  for (uint32_t i = 0U; i < names; i++) {
    uint8_t *name = prv_ex_entry();

    name[0] = in_use ? 0xC1 : 0x41;
    memcpy(&name[2], "x\0y\0z\0", 6);
  }
}

static void prv_fill(uint8_t *dst, uint32_t file, uint32_t offset, uint32_t len) {
  for (uint32_t i = 0U; i < len; i++) {
    dst[i] = prv_content(file, offset + i);
  }
}

struct found {
  struct fat_ro_file files[8];
  unsigned int count;
  unsigned int stop_at;
};

static int prv_collect(struct fat_ro *fs, struct fat_ro_file *file, void *arg) {
  struct found *found = arg;

  cl_assert(found->count < 8U);
  found->files[found->count++] = *file;

  return found->count == found->stop_at ? 7 : 0;
}

static void prv_check_file(struct fat_ro_file *file, uint32_t id) {
  static uint8_t s_buf[64 * 1024];

  cl_assert(file->size <= sizeof(s_buf));
  memset(s_buf, 0, sizeof(s_buf));
  cl_assert_equal_i(fat_ro_read(&s_fs, file, 0U, s_buf, file->size), 0);
  for (uint32_t i = 0U; i < file->size; i++) {
    cl_assert_equal_i(s_buf[i], prv_content(id, i));
  }

  // Pieces, out of order and across sectors and clusters
  for (uint32_t offset = 0U; offset < file->size; offset += 1013U) {
    const uint32_t len = (file->size - offset) < 700U ? file->size - offset : 700U;

    cl_assert_equal_i(fat_ro_read(&s_fs, file, offset, s_buf, len), 0);
    for (uint32_t i = 0U; i < len; i++) {
      cl_assert_equal_i(s_buf[i], prv_content(id, offset + i));
    }
  }
  cl_assert_equal_i(fat_ro_read(&s_fs, file, 3U, s_buf, 5U), 0);
  cl_assert_equal_i(s_buf[0], prv_content(id, 3U));

  cl_assert_equal_i(fat_ro_read(&s_fs, file, file->size, s_buf, 1U), -EINVAL);
}

static void prv_ex_volume(uint32_t base) {
  prv_ex_format(base);

  // A directory, a deleted file, then two files: one contiguous, one on a chain of clusters
  prv_ex_file(0x10, 5U, 4096U, true, 1U, true);
  prv_ex_file(0x20, 6U, 1000U, true, 1U, false);
  prv_ex_file(0x20, 10U, 10000U, true, 2U, true);
  prv_ex_file(0x20, 20U, 9000U, false, 1U, true);

  for (uint32_t c = 0U; c < 3U; c++) {
    prv_fill(prv_ex_cluster(10U + c), 1U, c * 4096U, 4096U);
  }
  // 20 -> 25 -> 22
  prv_fill(prv_ex_cluster(20U), 2U, 0U, 4096U);
  prv_fill(prv_ex_cluster(25U), 2U, 4096U, 4096U);
  prv_fill(prv_ex_cluster(22U), 2U, 8192U, 4096U);
  prv_ex_fat(20U, 25U);
  prv_ex_fat(25U, 22U);
  prv_ex_fat(22U, 0xFFFFFFFFU);
}

static void prv_check_ex_files(void) {
  struct found found = {0};

  cl_assert_equal_i(fat_ro_foreach_root_file(&s_fs, prv_collect, &found), 0);
  cl_assert_equal_i(found.count, 2);
  cl_assert_equal_i(found.files[0].size, 10000);
  cl_assert(found.files[0].contiguous);
  cl_assert_equal_i(found.files[1].size, 9000);
  cl_assert(!found.files[1].contiguous);
  prv_check_file(&found.files[0], 1U);
  prv_check_file(&found.files[1], 2U);
}

void test_fat_ro__initialize(void) {
  memset(s_store, 0, sizeof(s_store));
  memset(&s_fs, 0, sizeof(s_fs));
  s_reads = 0U;
}

void test_fat_ro__exfat_on_the_whole_device(void) {
  prv_ex_volume(0U);

  cl_assert_equal_i(fat_ro_mount(&s_fs, prv_read, nullptr, DEV_SECTORS), 0);
  cl_assert_equal_i(s_fs.type, FAT_RO_EXFAT);
  prv_check_ex_files();
}

void test_fat_ro__exfat_in_an_mbr_partition(void) {
  uint8_t *mbr = prv_sector(0U);

  // The first entry left empty, as some tools do
  mbr[446 + 16 + 4] = 0x07;
  prv_put32(&mbr[446 + 16 + 8], 128U);
  prv_put32(&mbr[446 + 16 + 12], EX_SECTORS);
  mbr[510] = 0x55;
  mbr[511] = 0xAA;
  prv_ex_volume(128U);

  cl_assert_equal_i(fat_ro_mount(&s_fs, prv_read, nullptr, DEV_SECTORS), 0);
  prv_check_ex_files();
}

void test_fat_ro__the_callback_stops_the_walk(void) {
  struct found found = {.stop_at = 1U};

  prv_ex_volume(0U);
  cl_assert_equal_i(fat_ro_mount(&s_fs, prv_read, nullptr, DEV_SECTORS), 0);
  cl_assert_equal_i(fat_ro_foreach_root_file(&s_fs, prv_collect, &found), 7);
  cl_assert_equal_i(found.count, 1);
}

void test_fat_ro__a_volume_past_the_device_is_refused(void) {
  prv_ex_volume(0U);
  cl_assert_equal_i(fat_ro_mount(&s_fs, prv_read, nullptr, EX_SECTORS - 1U), -ENOENT);
}

void test_fat_ro__no_volume(void) {
  cl_assert_equal_i(fat_ro_mount(&s_fs, prv_read, nullptr, DEV_SECTORS), -ENOENT);

  // A signature, but neither a boot sector nor a partition
  prv_sector(0U)[510] = 0x55;
  prv_sector(0U)[511] = 0xAA;
  cl_assert_equal_i(fat_ro_mount(&s_fs, prv_read, nullptr, DEV_SECTORS), -ENOENT);
}

// --- FAT32: one sector a cluster, 67000 sectors, two FATs of 520 sectors

#define F32_RSVD    32U
#define F32_FATSZ   520U
#define F32_DATA    (F32_RSVD + 2U * F32_FATSZ)
#define F32_SECTORS 67000U

static uint8_t *prv_f32_cluster(uint32_t cluster) {
  return prv_sector(F32_DATA + cluster - 2U);
}

static void prv_f32_fat(uint32_t cluster, uint32_t next) {
  // Both copies, though only the first is read
  for (uint32_t i = 0U; i < 2U; i++) {
    prv_put32(&prv_sector(F32_RSVD + i * F32_FATSZ + cluster / 128U)[(cluster % 128U) * 4U], next);
  }
}

static void prv_fat_bpb(uint8_t *vbr, uint32_t spc, uint32_t rsvd, uint32_t root_entries,
                        uint32_t total16, uint32_t fatsz16) {
  vbr[0] = 0xEB;
  vbr[1] = 0x58;
  vbr[2] = 0x90;
  memcpy(&vbr[3], "MSDOS5.0", 8);
  prv_put16(&vbr[11], SS);
  vbr[13] = (uint8_t)spc;
  prv_put16(&vbr[14], rsvd);
  vbr[16] = 2;
  prv_put16(&vbr[17], root_entries);
  prv_put16(&vbr[19], total16);
  vbr[21] = 0xF8;
  prv_put16(&vbr[22], fatsz16);
  vbr[510] = 0x55;
  vbr[511] = 0xAA;
}

static void prv_fat_dirent(uint8_t *e, const char *name, uint8_t attr, uint32_t first,
                           uint32_t size) {
  memcpy(e, name, 11);
  e[11] = attr;
  prv_put16(&e[20], first >> 16);
  prv_put16(&e[26], first & 0xFFFFU);
  prv_put32(&e[28], size);
}

static void prv_f32_format(void) {
  uint8_t *vbr = prv_sector(0U);

  prv_fat_bpb(vbr, 1U, F32_RSVD, 0U, 0U, 0U);
  prv_put32(&vbr[32], F32_SECTORS);
  prv_put32(&vbr[36], F32_FATSZ);
  prv_put32(&vbr[44], 2U);
  prv_f32_fat(0U, 0x0FFFFFF8U);
  prv_f32_fat(1U, 0x0FFFFFFFU);
  prv_f32_fat(2U, 0x0FFFFFFFU);
}

struct counted {
  unsigned int files;
  uint32_t last_size;
};

static int prv_count(struct fat_ro *fs, struct fat_ro_file *file, void *arg) {
  struct counted *counted = arg;

  counted->files++;
  counted->last_size = file->size;

  return 0;
}

void test_fat_ro__fat32_root_over_two_clusters(void) {
  uint8_t *root[2];
  struct counted counted = {0};

  prv_f32_format();
  // 16 entries in each cluster of the directory
  prv_f32_fat(2U, 3U);
  prv_f32_fat(3U, 0x0FFFFFFFU);
  root[0] = prv_f32_cluster(2U);
  root[1] = prv_f32_cluster(3U);

  // A label, a long name entry, a deleted file and a directory, left out, then 12 empty files
  prv_fat_dirent(&root[0][0], "UNA UPD    ", 0x08, 0U, 0U);
  prv_fat_dirent(&root[0][32], "Aunapebble ", 0x0F, 0U, 0U);
  prv_fat_dirent(&root[0][64], "\xE5OLD    OTA", 0x20, 30U, 100U);
  prv_fat_dirent(&root[0][96], "SUBDIR     ", 0x10, 31U, 0U);
  for (uint32_t i = 4U; i < 16U; i++) {
    prv_fat_dirent(&root[0][i * 32U], "EMPTY   TXT", 0x20, 0U, 0U);
  }
  // Then a file in the second cluster, and the end
  prv_fat_dirent(&root[1][0], "UNAPEB~1PBP", 0x20, 5U, 300U);

  cl_assert_equal_i(fat_ro_mount(&s_fs, prv_read, nullptr, DEV_SECTORS), 0);
  cl_assert_equal_i(s_fs.type, FAT_RO_FAT32);
  cl_assert_equal_i(fat_ro_foreach_root_file(&s_fs, prv_count, &counted), 0);
  cl_assert_equal_i(counted.files, 13);
  cl_assert_equal_i(counted.last_size, 300);
}

void test_fat_ro__fat32_reads_a_fragmented_file(void) {
  const uint32_t chain[] = {5U, 9U, 6U, 20U, 7U};

  prv_f32_format();
  prv_fat_dirent(prv_f32_cluster(2U), "UNAPEB~1PBP", 0x20, chain[0], 2300U);

  for (uint32_t i = 0U; i < 5U; i++) {
    prv_fill(prv_f32_cluster(chain[i]), 3U, i * SS, SS);
    prv_f32_fat(chain[i], i + 1U < 5U ? chain[i + 1U] : 0x0FFFFFFFU);
  }

  struct found found = {0};
  cl_assert_equal_i(fat_ro_mount(&s_fs, prv_read, nullptr, DEV_SECTORS), 0);
  cl_assert_equal_i(fat_ro_foreach_root_file(&s_fs, prv_collect, &found), 0);
  cl_assert_equal_i(found.count, 1);
  cl_assert_equal_i(found.files[0].size, 2300);
  prv_check_file(&found.files[0], 3U);
}

void test_fat_ro__a_broken_chain_fails_the_read(void) {
  uint8_t buf[SS];

  prv_f32_format();
  prv_fat_dirent(prv_f32_cluster(2U), "BROKEN  BIN", 0x20, 5U, 2000U);
  // Free after the first cluster
  prv_f32_fat(5U, 0U);

  struct found found = {0};
  cl_assert_equal_i(fat_ro_mount(&s_fs, prv_read, nullptr, DEV_SECTORS), 0);
  cl_assert_equal_i(fat_ro_foreach_root_file(&s_fs, prv_collect, &found), 0);
  cl_assert_equal_i(found.count, 1);
  cl_assert_equal_i(fat_ro_read(&s_fs, &found.files[0], 0U, buf, SS), 0);
  cl_assert_equal_i(fat_ro_read(&s_fs, &found.files[0], SS, buf, SS), -EIO);
}

// --- FAT16: 4 sectors a cluster, a fixed root directory of 512 entries

void test_fat_ro__fat16(void) {
  uint8_t *vbr = prv_sector(0U);
  const uint32_t rsvd = 1U;
  const uint32_t fatsz = 20U;
  const uint32_t root = rsvd + 2U * fatsz;
  const uint32_t data = root + 32U;

  prv_fat_bpb(vbr, 4U, rsvd, 512U, 20000U, fatsz);
  // Cluster 3 then 2
  prv_put16(&prv_sector(rsvd)[3U * 2U], 2U);
  prv_put16(&prv_sector(rsvd)[2U * 2U], 0xFFFFU);
  prv_fat_dirent(prv_sector(root), "PACK    BIN", 0x20, 3U, 3000U);
  prv_fill(prv_sector(data + (3U - 2U) * 4U), 4U, 0U, 2048U);
  prv_fill(prv_sector(data + (2U - 2U) * 4U), 4U, 2048U, 2048U);

  struct found found = {0};
  cl_assert_equal_i(fat_ro_mount(&s_fs, prv_read, nullptr, DEV_SECTORS), 0);
  cl_assert_equal_i(s_fs.type, FAT_RO_FAT16);
  cl_assert_equal_i(fat_ro_foreach_root_file(&s_fs, prv_collect, &found), 0);
  cl_assert_equal_i(found.count, 1);
  prv_check_file(&found.files[0], 4U);
}

void test_fat_ro__fat12_is_not_supported(void) {
  prv_fat_bpb(prv_sector(0U), 1U, 1U, 224U, 2880U, 9U);
  cl_assert_equal_i(fat_ro_mount(&s_fs, prv_read, nullptr, DEV_SECTORS), -ENOTSUP);
}
