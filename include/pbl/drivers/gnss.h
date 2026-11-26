/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <pbl/device.h>
#include <pbl/kernel/mutex.h>

/**
 * @defgroup drivers_gnss GNSS receiver
 * @ingroup drivers
 * @brief Satellite positioning receiver class.
 *
 * A GNSS receiver is a struct pbl_gnss device. Once started, the driver reports one fix per
 * navigation epoch to the listener a consumer registered with pbl_gnss_set_listener(), the
 * location service in practice, including epochs without a position so that the consumer can
 * tell the receiver is still searching. Protocol handling (NMEA, vendor binary protocols) stays
 * in the driver: the class only sees decoded fixes.
 *
 * Every call into the driver, and every report out of it, runs with the device lock held, a
 * recursive mutex. The listener runs with it held too.
 *
 * @code{.c}
 * static void prv_fix(const struct pbl_gnss *gnss, const struct pbl_gnss_fix *fix,
 *                     void *user_data) {
 *   if (fix->quality >= PBL_GNSS_FIX_2D) {
 *     ...
 *   }
 * }
 *
 * static const struct pbl_gnss_listener s_listener = {.fix = prv_fix};
 *
 * pbl_gnss_set_listener(GNSS, &s_listener);
 * pbl_gnss_start(GNSS, 1000);
 * @endcode
 * @{
 */

/** @brief Fix quality. */
enum pbl_gnss_fix_quality {
  /** No position: searching, or the signal is lost. */
  PBL_GNSS_FIX_NONE = 0,
  /** Horizontal position only. */
  PBL_GNSS_FIX_2D,
  /** Horizontal position and altitude. */
  PBL_GNSS_FIX_3D,
  /** 3D fix with differential or SBAS corrections. */
  PBL_GNSS_FIX_DGNSS,
};

/** @brief One navigation epoch. Fields other than the quality and satellites are only valid with
 * a fix. */
struct pbl_gnss_fix {
  /** UTC time of the epoch in milliseconds since the epoch, or 0 when unknown. */
  uint64_t utc_ms;
  /** Latitude, in 1e-7 degrees, positive north. */
  int32_t lat_e7;
  /** Longitude, in 1e-7 degrees, positive east. */
  int32_t lon_e7;
  /** Altitude above mean sea level, in centimetres; valid with a 3D fix. */
  int32_t alt_cm;
  /** Ground speed, in millimetres per second. */
  uint32_t speed_mm_s;
  /** Course over ground, in hundredths of a degree clockwise from true north. */
  uint16_t course_cdeg;
  /** Horizontal dilution of precision, times 10; 0 when unknown. */
  uint16_t hdop_x10;
  /** Estimated horizontal accuracy (1 sigma), in decimetres; 0 when unknown. */
  uint16_t h_acc_dm;
  /** Satellites used in the solution. */
  uint8_t sats_used;
  /** Quality, an enum pbl_gnss_fix_quality. */
  uint8_t quality;
};

struct pbl_gnss;

/** @brief GNSS listener. */
struct pbl_gnss_listener {
  /** Optional. New epoch. */
  void (*fix)(const struct pbl_gnss *gnss, const struct pbl_gnss_fix *fix, void *user_data);
  /** Data passed to the callbacks. */
  void *user_data;
};

/** @brief GNSS driver operations, called with the device lock held. */
struct pbl_gnss_ops {
  /** Optional. Bring the hardware up, left powered down. */
  int (*init)(const struct pbl_gnss *gnss);
  /** Power the receiver up and report an epoch every @p interval_ms, which it may round. */
  int (*start)(const struct pbl_gnss *gnss, uint32_t interval_ms);
  /** Stop reporting and power the receiver down, keeping what speeds up the next fix. */
  int (*stop)(const struct pbl_gnss *gnss);
};

