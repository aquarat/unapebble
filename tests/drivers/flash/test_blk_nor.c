/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "blk_nor.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <clar.h>

// A block device as an eMMC: 512-byte blocks rewritten whole. Next to it, the NOR flash the class
// expects, bytes ANDed by writes and 4 KiB subsectors erased, which the device must hold after
// every call: nothing is held back.

#define BS         BLK_NOR_BLOCK_SIZE
#define NUM_BLOCKS 256U
#define DEV_SIZE   (NUM_BLOCKS * BS)
#define SUBSECTOR  4096U

static uint8_t s_dev[DEV_SIZE];
static uint8_t s_ref[DEV_SIZE];
static unsigned int s_reads;
static unsigned int s_writes;
static unsigned int s_blocks_written;
// The write that fails, counted from 1, or 0
static unsigned int s_fail_write;
static struct blk_nor s_nor;

static int prv_hw_read(void *ctx, uint32_t block, void *buf, uint32_t count) {
  cl_assert(count > 0U);
  cl_assert(block + count <= NUM_BLOCKS);
  memcpy(buf, &s_dev[block * BS], count * BS);
  s_reads++;

  return 0;
}

static int prv_hw_write(void *ctx, uint32_t block, const void *buf, uint32_t count) {
  cl_assert(count > 0U && count <= BLK_NOR_BATCH_BLOCKS);
  cl_assert(block + count <= NUM_BLOCKS);
  s_writes++;
  if (s_writes == s_fail_write) {
    // Torn: the first block made it
    memcpy(&s_dev[block * BS], buf, BS);
    return -EIO;
  }
  memcpy(&s_dev[block * BS], buf, count * BS);
  s_blocks_written += count;

  return 0;
}

static const struct blk_nor_hw s_hw = {
  .read = prv_hw_read,
  .write = prv_hw_write,
};

static void prv_write(uint32_t addr, const void *buf, size_t len) {
  const uint8_t *src = buf;

  for (size_t i = 0U; i < len; i++) {
    s_ref[addr + i] &= src[i];
  }
  cl_assert_equal_i(blk_nor_write(&s_nor, addr, buf, len), 0);
  cl_assert(memcmp(s_dev, s_ref, sizeof(s_dev)) == 0);
}

static void prv_erase(uint32_t addr, size_t len) {
  memset(&s_ref[addr], 0xFF, len);
  cl_assert_equal_i(blk_nor_erase(&s_nor, addr, len), 0);
  cl_assert(memcmp(s_dev, s_ref, sizeof(s_dev)) == 0);
}

static void prv_check_reads(void) {
  static uint8_t s_buf[DEV_SIZE];

  // Many alignments and lengths, cached or not
  for (uint32_t addr = 0U; addr < DEV_SIZE; addr += 397U) {
    size_t len = 1U + (addr * 7U) % 3000U;

    if (len > DEV_SIZE - addr) {
      len = DEV_SIZE - addr;
    }

    memset(s_buf, 0, len);
    cl_assert_equal_i(blk_nor_read(&s_nor, addr, s_buf, len), 0);
    cl_assert(memcmp(s_buf, &s_ref[addr], len) == 0);
  }
  cl_assert_equal_i(blk_nor_read(&s_nor, 0U, s_buf, sizeof(s_buf)), 0);
  cl_assert(memcmp(s_buf, s_ref, sizeof(s_buf)) == 0);
}

void test_blk_nor__initialize(void) {
  memset(s_dev, 0xFF, sizeof(s_dev));
  memset(s_ref, 0xFF, sizeof(s_ref));
  s_reads = 0U;
  s_writes = 0U;
  s_blocks_written = 0U;
  s_fail_write = 0U;
  blk_nor_init(&s_nor, &s_hw, nullptr);
}

void test_blk_nor__writes_reach_the_device_before_returning(void) {
  const uint8_t data[5] = {1, 2, 3, 4, 5};

  prv_write(0x1003U, data, sizeof(data));
  cl_assert_equal_i(s_writes, 1);
  prv_check_reads();
}

void test_blk_nor__writes_only_clear_bits(void) {
  const uint8_t first[4] = {0xF0, 0x0F, 0xAA, 0x55};
  const uint8_t second[4] = {0xCC, 0xCC, 0xFF, 0x00};

  prv_write(0x200U, first, sizeof(first));
  prv_write(0x200U, second, sizeof(second));
  cl_assert_equal_i(s_dev[0x200U], 0xC0);
  cl_assert_equal_i(s_dev[0x201U], 0x0C);
  cl_assert_equal_i(s_dev[0x202U], 0xAA);
  cl_assert_equal_i(s_dev[0x203U], 0x00);
  prv_check_reads();
}

void test_blk_nor__unchanged_blocks_are_not_written(void) {
  uint8_t ones[3 * BS];

  memset(ones, 0xFF, sizeof(ones));
  prv_write(0x400U, ones, 7U);
  prv_write(0x800U, ones, sizeof(ones));
  cl_assert_equal_i(s_writes, 0);
}

// The flash log's pattern: a record header with its flags erased, the message, then the flags
// cleared in place: the block stays cached, read once
void test_blk_nor__appends_read_their_block_once(void) {
  uint32_t addr = 0x4000U;

  for (unsigned int i = 0U; i < 20U; i++) {
    const uint8_t hdr[2] = {0xFF, 12U};
    uint8_t msg[12];
    const uint8_t flags = 0xFE;
    const uint32_t start = addr;

    memset(msg, (int)i, sizeof(msg));
    prv_write(addr, hdr, sizeof(hdr));
    prv_write(addr + 2U, msg, sizeof(msg));
    prv_write(start, &flags, 1U);
    addr += 2U + sizeof(msg);
  }

  // 280 bytes in one block
  cl_assert_equal_i(s_reads, 1);
  cl_assert_equal_i(s_writes, 60);
  prv_check_reads();
}

