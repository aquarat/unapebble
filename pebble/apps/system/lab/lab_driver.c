/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

// The eMMC through the drivers the eMMC builds use: the MMC class with the STM32 SDMMC driver,
// the supply cut under it as their board did at boot, and the NOR emulation on its blocks, timed
// to estimate what a filesystem format takes.

#include "lab.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include <pbl/crc/crc.h>
#include <pbl/drivers/gpio.h>
#include <pbl/drivers/mmc.h>
#include <pbl/drivers/mmc/stm32_sdmmc.h>
#include <pbl/kernel/delay.h>
#include <pbl/kernel/time.h>
#include <pbl/util/math.h>
#include <pbl/util/misc.h>
#include <pbl/util/size.h>

// The NOR emulation of the eMMC builds' storage, private to the flash drivers
#include "../../../../drivers/flash/blk_nor.h"

#define MULTI_BLOCKS 32U

static struct {
  uint8_t *buf;
  struct blk_nor *nor;
} s;

static uint32_t prv_ms_since(uint64_t start) {
  return (uint32_t)(pbl_uptime_ms() - start);
}

static const struct pbl_mmc_stm32_sdmmc *prv_sd(void) {
  return PBL_CONTAINER_OF(MMC, const struct pbl_mmc_stm32_sdmmc, mmc);
}

static int prv_clock(void) {
  s.buf = lab_alloc(MULTI_BLOCKS * 512U);
  if (s.buf == nullptr) {
    return -ENOMEM;
  }

  return lab_emmc_clock_select(lab_emmc_clock_is_pll1_q());
}

static int prv_power_up(void) {
  const uint64_t start = pbl_uptime_ms();
  uint32_t blocks;
  int ret;

  ret = pbl_mmc_get_num_blocks(MMC, &blocks);
  lab_printf("%d, %lu blocks, %lu ms", ret, (unsigned long)blocks,
             (unsigned long)prv_ms_since(start));

  return ret;
}

static int prv_read_one(void) {
  const uint64_t start = pbl_uptime_ms();
  int ret = lab_mmc_read(LAB_GUI_FIRST, s.buf, 1U);

  if (ret == 0) {
    lab_printf("%lu ms, crc 0x%08" PRIx32, (unsigned long)prv_ms_since(start),
               pbl_crc32(0U, s.buf, 512U));
    lab_hex("", s.buf, 16U);
  }

  return ret;
}

static int prv_read_multi(void) {
  const uint64_t start = pbl_uptime_ms();
  int ret = lab_mmc_read(LAB_GUI_FIRST, s.buf, MULTI_BLOCKS);

  if (ret == 0) {
    lab_printf("%u blocks %lu ms, crc 0x%08" PRIx32, MULTI_BLOCKS,
               (unsigned long)prv_ms_since(start), pbl_crc32(0U, s.buf, MULTI_BLOCKS * 512U));
  }

  return ret;
}

static int prv_write_test(void) {
  uint8_t *orig = s.buf;
  uint8_t *pattern = &s.buf[512];
  uint8_t *back = &s.buf[1024];
  uint64_t start;
  int ret;

  ret = lab_mmc_read(LAB_SCRATCH_LBA, orig, 1U);
  if (ret != 0) {
    return ret;
  }
  for (size_t i = 0U; i < 512U; i++) {
    pattern[i] = (uint8_t)(i * 13U + 0xA5U);
  }
  start = pbl_uptime_ms();
  ret = lab_mmc_write(LAB_SCRATCH_LBA, pattern, 1U);
  lab_printf("write: %d, %lu ms", ret, (unsigned long)prv_ms_since(start));
  if (ret == 0) {
    ret = lab_mmc_read(LAB_SCRATCH_LBA, back, 1U);
  }
  if (ret == 0) {
    lab_printf("read back: %s", memcmp(pattern, back, 512U) == 0 ? "same" : "DIFFERENT");
    ret = lab_mmc_write(LAB_SCRATCH_LBA, orig, 1U);
  }
  if (ret == 0) {
    ret = lab_mmc_read(LAB_SCRATCH_LBA, back, 1U);
  }
  if (ret == 0) {
    lab_printf("restored: %s", memcmp(orig, back, 512U) == 0 ? "same" : "DIFFERENT");
  }

  return ret;
}

static int prv_idle_off(void) {
  pbl_sleep_ms(CONFIG_MMC_IDLE_OFF_MS + 500U);
  lab_printf("card %s after %u ms idle", MMC->state->powered ? "STILL ON" : "off",
             CONFIG_MMC_IDLE_OFF_MS + 500U);

  return 0;
}

