/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pbl/drivers/gnss.h>

/*
 * NMEA 0183 (4.10 and 4.11) for GNSS drivers: sentences checked and split in place, and the
 * sentences of a navigation epoch (GGA, RMC, GSA, GSV) gathered into one fix. Everything is in
 * integers: positions in 1e-7 degrees, as struct pbl_gnss_fix has them.
 */

/** Longest sentence kept, '$' to the checksum; NMEA's are 82 characters with CR LF. */
#define NMEA_MAX_LEN 160U

/** Most fields in a sentence. */
#define NMEA_MAX_FIELDS 24U

/** Sentence types the gatherer reads, and the others. */
enum nmea_type {
  NMEA_OTHER,
  NMEA_GGA,
  NMEA_RMC,
  NMEA_GSA,
  NMEA_GSV,
  /** A proprietary sentence of the Airoha receivers, $PAIRnnn. */
  NMEA_PAIR,
  NMEA_TYPE_COUNT,
};

/** Constellations, by the talker of their GSV sentences. */
enum nmea_system {
  NMEA_SYS_GPS,
  NMEA_SYS_GLONASS,
  NMEA_SYS_GALILEO,
  NMEA_SYS_BEIDOU,
  NMEA_SYS_QZSS,
  NMEA_SYS_NAVIC,
  NMEA_SYS_COUNT,
};

/** Satellites of an epoch, from its GSV sentences. */
struct nmea_sky {
  /** Satellites in view, per constellation: the most any of its signals lists. */
  uint8_t in_view[NMEA_SYS_COUNT];
  /** Satellites with a signal (a C/N0), per constellation. */
  uint8_t tracked[NMEA_SYS_COUNT];
  /** Strongest C/N0, in dB-Hz, 0 for none. */
  uint8_t best_cn0;
};

/** An epoch, as gathered. */
struct nmea_epoch {
  /** What the epoch gives. */
  struct pbl_gnss_fix fix;
  /** Its satellites. */
  struct nmea_sky sky;
  /** Time of day of its GGA or RMC, in milliseconds, or UINT32_MAX. */
  uint32_t time_ms;
  /** Sentences in it. */
  uint16_t sentences;
  /** GGA quality, GSA fix type, RMC status and mode, 0 until seen. */
  uint8_t gga_quality;
  uint8_t gsa_fix;
  char rmc_status;
  char rmc_mode;
  bool has_gga;
  bool has_rmc;
  bool gga_alt;
  bool gga_pos;
  bool rmc_pos;
  /** PRNs with a signal per constellation, so that each counts once whatever its signals. */
  uint32_t seen[NMEA_SYS_COUNT][8];
};

/**
 * Check the checksum of a sentence, '$' to its two hex digits, without CR LF.
 *
 * @param line Sentence, NUL-terminated.
 * @return true if it has a valid checksum.
 */
bool nmea_checksum_ok(const char *line);

/**
 * Checksum of the characters between '$' and '*', as a sentence sent ends with.
 *
 * @param body Characters after '$', up to the end or a '*'.
 * @return Checksum.
 */
uint8_t nmea_checksum(const char *body);

/**
 * Split a checked sentence in place into its fields, the address first, without '$' or the
 * checksum.
 *
 * @param line Sentence, checked with nmea_checksum_ok().
 * @param[out] fields Fields.
 * @param max Capacity of @p fields.
 * @return Number of fields, or -1 if there are more than @p max.
 */
int nmea_split(char *line, char **fields, size_t max);

/**
 * Type of a sentence, from its address.
 *
 * @param address First field, as "GNGGA" or "PAIR001".
 * @return Type.
 */
enum nmea_type nmea_type_of(const char *address);

/** Start an empty epoch. */
void nmea_epoch_init(struct nmea_epoch *ep);

/**
 * Add a sentence to the epoch.
 *
 * A GGA or RMC of another time than the epoch's ends it first: the epoch gathered so far is
 * moved to @p done, and the sentence starts the next one.
 *
 * @param ep Epoch.
 * @param fields Fields of the sentence, from nmea_split().
 * @param count Number of fields.
 * @param[out] done The epoch ended, if any.
 * @return true if an epoch ended into @p done.
 */
bool nmea_epoch_add(struct nmea_epoch *ep, char **fields, int count, struct nmea_epoch *done);

/**
 * End the epoch, as when the receiver has gone quiet after it.
 *
 * @param ep Epoch, started again empty.
 * @param[out] done The epoch, if it had any sentence.
 * @return true if there was an epoch.
 */
bool nmea_epoch_end(struct nmea_epoch *ep, struct nmea_epoch *done);

/**
 * Satellites in view in an epoch, over all constellations.
 *
 * @param sky Satellites.
 * @return Number in view.
 */
unsigned int nmea_sky_in_view(const struct nmea_sky *sky);

/**
 * Satellites with a signal in an epoch, over all constellations.
 *
 * @param sky Satellites.
 * @return Number tracked.
 */
unsigned int nmea_sky_tracked(const struct nmea_sky *sky);
