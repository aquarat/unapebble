/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * NOR semantics on a block device that rewrites whole blocks, as an eMMC, which levels its wear
 * itself: the flash class and its users (PFS, the flash log, the coredump) program bytes,
 * clearing bits, read at any offset, and erase subsectors to 0xFF.
 *
 * A write reads the blocks it touches, ANDs the data in and writes back the blocks that change,
 * before it returns: nothing is held back in RAM, so writes reach the device in the order they
 * were made. An erase writes blocks of 0xFF. Runs of whole blocks go to and from the device a
 * batch at a time.
 *
 * The last blocks read or written in part are cached, holding what the device holds, for the
 * small reads and appends that most accesses are.
 *
 * Not thread safe: the flash class serializes calls.
 */

#define BLK_NOR_BLOCK_SIZE 512U
// Blocks cached for partial accesses
#define BLK_NOR_CACHE_BLOCKS 16U
// Blocks moved by one device access in runs of whole blocks
#define BLK_NOR_BATCH_BLOCKS 8U

#define BLK_NOR_NO_BLOCK UINT32_MAX

/** Device operations, on block numbers from the start of the flash. */
struct blk_nor_hw {
  /** Read @p count blocks from @p block. Returns 0 or a negative errno. */
  int (*read)(void *ctx, uint32_t block, void *buf, uint32_t count);
  /** Write @p count blocks at @p block. Returns 0 or a negative errno. */
  int (*write)(void *ctx, uint32_t block, const void *buf, uint32_t count);
};

struct blk_nor_entry {
  uint32_t block;
  uint32_t last_use;
};

struct blk_nor {
  const struct blk_nor_hw *hw;
  void *ctx;
  uint32_t clock;
  struct blk_nor_entry entries[BLK_NOR_CACHE_BLOCKS];
  uint8_t cache[BLK_NOR_CACHE_BLOCKS][BLK_NOR_BLOCK_SIZE] __attribute__((aligned(4)));
  // A batch being read, changed and written back; then blocks of 0xFF for erases
  uint8_t batch[BLK_NOR_BATCH_BLOCKS * BLK_NOR_BLOCK_SIZE] __attribute__((aligned(4)));
  uint8_t erased[BLK_NOR_BATCH_BLOCKS * BLK_NOR_BLOCK_SIZE] __attribute__((aligned(4)));
};

/** Start with an empty cache, on device @p hw called with @p ctx. */
void blk_nor_init(struct blk_nor *nor, const struct blk_nor_hw *hw, void *ctx);

/** Forget what is cached, for a device that may have changed behind the cache. */
void blk_nor_invalidate(struct blk_nor *nor);

/** Read @p len bytes at @p addr. Returns 0 or a negative errno. */
int blk_nor_read(struct blk_nor *nor, uint32_t addr, void *buf, size_t len);

/** Clear the bits of @p buf that are clear at @p addr. Returns 0 or a negative errno. */
int blk_nor_write(struct blk_nor *nor, uint32_t addr, const void *buf, size_t len);

/**
 * Erase @p len bytes at @p addr to 0xFF, both multiples of BLK_NOR_BLOCK_SIZE. Returns 0 or a
 * negative errno.
 */
int blk_nor_erase(struct blk_nor *nor, uint32_t addr, size_t len);
