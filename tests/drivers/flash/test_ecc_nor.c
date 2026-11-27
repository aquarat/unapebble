/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include <clar.h>

#include <stdlib.h>
#include <string.h>

#include "ecc_nor.h"

// A flash as the STM32U5's: 8 KiB pages, words of 16 bytes each programmed once after an erase.
// Next to it, the NOR flash the class expects, bytes ANDed by writes and 4 KiB subsectors
// erased, which the flash must read as once everything is programmed.

#define FLASH_SIZE (8U * ECC_NOR_PAGE_SIZE)
#define NUM_WORDS  (FLASH_SIZE / ECC_NOR_WORD_SIZE)
#define NUM_PAGES  (FLASH_SIZE / ECC_NOR_PAGE_SIZE)
#define SUBSECTOR  4096U

static uint8_t s_flash[FLASH_SIZE];
static bool s_programmed[NUM_WORDS];
static unsigned int s_erases[NUM_PAGES];
static unsigned int s_programs;
static unsigned int s_syncs;
static uint8_t s_ref[FLASH_SIZE];
static struct ecc_nor s_nor;

static int prv_program(void *ctx, uint32_t addr, const uint8_t data[ECC_NOR_WORD_SIZE]) {
  cl_assert((addr % ECC_NOR_WORD_SIZE) == 0U);
  cl_assert(addr < FLASH_SIZE);
  // Programming a word twice is what the ECC forbids
  cl_assert(!s_programmed[addr / ECC_NOR_WORD_SIZE]);
  s_programmed[addr / ECC_NOR_WORD_SIZE] = true;
  memcpy(&s_flash[addr], data, ECC_NOR_WORD_SIZE);
  s_programs++;

  return 0;
}

static int prv_erase_page(void *ctx, uint32_t addr) {
  cl_assert((addr % ECC_NOR_PAGE_SIZE) == 0U);
  memset(&s_flash[addr], 0xFF, ECC_NOR_PAGE_SIZE);
  memset(&s_programmed[addr / ECC_NOR_WORD_SIZE], 0, ECC_NOR_PAGE_WORDS * sizeof(bool));
  s_erases[addr / ECC_NOR_PAGE_SIZE]++;

  return 0;
}

static void prv_read(void *ctx, uint32_t addr, void *buf, size_t len) {
  memcpy(buf, &s_flash[addr], len);
}

static void prv_sync(void *ctx) {
  s_syncs++;
}

static const struct ecc_nor_hw s_hw = {
  .program = prv_program,
  .erase_page = prv_erase_page,
  .read = prv_read,
  .sync = prv_sync,
};

static void prv_write(uint32_t addr, const void *buf, size_t len, uint32_t now) {
  const uint8_t *src = buf;

  for (size_t i = 0U; i < len; i++) {
    s_ref[addr + i] &= src[i];
  }
  cl_assert_equal_i(ecc_nor_write(&s_nor, addr, buf, len, now), 0);
}

static void prv_erase(uint32_t addr, size_t len, uint32_t now) {
  memset(&s_ref[addr], 0xFF, len);
  cl_assert_equal_i(ecc_nor_erase(&s_nor, addr, len, now), 0);
}

static void prv_check_reads(void) {
  static uint8_t s_buf[FLASH_SIZE];

  ecc_nor_read(&s_nor, 0U, s_buf, sizeof(s_buf));
  cl_assert(memcmp(s_buf, s_ref, sizeof(s_buf)) == 0);
}

static unsigned int prv_total_erases(void) {
  unsigned int n = 0U;

  for (size_t i = 0U; i < NUM_PAGES; i++) {
    n += s_erases[i];
  }

  return n;
}

void test_ecc_nor__initialize(void) {
  memset(s_flash, 0xFF, sizeof(s_flash));
  memset(s_ref, 0xFF, sizeof(s_ref));
  memset(s_programmed, 0, sizeof(s_programmed));
  memset(s_erases, 0, sizeof(s_erases));
  s_programs = 0U;
  s_syncs = 0U;
  ecc_nor_init(&s_nor, &s_hw, nullptr);
}

