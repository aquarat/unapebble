/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pbl/drivers/gnss.h>
#include <pbl/drivers/gpio.h>
#include <pbl/drivers/uart.h>

/**
 * @defgroup drivers_gnss_ag3335 Airoha AG3335
 * @ingroup drivers_gnss
 * @brief @ref drivers_gnss driver for the Airoha AG3335 receivers, on a UART.
 *
 * The receiver is powered by its enable line, which is all there is: no reset, no wake-up line.
 * Started, the driver powers it, waits for it to say its GNSS subsystem is on ($PAIR001,002,0),
 * sets it up with its PAIR commands and gathers its NMEA sentences, GGA, RMC, GSA and GSV, into
 * an epoch at each burst, reported once the line goes quiet. Stopped, it cuts its power and puts
 * the UART's pins to sleep. The UART hands the driver each byte from its interrupt; the driver
 * parses them on the high priority system work queue: the shell, whose commands wait for the
 * receiver, runs on the other one.
 *
 * @code{.c}
 * PBL_GNSS_AG3335_DEFINE(s_gnss, &s_lpuart1, PBL_GPIO(&dt_gpiog, 5, 0));
 * @endcode
 * @{
 */

/** @brief Bytes received and not parsed yet, at most. */
#define PBL_GNSS_AG3335_RX_RING_SIZE 2048U

/** @cond INTERNAL_HIDDEN */
struct pbl_gnss_ag3335_data;
/** @endcond */

/** @brief AG3335 receiver. */
struct pbl_gnss_ag3335 {
  /** Receiver, child of the UART. */
  struct pbl_gnss gnss;
  /** UART. */
  const struct pbl_uart *uart;
  /** Enable line, active when powered. */
  struct pbl_gpio enable;
  /** Runtime data. */
  struct pbl_gnss_ag3335_data *data;
};

/** @cond INTERNAL_HIDDEN */
extern const struct pbl_gnss_ops pbl_gnss_ag3335_ops;
extern struct pbl_gnss_ag3335_data pbl_gnss_ag3335_data;
/** @endcond */

/**
 * @brief Define the receiver. A board has one.
 *
 * @param sym Symbol of the receiver.
 * @param _uart UART.
 * @param _enable Enable line, from PBL_GPIO().
 */
#define PBL_GNSS_AG3335_DEFINE(sym, _uart, _enable)                                             \
  PBL_GNSS_STATE_DEFINE(sym);                                                                   \
  PBL_DEVICE_INSTANCE(sym, struct pbl_gnss_ag3335, struct pbl_gnss, gnss) {                     \
    .gnss = PBL_GNSS_INIT(sym, "ag3335", &(_uart)->dev, &pbl_gnss_ag3335_ops), .uart = (_uart), \
    .enable = _enable, .data = &pbl_gnss_ag3335_data,                                           \
  }

/**
 * @brief Send the running receiver a PAIR command and wait for its acknowledgement.
 *
 * For diagnostics and experiments. Only commands that change the receiver's running state are
 * taken, none that write its flash; their effect lasts until it is powered down.
 *
 * @param gnss Receiver.
 * @param cmd Command without '$' and checksum, as "PAIR066,1,0,0,0,0,0".
 * @param[out] reply The receiver's answer to a query, as "PAIR067,1,0,0,0,0,0", or empty.
 * @param reply_len Size of @p reply.
 * @return The result the receiver acknowledged, 0 for success, -EPERM for a command not taken,
 *         -ENODEV if the receiver is not running, -EBUSY if it is still starting or another
 *         command waits, -ETIMEDOUT without an acknowledgement.
 */
int pbl_gnss_ag3335_command(const struct pbl_gnss *gnss, const char *cmd, char *reply,
                            size_t reply_len);

/** @} */