void test_blk_nor__whole_blocks_go_in_batches(void) {
  uint8_t data[20 * BS];

  for (size_t i = 0U; i < sizeof(data); i++) {
    data[i] = (uint8_t)(i * 31U);
  }
  prv_write(0x2000U, data, sizeof(data));

  // 20 blocks, in batches of 8: 3 reads, 3 writes
  cl_assert_equal_i(s_reads, 3);
  cl_assert_equal_i(s_writes, 3);
  cl_assert_equal_i(s_blocks_written, 20);
  prv_check_reads();
}

void test_blk_nor__unaligned_runs_split_at_blocks(void) {
  uint8_t data[3 * BS];

  memset(data, 0x3C, sizeof(data));
  prv_write(0x6100U, data, sizeof(data));
  prv_check_reads();
}

void test_blk_nor__erase_writes_blank_blocks(void) {
  uint8_t data[2 * SUBSECTOR];

  memset(data, 0x00, sizeof(data));
  prv_write(0x8000U, data, sizeof(data));
  prv_check_reads();

  s_writes = 0U;
  prv_erase(0x8000U, SUBSECTOR);
  cl_assert_equal_i(s_writes, SUBSECTOR / (BLK_NOR_BATCH_BLOCKS * BS));
  prv_check_reads();

  // A sector of 64 KiB, unaligned to the batches of other erases
  prv_erase(0x0U, 0x10000U);
  prv_check_reads();
}

void test_blk_nor__erase_refuses_partial_blocks(void) {
  cl_assert_equal_i(blk_nor_erase(&s_nor, 0x100U, BS), -EINVAL);
  cl_assert_equal_i(blk_nor_erase(&s_nor, 0U, BS + 1U), -EINVAL);
  cl_assert_equal_i(s_writes, 0);
}

void test_blk_nor__cached_blocks_follow_erases(void) {
  const uint8_t data[2] = {0x12, 0x34};
  uint8_t buf[2];

  prv_write(0x9010U, data, sizeof(data));
  cl_assert_equal_i(blk_nor_read(&s_nor, 0x9010U, buf, sizeof(buf)), 0);
  prv_erase(0x9000U, SUBSECTOR);
  cl_assert_equal_i(blk_nor_read(&s_nor, 0x9010U, buf, sizeof(buf)), 0);
  cl_assert_equal_i(buf[0], 0xFF);
  cl_assert_equal_i(buf[1], 0xFF);
}

void test_blk_nor__a_failed_write_leaves_the_cache_as_the_device(void) {
  const uint8_t data[4] = {0x01, 0x02, 0x03, 0x04};
  uint8_t big[2 * BS];
  uint8_t buf[4];

  s_fail_write = 1U;
  memset(&s_ref[0xA000U], 0xFF, 4U);
  cl_assert_equal_i(blk_nor_write(&s_nor, 0xA000U, data, sizeof(data)), -EIO);

  // What the device holds, whatever the write left there
  cl_assert_equal_i(blk_nor_read(&s_nor, 0xA000U, buf, sizeof(buf)), 0);
  cl_assert(memcmp(buf, &s_dev[0xA000U], sizeof(buf)) == 0);

  // A torn batch too
  memset(big, 0x00, sizeof(big));
  s_writes = 0U;
  s_fail_write = 1U;
  cl_assert_equal_i(blk_nor_read(&s_nor, 0xB000U + BS, buf, sizeof(buf)), 0);
  cl_assert_equal_i(blk_nor_write(&s_nor, 0xB000U, big, sizeof(big)), -EIO);
  cl_assert_equal_i(blk_nor_read(&s_nor, 0xB000U + BS, buf, sizeof(buf)), 0);
  cl_assert(memcmp(buf, &s_dev[0xB000U + BS], sizeof(buf)) == 0);
}

void test_blk_nor__invalidate_rereads_the_device(void) {
  const uint8_t data[1] = {0x00};
  uint8_t buf[1];

  prv_write(0xC000U, data, sizeof(data));
  // Changed behind the cache
  s_dev[0xC001U] = 0x42;
  blk_nor_invalidate(&s_nor);
  cl_assert_equal_i(blk_nor_read(&s_nor, 0xC001U, buf, sizeof(buf)), 0);
  cl_assert_equal_i(buf[0], 0x42);
}

void test_blk_nor__random_operations_match_nor(void) {
  static uint8_t data[3 * SUBSECTOR];

  srand(1234);
  for (unsigned int i = 0U; i < 2000U; i++) {
    const unsigned int op = (unsigned int)rand() % 8U;

    if (op == 0U) {
      const uint32_t sub = (uint32_t)rand() % (DEV_SIZE / SUBSECTOR);
      const uint32_t count = 1U + (uint32_t)rand() % 2U;

      if ((sub + count) * SUBSECTOR <= DEV_SIZE) {
        prv_erase(sub * SUBSECTOR, count * SUBSECTOR);
      }
    } else {
      const uint32_t addr = (uint32_t)rand() % DEV_SIZE;
      size_t len = 1U + (size_t)rand() % (op == 1U ? sizeof(data) : 64U);

      if (len > DEV_SIZE - addr) {
        len = DEV_SIZE - addr;
      }
      for (size_t j = 0U; j < len; j++) {
        data[j] = (uint8_t)rand();
      }
      prv_write(addr, data, len);
    }

    if ((i % 200U) == 0U) {
      prv_check_reads();
    }
  }
  prv_check_reads();
}