static int prv_power_off(void) {
  pbl_mmc_power_off(MMC);
  lab_printf("card %s", MMC->state->powered ? "on" : "off");

  return 0;
}

static void prv_cleanup(void) {
  pbl_mmc_power_off(MMC);
  s.buf = nullptr;
  s.nor = nullptr;
}

static const struct lab_step s_driver_steps[] = {
  {"kernel clock", prv_clock},
  {"power up (MMC class)", prv_power_up},
  {"read block 409600", prv_read_one},
  {"read 32 blocks", prv_read_multi},
  {"write/read block 600000", prv_write_test},
  {"idle: card powers off", prv_idle_off},
  {"read after idle", prv_read_one},
  {"power off", prv_power_off},
};

const struct lab_experiment lab_exp_emmc_driver = {
  .title = "eMMC via driver",
  .subtitle = "MMC class, as the eMMC builds",
  .steps = s_driver_steps,
  .num_steps = PBL_ARRAY_SIZE(s_driver_steps),
  .cleanup = prv_cleanup,
};

// The board of the eMMC builds, coming up after the NOR window, drove EMMC_EN low under the
// driver, the pins left with their pull-ups

static int prv_cut(void) {
  (void)pbl_gpio_set(&prv_sd()->enable, false);
  lab_printf("EMMC_EN low, pins as they were");
  pbl_sleep_ms(100U);

  return 0;
}

static int prv_read_after_cut(void) {
  const uint64_t start = pbl_uptime_ms();
  int ret = lab_mmc_read(LAB_GUI_FIRST, s.buf, 1U);

  lab_printf("read: %d after %lu ms", ret, (unsigned long)prv_ms_since(start));
  lab_printf("%s", ret == 0 ? "recovered by power cycle" : "NOT recovered: B would assert");

  return ret;
}

static const struct lab_step s_cut_steps[] = {
  {"kernel clock", prv_clock},          {"power up (MMC class)", prv_power_up},
  {"cut supply as B's board", prv_cut}, {"read, as B's next access", prv_read_after_cut},
  {"read again", prv_read_one},         {"power off", prv_power_off},
};

const struct lab_experiment lab_exp_emmc_supply_cut = {
  .title = "eMMC supply cut",
  .subtitle = "B's board cut EMMC_EN at boot",
  .steps = s_cut_steps,
  .num_steps = PBL_ARRAY_SIZE(s_cut_steps),
  .cleanup = prv_cleanup,
};

// NOR on the Lab's scratch blocks, beyond the eMMC builds' window

#define NOR_FIRST LAB_SCRATCH_LBA
#define NOR_SIZE  (128U * 1024U)

static int prv_hw_read(void *ctx, uint32_t block, void *buf, uint32_t count) {
  if (block + count > NOR_SIZE / 512U) {
    return -EINVAL;
  }

  return lab_mmc_read(NOR_FIRST + block, buf, count);
}

static int prv_hw_write(void *ctx, uint32_t block, const void *buf, uint32_t count) {
  if (block + count > NOR_SIZE / 512U) {
    return -EINVAL;
  }

  return lab_mmc_write(NOR_FIRST + block, buf, count);
}

static const struct blk_nor_hw s_hw = {
  .read = prv_hw_read,
  .write = prv_hw_write,
};

static int prv_nor_init(void) {
  int ret = prv_clock();

  if (ret != 0) {
    return ret;
  }
  s.nor = lab_alloc(sizeof(*s.nor));
  if (s.nor == nullptr) {
    return -ENOMEM;
  }
  blk_nor_init(s.nor, &s_hw, nullptr);
  lab_printf("window at block %u, %u KiB", NOR_FIRST, NOR_SIZE / 1024U);

  return prv_power_up();
}

static int prv_check(uint32_t addr, uint32_t len, uint8_t value, const char *what) {
  for (uint32_t off = 0U; off < len; off += 512U) {
    const uint32_t n = PBL_MIN(512U, len - off);
    int ret = blk_nor_read(s.nor, addr + off, s.buf, n);

    if (ret != 0) {
      return ret;
    }
    for (uint32_t i = 0U; i < n; i++) {
      if (s.buf[i] != value) {
        lab_printf("%s: 0x%02x at 0x%lx", what, s.buf[i], (unsigned long)(addr + off + i));
        return -EIO;
      }
    }
  }
  lab_printf("%s: good", what);

  return 0;
}

