/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * @defgroup boot_trace Boot trace
 * @ingroup subsys
 * @brief Breadcrumbs and fault records that survive a reset, for a watch without a console.
 *
 * A breadcrumb is an area and a step, written before something that may not return. The last one
 * is kept in a backup register, which survives any reset and the bootloader installing another
 * image; the last @ref PBL_BOOT_TRACE_TRAIL ones, with their uptime, the last CPU fault and the
 * last panic are kept in a RAM record, which survives a reset but not a power loss. At the next
 * boot the record of the run before is kept apart, with the reset flags of the SoC, for a viewer
 * to show.
 *
 * @code{.c}
 * PBL_BOOT_TRACE(PBL_BT_PFS, PBL_BT_PFS_FORMAT);
 * pfs_format(true);
 * @endcode
 * @{
 */

/** @brief Areas of breadcrumbs. */
enum pbl_boot_trace_area {
  PBL_BT_NONE = 0,
  /** The kernel: boot, reboot and power-off. */
  PBL_BT_KERNEL = 1,
  /** The stages of the firmware's main(). */
  PBL_BT_MAIN = 2,
  /** The MMC class. */
  PBL_BT_MMC = 3,
  /** The STM32 SDMMC driver. */
  PBL_BT_SDMMC = 4,
  /** The NOR window on the MMC. */
  PBL_BT_NOR = 5,
  /** The filesystem. */
  PBL_BT_PFS = 6,
  /** The system resources. */
  PBL_BT_RES = 7,
  /** The battery drivers. */
  PBL_BT_BATT = 8,
  /** Experiments of the Lab app, one area each from here. */
  PBL_BT_LAB = 0x40,
};

/** @brief Steps of @ref PBL_BT_KERNEL. */
enum {
  PBL_BT_KERNEL_START = 1,
  PBL_BT_KERNEL_DEVICES = 2,
  PBL_BT_KERNEL_MAIN = 3,
  PBL_BT_KERNEL_BOOT_DONE = 4,
  PBL_BT_KERNEL_REBOOT = 5,
  PBL_BT_KERNEL_POWER_OFF = 6,
  PBL_BT_KERNEL_EMERGENCY = 7,
};

/** @brief Steps of @ref PBL_BT_MAIN, the stages of the firmware's main(). */
enum {
  PBL_BT_MAIN_START = 1,
  PBL_BT_MAIN_CLOCK_RESTORE,
  PBL_BT_MAIN_SPLASH,
  PBL_BT_MAIN_INIT_DRIVERS,
  PBL_BT_MAIN_CLOCK_INIT,
  PBL_BT_MAIN_LOG_FLASH,
  PBL_BT_MAIN_DEBUG,
  PBL_BT_MAIN_SERVICES_EARLY,
  PBL_BT_MAIN_PRF_UPDATE,
  PBL_BT_MAIN_RESOURCES,
  PBL_BT_MAIN_SYSTEM_RESOURCES,
  PBL_BT_MAIN_COMPOSITOR,
  PBL_BT_MAIN_BLUETOOTH,
  PBL_BT_MAIN_SERVICES,
  PBL_BT_MAIN_LAUNCHER,
};

/** @brief Steps of @ref PBL_BT_MMC. */
enum {
  /** Powering the card up, try in the low byte. */
  PBL_BT_MMC_POWER_ON = 0x100,
  PBL_BT_MMC_ON = 0x200,
  PBL_BT_MMC_POWER_ON_FAILED = 0x300,
  /** A transfer failed: 0x400 a read, 0x500 a write, then retried. */
  PBL_BT_MMC_READ_FAILED = 0x400,
  PBL_BT_MMC_WRITE_FAILED = 0x500,
  PBL_BT_MMC_OFF = 0x600,
};

/** @brief Steps of @ref PBL_BT_SDMMC, the card's power-up. */
enum {
  PBL_BT_SDMMC_SUPPLY = 1,
  PBL_BT_SDMMC_CLOCK,
  PBL_BT_SDMMC_CMD0,
  PBL_BT_SDMMC_CMD1,
  PBL_BT_SDMMC_CID_RCA_SELECT,
  PBL_BT_SDMMC_EXT_CSD,
  PBL_BT_SDMMC_PARTITION,
  PBL_BT_SDMMC_POWER_CLASS,
  PBL_BT_SDMMC_BUS_WIDTH,
  PBL_BT_SDMMC_HS,
  PBL_BT_SDMMC_EXT_CSD_AGAIN,
  PBL_BT_SDMMC_READY,
  PBL_BT_SDMMC_FAILED,
};

/** @brief Steps of @ref PBL_BT_NOR. */
enum {
  PBL_BT_NOR_INIT = 1,
  PBL_BT_NOR_READY,
  PBL_BT_NOR_FAILED,
  /** An erase, of the 4 KiB unit in the low bits. */
  PBL_BT_NOR_ERASE = 0x10000,
};

/** @brief Steps of @ref PBL_BT_PFS. */
enum {
  PBL_BT_PFS_INIT = 1,
  PBL_BT_PFS_CHECK,
  PBL_BT_PFS_FORMAT,
  PBL_BT_PFS_FORMAT_HEADERS,
  PBL_BT_PFS_FORMAT_DONE,
  PBL_BT_PFS_GC_RECOVER,
  PBL_BT_PFS_GC_REGION,
  PBL_BT_PFS_PREPARE,
  PBL_BT_PFS_READY,
};

/** @brief Steps of @ref PBL_BT_RES. */
enum {
  /** Checking bank n, in the low bits. */
  PBL_BT_RES_CHECK_BANK = 0x10,
  PBL_BT_RES_FOUND = 0x20,
  /** Installing into bank n from the update drive. */
  PBL_BT_RES_INSTALL = 0x30,
  PBL_BT_RES_MOUNT = 0x40,
  PBL_BT_RES_SEARCH,
  PBL_BT_RES_ERASE,
  PBL_BT_RES_INSTALLED,
  PBL_BT_RES_INSTALL_FAILED,
  /** None, about to power off. */
  PBL_BT_RES_MISSING = 0xF0,
  /** Copying, at the 4 KiB chunk in the low bits. */
  PBL_BT_RES_COPY = 0x10000,
};

/** @brief Steps of @ref PBL_BT_BATT. */
enum {
  PBL_BT_BATT_CHARGER = 1,
  PBL_BT_BATT_CHARGER_READY,
  PBL_BT_BATT_GAUGE,
  PBL_BT_BATT_GAUGE_READY,
};

/** @brief Values noted along the way, kept in the record. */
enum pbl_boot_trace_note {
  /** CRC of the system resources the firmware wants. */
  PBL_BT_NOTE_RES_CRC,
  /** Last MMC error, negative errno. */
  PBL_BT_NOTE_MMC_ERR,
  /** Blocks of the card. */
  PBL_BT_NOTE_MMC_BLOCKS,
  /** OCR of the card. */
  PBL_BT_NOTE_MMC_OCR,
  /** SDMMC STA of the last error. */
  PBL_BT_NOTE_SDMMC_STA,
  /** Bytes of resources copied. */
  PBL_BT_NOTE_RES_COPIED,
  /** Value of the last step that reported one, by the area. */
  PBL_BT_NOTE_VALUE,
  PBL_BT_NUM_NOTES = 8,
};

/** @brief Breadcrumbs kept in the RAM record. */
#define PBL_BOOT_TRACE_TRAIL 40U

/** @brief A breadcrumb: the area in the top byte, the step below. */
#define PBL_BOOT_TRACE_CRUMB(area, step) \
  (((uint32_t)(area) << 24) | ((uint32_t)(step) & 0x00FFFFFFU))
/** @brief Area of a breadcrumb. */
#define PBL_BOOT_TRACE_AREA(crumb) ((uint8_t)((crumb) >> 24))
/** @brief Step of a breadcrumb. */
#define PBL_BOOT_TRACE_STEP(crumb) ((crumb) & 0x00FFFFFFU)

/** @brief A CPU fault. */
struct pbl_boot_trace_fault {
  /** enum pbl_fault_reason + 1, 0 for none. */
  uint32_t kind;
  uint32_t pc;
  uint32_t lr;
  uint32_t psr;
  uint32_t sp;
  uint32_t exc_return;
  uint32_t cfsr;
  uint32_t hfsr;
  uint32_t shcsr;
  uint32_t mmfar;
  uint32_t bfar;
  uint32_t uptime_ms;
  /** The last breadcrumb when it happened. */
  uint32_t crumb;
  char thread[12];
};

/** @brief A panic or failed assertion. */
struct pbl_boot_trace_panic {
  /** Non-zero when set. */
  uint32_t valid;
  uint32_t lr;
  uint32_t line;
  uint32_t uptime_ms;
  uint32_t crumb;
  char file[24];
  char msg[64];
};

/** @brief The RAM record of a run. */
struct pbl_boot_trace_record {
  uint32_t magic;
  /** Boots seen by images with the trace, the same as the backup register's. */
  uint32_t boot_count;
  /** The image: CONFIG_BOOT_TRACE_IMAGE and its version. */
  char image[8];
  char version[32];
  /** RCC CSR when the run started: how the run before it ended. */
  uint32_t reset_flags;
  /** The reboot reason registers when the run started. */
  uint32_t reboot_reason[4];
  /** Breadcrumbs written; the last ones are in trail[], at count % PBL_BOOT_TRACE_TRAIL. */
  uint32_t trail_count;
  struct {
    uint32_t crumb;
    uint32_t uptime_ms;
  } trail[PBL_BOOT_TRACE_TRAIL];
  struct pbl_boot_trace_fault fault;
  struct pbl_boot_trace_panic panic;
  uint32_t notes[PBL_BT_NUM_NOTES];
  /** Bit n: notes[n] set. */
  uint32_t notes_set;
  uint32_t magic_end;
};

/** @brief What this boot found of the run before. */
struct pbl_boot_trace_boot {
  /** RCC CSR at this boot: why the run before ended. */
  uint32_t reset_flags;
  /** The breadcrumb backup register. */
  uint32_t bkp_crumb;
  /** The boot count backup register: 0xB7 in the top byte, image id, then the count. */
  uint32_t bkp_boot;
  /** Whether the backup registers held a breadcrumb. */
  bool bkp_valid;
  /** Whether the RAM record of the run before was found. */
  bool ram_valid;
  /** Whether it is the one of the last boot the backup registers counted. */
  bool ram_current;
  /** The record of the run before, when ram_valid. */
  struct pbl_boot_trace_record previous;
};

#ifdef CONFIG_BOOT_TRACE

/** @brief Keep the run before apart and start the record of this one, at the start of boot. */
void pbl_boot_trace_init(void);

/**
 * @brief Write a breadcrumb.
 *
 * @param area Area, from enum pbl_boot_trace_area.
 * @param step Step, 24 bits.
 */
void pbl_boot_trace_crumb(uint8_t area, uint32_t step);

/** @brief Note a value. */
void pbl_boot_trace_note(enum pbl_boot_trace_note note, uint32_t value);

/** @brief Record a CPU fault, from the fault handler. */
void pbl_boot_trace_fault(const struct pbl_boot_trace_fault *fault);

/** @brief Record a panic, before it is logged. */
void pbl_boot_trace_panic(const char *file, int line, uintptr_t lr, const char *msg);

/** @brief What this boot found of the run before. */
const struct pbl_boot_trace_boot *pbl_boot_trace_boot_info(void);

/** @brief The record of this run, as it is now. */
const struct pbl_boot_trace_record *pbl_boot_trace_current(void);

/** @brief The name of an area, "?" if unknown. */
const char *pbl_boot_trace_area_name(uint8_t area);

#else

static inline void pbl_boot_trace_crumb(uint8_t area, uint32_t step) {
}

static inline void pbl_boot_trace_note(enum pbl_boot_trace_note note, uint32_t value) {
}

#endif

/** @brief A breadcrumb of the boot path, kept only with @c CONFIG_BOOT_TRACE_BOOT_PATH. */
#ifdef CONFIG_BOOT_TRACE_BOOT_PATH
#define PBL_BOOT_TRACE(area, step)     pbl_boot_trace_crumb((area), (step))
#define PBL_BOOT_TRACE_NOTE(note, val) pbl_boot_trace_note((note), (val))
#else
#define PBL_BOOT_TRACE(area, step) \
  do {                             \
  } while (0)
#define PBL_BOOT_TRACE_NOTE(note, val) \
  do {                                 \
  } while (0)
#endif

/** @} */
