/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * NOR semantics on a flash whose words carry ECC, as the embedded flash of the STM32U5: a word
 * of ECC_NOR_WORD_SIZE bytes is programmed once after its page is erased, while the flash class
 * and its users (PFS, the flash log, the coredump) program bytes, and clear bits of bytes they
 * programmed before.
 *
 * Writes go to a few pages cached in RAM, holding what the page reads as, and reach the flash
 * later, word by word:
 *
 * - a word every byte of which was written, ECC_NOR_SETTLE_MS after it was first changed;
 * - a word only some bytes of which were written, as the tail of a record that the next one
 *   continues, ECC_NOR_PARTIAL_MS after it was first changed, or as soon as its page leaves the
 *   cache, or on ecc_nor_flush() with @c all;
 * - a word already programmed whose bits are cleared again, by erasing its page and programming
 *   the page again, the words that were in the flash before at once.
 *
 * Programming a word only once it is whole keeps appends, which most writes are, from wearing
 * the flash, at the cost of the last ECC_NOR_SETTLE_MS of writes, and of the partial word that
 * ends them, on a reset that does not go through ecc_nor_flush(). Erasing part of a page, as a 4
 * KiB subsector of an 8 KiB page, erases the page and programs the rest of it again.
 *
 * Not thread safe: the flash class serializes calls.
 */

#define ECC_NOR_PAGE_SIZE  8192U
#define ECC_NOR_WORD_SIZE  16U
#define ECC_NOR_PAGE_WORDS (ECC_NOR_PAGE_SIZE / ECC_NOR_WORD_SIZE)
#define ECC_NOR_SLOTS      4U

// How long a whole word stays in RAM after its first change
#define ECC_NOR_SETTLE_MS 1000U
// How long a partial word stays in RAM after its first change, for the next append to fill it
#define ECC_NOR_PARTIAL_MS (10U * 60U * 1000U)

#define ECC_NOR_NO_PAGE UINT32_MAX

/** Hardware operations, on offsets in the flash. */
struct ecc_nor_hw {
  /** Program the erased word at @p addr. Returns 0 or a negative errno. */
  int (*program)(void *ctx, uint32_t addr, const uint8_t data[ECC_NOR_WORD_SIZE]);
  /** Erase the page at @p addr. Returns 0 or a negative errno. */
  int (*erase_page)(void *ctx, uint32_t addr);
  /** Read @p len bytes at @p addr from the flash itself. */
  void (*read)(void *ctx, uint32_t addr, void *buf, size_t len);
  /** Optional. Called after programming or erasing, before the flash is read again. */
  void (*sync)(void *ctx);
};

struct ecc_nor_slot {
  uint32_t page;
  uint32_t last_use;
  // Words that differ from the flash, and when each first did
  uint32_t pending[ECC_NOR_PAGE_WORDS / 32U];
  uint32_t since_ms[ECC_NOR_PAGE_WORDS];
  // Bytes written while their word was pending
  uint8_t written[ECC_NOR_PAGE_SIZE / 8U];
  uint8_t data[ECC_NOR_PAGE_SIZE] __attribute__((aligned(4)));
};

struct ecc_nor {
  const struct ecc_nor_hw *hw;
  void *ctx;
  uint32_t clock;
  // Program every change at once, without the cache delaying it
  bool write_through;
  // The flash changed since the last sync
  bool changed;
  struct ecc_nor_slot slots[ECC_NOR_SLOTS];
};

/** Start with an empty cache, on hardware @p hw called with @p ctx. */
void ecc_nor_init(struct ecc_nor *nor, const struct ecc_nor_hw *hw, void *ctx);

/** Read what the flash reads as, pending writes included. */
void ecc_nor_read(struct ecc_nor *nor, uint32_t addr, void *buf, size_t len);

/** Clear the bits of @p buf that are clear at @p addr. Returns 0 or a negative errno. */
int ecc_nor_write(struct ecc_nor *nor, uint32_t addr, const void *buf, size_t len,
                  uint32_t now_ms);

/**
 * Erase @p len bytes at @p addr, both multiples of ECC_NOR_WORD_SIZE. Returns 0 or a negative
 * errno.
 */
int ecc_nor_erase(struct ecc_nor *nor, uint32_t addr, size_t len, uint32_t now_ms);

/**
 * Program the pending words that are due at @p now_ms, or all of them with @p all. Returns 0 or
 * a negative errno.
 */
int ecc_nor_flush(struct ecc_nor *nor, uint32_t now_ms, bool all);

/**
 * Get how long after @p now_ms the next pending word is due, 0 if one is, or UINT32_MAX without
 * any pending.
 */
uint32_t ecc_nor_next_due_ms(const struct ecc_nor *nor, uint32_t now_ms);

/** Program every pending word, then program every later change at once. */
int ecc_nor_set_write_through(struct ecc_nor *nor, uint32_t now_ms);