static int prv_nor_erase(void) {
  const uint64_t start = pbl_uptime_ms();
  uint32_t ms;
  int ret = blk_nor_erase(s.nor, 0U, 64U * 1024U);

  ms = prv_ms_since(start);
  lab_printf("64 KiB erase: %d, %lu ms", ret, (unsigned long)ms);
  // A 16 MiB filesystem's format erases 256 of these
  lab_printf("16 MiB: about %lu s", (unsigned long)(ms * 256U / 1000U));
  if (ret != 0) {
    return ret;
  }

  return prv_check(0U, 64U * 1024U, 0xFFU, "erased");
}

// Small writes, as PFS writes its page headers: 4096 of them in a format
static int prv_nor_program(void) {
  static const uint8_t s_a[13] = {0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0,
                                  0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0};
  static const uint8_t s_b[13] = {0x0F, 0x3F, 0x0F, 0x3F, 0x0F, 0x3F, 0x0F,
                                  0x3F, 0x0F, 0x3F, 0x0F, 0x3F, 0x0F};
  uint8_t back[13];
  uint64_t start;
  uint32_t ms;
  int ret;

  // Across a block boundary
  start = pbl_uptime_ms();
  ret = blk_nor_write(s.nor, 4090U, s_a, sizeof(s_a));
  ms = prv_ms_since(start);
  if (ret == 0) {
    ret = blk_nor_write(s.nor, 4090U, s_b, sizeof(s_b));
  }
  if (ret == 0) {
    ret = blk_nor_read(s.nor, 4090U, back, sizeof(back));
  }
  if (ret != 0) {
    return ret;
  }
  for (size_t i = 0U; i < sizeof(back); i++) {
    if (back[i] != (uint8_t)(s_a[i] & s_b[i])) {
      lab_printf("AND fails at %u: 0x%02x", (unsigned int)i, back[i]);
      return -EIO;
    }
  }
  lab_printf("bits cleared only: good");

  // One header-sized write per 4 KiB page, cache cold
  blk_nor_invalidate(s.nor);
  start = pbl_uptime_ms();
  for (uint32_t page = 0U; page < 8U; page++) {
    ret = blk_nor_write(s.nor, page * 4096U + 64U, s_b, 12U);
    if (ret != 0) {
      return ret;
    }
  }
  ms = prv_ms_since(start);
  lab_printf("8 header writes: %lu ms", (unsigned long)ms);
  lab_printf("4096 headers: about %lu s", (unsigned long)(ms * 512U / 1000U));

  return 0;
}

// What PFS's erase does first: a blank check of 128-byte reads
static int prv_nor_blank_check(void) {
  uint8_t chunk[128];
  const uint64_t start = pbl_uptime_ms();
  uint32_t ms;
  int ret;

  blk_nor_invalidate(s.nor);
  for (uint32_t off = 0U; off < 64U * 1024U; off += sizeof(chunk)) {
    ret = blk_nor_read(s.nor, off, chunk, sizeof(chunk));
    if (ret != 0) {
      return ret;
    }
  }
  ms = prv_ms_since(start);
  lab_printf("64 KiB blank check: %lu ms", (unsigned long)ms);
  lab_printf("16 MiB: about %lu s", (unsigned long)(ms * 256U / 1000U));

  return 0;
}

static int prv_nor_subsector(void) {
  const uint64_t start = pbl_uptime_ms();
  int ret = blk_nor_erase(s.nor, 4096U, 4096U);

  lab_printf("4 KiB erase: %d, %lu ms", ret, (unsigned long)prv_ms_since(start));
  if (ret != 0) {
    return ret;
  }
  ret = prv_check(4096U, 4096U, 0xFFU, "erased");
  if (ret == 0) {
    // The block before it keeps what was written
    ret = blk_nor_read(s.nor, 4090U, s.buf, 6U);
    lab_printf("neighbour %s", (ret == 0 && s.buf[0] == 0x00U) ? "kept" : "CHANGED");
  }

  return ret;
}

static const struct lab_step s_nor_steps[] = {
  {"init, card up", prv_nor_init},
  {"erase 64 KiB", prv_nor_erase},
  {"program and read back", prv_nor_program},
  {"blank check, as PFS", prv_nor_blank_check},
  {"erase 4 KiB", prv_nor_subsector},
  {"power off", prv_power_off},
};

const struct lab_experiment lab_exp_nor = {
  .title = "NOR window",
  .subtitle = "Erase/program on scratch, timed",
  .steps = s_nor_steps,
  .num_steps = PBL_ARRAY_SIZE(s_nor_steps),
  .cleanup = prv_cleanup,
};