/** @brief GNSS runtime state. */
struct pbl_gnss_state {
  /** Device lock. */
  struct pbl_mutex lock;
  /** Listener, or NULL. */
  const struct pbl_gnss_listener *listener;
  /** Last epoch reported. */
  struct pbl_gnss_fix last;
  /** Epoch interval in milliseconds, 0 when stopped. */
  uint32_t interval_ms;
  /** An epoch was reported since the receiver started. */
  bool has_last;
};

/** @brief GNSS receiver. */
struct pbl_gnss {
  /** Device. */
  struct pbl_device dev;
  /** Driver operations. */
  const struct pbl_gnss_ops *ops;
  /** Runtime state. */
  struct pbl_gnss_state *state;
};

/** @brief The board's GNSS receiver. */
extern const struct pbl_gnss *const GNSS;

/**
 * @brief Define the class state of a GNSS receiver.
 * @param sym Symbol of the instance.
 */
#define PBL_GNSS_STATE_DEFINE(sym) \
  PBL_DEVICE_STATE_DEFINE(sym);    \
  static struct pbl_gnss_state sym##_gnss_state = {.lock = PBL_MUTEX_INITIALIZER}

/** @cond INTERNAL_HIDDEN */
extern const struct pbl_device_class pbl_gnss_class;
int pbl_gnss_dev_init(const struct pbl_device *dev);
/** @endcond */

/**
 * @brief Initialize a struct pbl_gnss.
 * @param sym Symbol of the instance, as passed to PBL_GNSS_STATE_DEFINE().
 * @param _name Device name.
 * @param _parent Parent device, such as the UART, or NULL.
 * @param _ops Driver operations.
 */
#define PBL_GNSS_INIT(sym, _name, _parent, _ops)                                           \
  {                                                                                        \
    .dev = PBL_DEVICE_INIT_CLASS(sym, _name, pbl_gnss_dev_init, _parent, &pbl_gnss_class), \
    .ops = (_ops),                                                                         \
    .state = &sym##_gnss_state,                                                            \
  }

/**
 * @brief Take the device lock.
 * @param gnss Receiver.
 */
void pbl_gnss_lock(const struct pbl_gnss *gnss);

/**
 * @brief Release the device lock.
 * @param gnss Receiver.
 */
void pbl_gnss_unlock(const struct pbl_gnss *gnss);

/**
 * @brief Set the listener.
 * @param gnss Receiver.
 * @param listener Listener, or NULL to remove it.
 * @return 0.
 */
int pbl_gnss_set_listener(const struct pbl_gnss *gnss, const struct pbl_gnss_listener *listener);

/**
 * @brief Start the receiver.
 *
 * Starting a running receiver changes its interval.
 *
 * @param gnss Receiver.
 * @param interval_ms Epoch interval in milliseconds, 1000 for 1 Hz.
 * @return 0, -ENODEV if the receiver is not ready, -EINVAL for a zero interval, or a negative
 *         errno.
 */
int pbl_gnss_start(const struct pbl_gnss *gnss, uint32_t interval_ms);

/**
 * @brief Stop the receiver.
 * @param gnss Receiver.
 * @return 0, -ENODEV if the receiver is not ready, or a negative errno.
 */
int pbl_gnss_stop(const struct pbl_gnss *gnss);

/**
 * @brief Check whether the receiver is started.
 * @param gnss Receiver.
 * @return true if started.
 */
bool pbl_gnss_is_running(const struct pbl_gnss *gnss);

/**
 * @brief Read the last epoch reported since the receiver started.
 * @param gnss Receiver.
 * @param[out] fix Epoch.
 * @return 0, -ENODEV if the receiver is not ready, or -ENODATA if nothing was reported yet.
 */
int pbl_gnss_peek(const struct pbl_gnss *gnss, struct pbl_gnss_fix *fix);

/**
 * @brief Report an epoch to the listener.
 *
 * For drivers, once per epoch while started. Must be called with the device lock held.
 *
 * @param gnss Receiver.
 * @param fix Epoch.
 */
void pbl_gnss_report_fix(const struct pbl_gnss *gnss, const struct pbl_gnss_fix *fix);

/** @} */