void test_ecc_nor__writes_read_back_before_they_are_programmed(void) {
  const uint8_t data[5] = {1, 2, 3, 4, 5};

  prv_write(0x1003U, data, sizeof(data), 0U);
  prv_check_reads();
  cl_assert_equal_i(s_programs, 0);
  // Nothing reached the flash, so its cache stays
  cl_assert_equal_i(s_syncs, 0);
  cl_assert_equal_i(ecc_nor_next_due_ms(&s_nor, 0U), ECC_NOR_PARTIAL_MS);
}

void test_ecc_nor__whole_words_are_programmed_once_settled(void) {
  uint8_t data[40];

  memset(data, 0x5A, sizeof(data));
  prv_write(0x2000U, data, sizeof(data), 100U);

  cl_assert_equal_i(ecc_nor_next_due_ms(&s_nor, 100U), ECC_NOR_SETTLE_MS);
  cl_assert_equal_i(ecc_nor_flush(&s_nor, 100U + ECC_NOR_SETTLE_MS - 1U, false), 0);
  cl_assert_equal_i(s_programs, 0);

  cl_assert_equal_i(ecc_nor_flush(&s_nor, 100U + ECC_NOR_SETTLE_MS, false), 0);
  // Two whole words; the third, 8 bytes of it written, waits for the rest
  cl_assert_equal_i(s_programs, 2);
  cl_assert_equal_i(s_syncs, 1);
  cl_assert(memcmp(&s_flash[0x2000U], data, 32U) == 0);
  cl_assert_equal_i(s_flash[0x2020U], 0xFF);
  prv_check_reads();

  cl_assert_equal_i(ecc_nor_flush(&s_nor, 100U + ECC_NOR_PARTIAL_MS, false), 0);
  cl_assert_equal_i(s_programs, 3);
  cl_assert(memcmp(s_flash, s_ref, sizeof(s_flash)) == 0);
  cl_assert_equal_i(prv_total_erases(), 0);
  cl_assert_equal_i(ecc_nor_next_due_ms(&s_nor, 0U), UINT32_MAX);
}

// The flash log's pattern: a record header with its flags erased, the message, then the flags
// cleared in place, settling between records
void test_ecc_nor__appends_and_flag_updates_erase_nothing(void) {
  uint32_t addr = 0x4000U;
  uint32_t now = 0U;

  for (unsigned int i = 0U; i < 300U; i++) {
    const uint8_t hdr[2] = {0xFF, (uint8_t)(10U + (i % 17U))};
    uint8_t msg[32];
    const uint8_t flags = 0xFE;
    const uint32_t start = addr;

    memset(msg, (int)i, hdr[1]);
    prv_write(addr, hdr, sizeof(hdr), now);
    prv_write(addr + 2U, msg, hdr[1], now);
    prv_write(start, &flags, 1U, now);
    addr += 2U + hdr[1];

    now += 700U;
    cl_assert_equal_i(ecc_nor_flush(&s_nor, now, false), 0);
    prv_check_reads();
  }

  cl_assert_equal_i(ecc_nor_flush(&s_nor, now, true), 0);
  cl_assert(memcmp(s_flash, s_ref, sizeof(s_flash)) == 0);
  cl_assert_equal_i(prv_total_erases(), 0);
}

void test_ecc_nor__clearing_programmed_bits_rewrites_the_page(void) {
  uint8_t data[64];
  const uint8_t clear = 0x0F;

  memset(data, 0xA5, sizeof(data));
  prv_write(0x6000U, data, sizeof(data), 0U);
  prv_write(0x7000U, data, sizeof(data), 0U);
  cl_assert_equal_i(ecc_nor_flush(&s_nor, 0U, true), 0);
  cl_assert_equal_i(prv_total_erases(), 0);

  prv_write(0x6010U, &clear, 1U, 10U);
  cl_assert_equal_i(ecc_nor_flush(&s_nor, 10U, true), 0);
  cl_assert_equal_i(s_erases[0x6000U / ECC_NOR_PAGE_SIZE], 1);
  cl_assert(memcmp(s_flash, s_ref, sizeof(s_flash)) == 0);
}

