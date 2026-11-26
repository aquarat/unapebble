/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>

#include <pbl/drivers/gnss.h>

/**
 * @defgroup drivers_gnss_sim Simulator
 * @ingroup drivers_gnss
 * @brief Simulated GNSS receiver.
 *
 * Once started, it reports CONFIG_GNSS_SIM_TIME_TO_FIX epochs without a position, then replays a
 * built-in route: a hilly loop of about 3.1 km at running pace, with a stop, looped forever.
 * Tests can take the signal away or report fixes of their own instead.
 * @{
 */

/** @cond INTERNAL_HIDDEN */
extern const struct pbl_gnss pbl_gnss_sim;
/** @endcond */

/** @brief The simulated receiver. */
#define SIM_GNSS (&pbl_gnss_sim)

/**
 * @brief Take the signal away or give it back.
 *
 * Without signal, epochs have no position; the route keeps going meanwhile.
 *
 * @param gnss Device.
 * @param available false for no signal.
 */
void pbl_gnss_sim_set_signal(const struct pbl_gnss *gnss, bool available);

/**
 * @brief Report a fix of the caller's every epoch instead of the route.
 *
 * Time and quality are reported as given.
 *
 * @param gnss Device.
 * @param fix Fix, or NULL to go back to the route.
 */
void pbl_gnss_sim_set_fix(const struct pbl_gnss *gnss, const struct pbl_gnss_fix *fix);

/**
 * @brief Restart the route from its start, searching again.
 *
 * @param gnss Device.
 */
void pbl_gnss_sim_reset(const struct pbl_gnss *gnss);

/** @} */
