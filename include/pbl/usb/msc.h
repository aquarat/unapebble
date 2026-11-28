/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * @defgroup usb_msc USB drive
 * @ingroup usbd
 * @brief A read-only, removable USB drive: mass storage, bulk-only transport, SCSI commands.
 *
 * One function of the device, one interface with a bulk endpoint each way, one logical unit of
 * 512-byte blocks, write-protected. A thread of its own runs the host's commands and calls the
 * medium's operations, which may take their time: the host waits, or polls while the medium is
 * becoming ready. Writes fail as the protection says, after their data is taken. The medium can
 * be absent, which hosts show as an empty card reader would; when one appears, or changes, the
 * host is told the medium may have changed (a unit attention).
 * @{
 */

/** @brief What the drive holds. Every operation runs in the drive's thread. */
struct pbl_usb_msc_medium {
  /**
   * Whether there is a medium: 0 with its number of blocks, @c -ENOMEDIUM for none, @c -EAGAIN
   * while it is becoming ready. Asked before each command that reaches the medium.
   */
  int (*status)(uint32_t *num_blocks, void *ctx);
  /** Read @p count blocks from @p lba: 0, or a negative errno. */
  int (*read)(uint32_t lba, void *buf, uint32_t count, void *ctx);
  /** Optional. The host ejected the medium (START STOP UNIT). */
  void (*eject)(void *ctx);
  void *ctx;
};

/**
 * @brief Give the drive its medium, starting the drive's thread the first time.
 *
 * @param medium The medium, which must outlive the drive.
 */
void pbl_usb_msc_init(const struct pbl_usb_msc_medium *medium);

/** @brief The medium changed: the next command tells the host so. */
void pbl_usb_msc_changed(void);

/** @brief Whether the host has the drive's interface configured. */
bool pbl_usb_msc_is_configured(void);

/** @brief Uptime of the host's last read, in milliseconds, the low 32 bits; 0 for none. */
uint32_t pbl_usb_msc_last_read_ms(void);

/** @} */