void test_ecc_nor__a_subsector_erase_keeps_the_other_half(void) {
  uint8_t data[100];

  memset(data, 0x33, sizeof(data));
  prv_write(0x8000U, data, sizeof(data), 0U);
  prv_write(0x9000U, data, sizeof(data), 0U);
  cl_assert_equal_i(ecc_nor_flush(&s_nor, 0U, true), 0);

  prv_erase(0x8000U, SUBSECTOR, 5U);
  cl_assert_equal_i(s_erases[0x8000U / ECC_NOR_PAGE_SIZE], 1);
  cl_assert(memcmp(s_flash, s_ref, sizeof(s_flash)) == 0);
  prv_check_reads();

  // An erased half is erased in the flash: nothing to do
  prv_erase(0x8000U, SUBSECTOR, 6U);
  cl_assert_equal_i(s_erases[0x8000U / ECC_NOR_PAGE_SIZE], 1);
}

void test_ecc_nor__pending_writes_survive_an_erase_of_the_other_half(void) {
  const uint8_t data[3] = {1, 2, 3};
  uint8_t old[16];

  memset(old, 0x11, sizeof(old));
  prv_write(0xA000U, old, sizeof(old), 0U);
  cl_assert_equal_i(ecc_nor_flush(&s_nor, 0U, true), 0);

  prv_write(0xB000U, data, sizeof(data), 1U);
  prv_erase(0xA000U, SUBSECTOR, 2U);
  prv_check_reads();

  cl_assert_equal_i(ecc_nor_flush(&s_nor, 3U, true), 0);
  cl_assert(memcmp(s_flash, s_ref, sizeof(s_flash)) == 0);
}

void test_ecc_nor__write_through_programs_at_once(void) {
  const uint8_t data[7] = {9, 8, 7, 6, 5, 4, 3};
  const uint8_t more[3] = {0x10, 0x20, 0x30};

  prv_write(0x3000U, data, sizeof(data), 0U);
  cl_assert_equal_i(ecc_nor_set_write_through(&s_nor, 0U), 0);
  cl_assert(memcmp(s_flash, s_ref, sizeof(s_flash)) == 0);

  prv_write(0x3007U, more, sizeof(more), 1U);
  cl_assert(memcmp(s_flash, s_ref, sizeof(s_flash)) == 0);
}

// Writes, erases, flushes and evictions at random, the flash always reading as the NOR flash
void test_ecc_nor__random_operations_match_nor(void) {
  uint32_t now = 0U;

  srand(1234);
  for (unsigned int i = 0U; i < 4000U; i++) {
    const unsigned int op = (unsigned int)rand() % 100U;

    if (op < 70U) {
      uint8_t buf[300];
      const size_t len = 1U + ((unsigned int)rand() % sizeof(buf));
      const uint32_t addr = (uint32_t)rand() % (FLASH_SIZE - len);

      for (size_t j = 0U; j < len; j++) {
        buf[j] = (uint8_t)rand();
      }
      prv_write(addr, buf, len, now);
    } else if (op < 80U) {
      const uint32_t addr = ((uint32_t)rand() % (FLASH_SIZE / SUBSECTOR)) * SUBSECTOR;
      const size_t len = (((unsigned int)rand() % 3U) + 1U) * SUBSECTOR;

      prv_erase(addr, (addr + len > FLASH_SIZE) ? (FLASH_SIZE - addr) : len, now);
    } else {
      cl_assert_equal_i(ecc_nor_flush(&s_nor, now, (op % 10U) == 0U), 0);
    }

    now += (uint32_t)rand() % 2000U;
    prv_check_reads();
  }

  cl_assert_equal_i(ecc_nor_flush(&s_nor, now, true), 0);
  cl_assert(memcmp(s_flash, s_ref, sizeof(s_flash)) == 0);
}
