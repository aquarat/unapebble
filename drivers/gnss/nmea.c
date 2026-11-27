/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "nmea.h"

#include <string.h>

#define MS_PER_DAY 86400000U
// 2020-01-01: dates before it are the receiver's default, not the time
#define NMEA_MIN_DAYS 18262

static int prv_hex(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return -1;
}

uint8_t nmea_checksum(const char *body) {
  uint8_t sum = 0U;

  for (; *body != '\0' && *body != '*'; body++) {
    sum ^= (uint8_t)*body;
  }

  return sum;
}

bool nmea_checksum_ok(const char *line) {
  const char *star;
  int hi;
  int lo;

  if (line[0] != '$') {
    return false;
  }

  star = strchr(line, '*');
  if (star == nullptr || star[1] == '\0' || star[2] == '\0' || star[3] != '\0') {
    return false;
  }

  hi = prv_hex(star[1]);
  lo = prv_hex(star[2]);
  if (hi < 0 || lo < 0) {
    return false;
  }

  return nmea_checksum(line + 1) == (uint8_t)((hi << 4) | lo);
}

int nmea_split(char *line, char **fields, size_t max) {
  char *p = line + 1;
  size_t n = 0U;

  if (max == 0U) {
    return -1;
  }

  fields[n++] = p;
  for (; *p != '\0'; p++) {
    if (*p == '*') {
      *p = '\0';
      break;
    }
    if (*p == ',') {
      if (n == max) {
        return -1;
      }
      *p = '\0';
      fields[n++] = p + 1;
    }
  }

  return (int)n;
}

enum nmea_type nmea_type_of(const char *address) {
  size_t len = strlen(address);

  if (len == 7U && strncmp(address, "PAIR", 4U) == 0) {
    return NMEA_PAIR;
  }
  if (len != 5U) {
    return NMEA_OTHER;
  }
  if (strcmp(address + 2, "GGA") == 0) {
    return NMEA_GGA;
  }
  if (strcmp(address + 2, "RMC") == 0) {
    return NMEA_RMC;
  }
  if (strcmp(address + 2, "GSA") == 0) {
    return NMEA_GSA;
  }
  if (strcmp(address + 2, "GSV") == 0) {
    return NMEA_GSV;
  }

  return NMEA_OTHER;
}

// A talker's constellation, or -1 for a combined or unknown one
static int prv_system(const char *address) {
  static const struct {
    char talker[3];
    uint8_t sys;
  } s_talkers[] = {
    {"GP", NMEA_SYS_GPS},    {"GL", NMEA_SYS_GLONASS}, {"GA", NMEA_SYS_GALILEO},
    {"GB", NMEA_SYS_BEIDOU}, {"BD", NMEA_SYS_BEIDOU},  {"GQ", NMEA_SYS_QZSS},
    {"QZ", NMEA_SYS_QZSS},   {"GI", NMEA_SYS_NAVIC},
  };

  for (size_t i = 0U; i < sizeof(s_talkers) / sizeof(s_talkers[0]); i++) {
    if (strncmp(address, s_talkers[i].talker, 2U) == 0) {
      return s_talkers[i].sys;
    }
  }

  return -1;
}

static bool prv_uint(const char *s, uint32_t *value) {
  uint32_t v = 0U;

  if (*s == '\0') {
    return false;
  }

  for (; *s != '\0'; s++) {
    if (*s < '0' || *s > '9' || v > (UINT32_MAX - 9U) / 10U) {
      return false;
    }
    v = v * 10U + (uint32_t)(*s - '0');
  }

  *value = v;

  return true;
}

// A decimal number, as an integer of @p decimals places, the digits past them dropped
static bool prv_fixed(const char *s, unsigned int decimals, int64_t *value) {
  bool neg = false;
  bool digits = false;
  int64_t v = 0;
  int frac = -1;

  if (*s == '-' || *s == '+') {
    neg = *s == '-';
    s++;
  }

  for (; *s != '\0'; s++) {
    if (*s == '.' && frac < 0) {
      frac = 0;
      continue;
    }
    if (*s < '0' || *s > '9') {
      return false;
    }
    digits = true;
    if (frac >= (int)decimals) {
      continue;
    }
    if (v > INT64_MAX / 100) {
      return false;
    }
    v = v * 10 + (*s - '0');
    if (frac >= 0) {
      frac++;
    }
  }

  if (!digits) {
    return false;
  }

  for (int i = frac < 0 ? 0 : frac; i < (int)decimals; i++) {
    v *= 10;
  }

  *value = neg ? -v : v;

  return true;
}

// hhmmss.sss to milliseconds of the day
static bool prv_time(const char *s, uint32_t *ms) {
  int64_t v;
  uint32_t hh;
  uint32_t mm;
  uint32_t ss_ms;

  if (strlen(s) < 6U || !prv_fixed(s, 3U, &v) || v < 0) {
    return false;
  }

  hh = (uint32_t)(v / 10000000);
  mm = (uint32_t)(v / 100000 % 100);
  ss_ms = (uint32_t)(v % 100000);
  if (hh > 23U || mm > 59U || ss_ms > 60999U) {
    return false;
  }

  *ms = (hh * 3600U + mm * 60U) * 1000U + ss_ms;

  return true;
}

// Days from 1970-01-01 to a date of the proleptic Gregorian calendar
static int64_t prv_days(int64_t y, uint32_t m, uint32_t d) {
  int64_t era;
  uint32_t yoe;
  uint32_t doy;
  uint32_t doe;

  y -= m <= 2U ? 1 : 0;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = (uint32_t)(y - era * 400);
  doy = (153U * (m > 2U ? m - 3U : m + 9U) + 2U) / 5U + d - 1U;
  doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;

  return era * 146097 + (int64_t)doe - 719468;
}

// ddmmyy to days since 1970, the years 80 to 99 in the 1900s
static bool prv_date(const char *s, int64_t *days) {
  uint32_t v;
  uint32_t d;
  uint32_t m;
  uint32_t y;

  if (strlen(s) != 6U || !prv_uint(s, &v)) {
    return false;
  }

  d = v / 10000U;
  m = v / 100U % 100U;
  y = v % 100U;
  if (d < 1U || d > 31U || m < 1U || m > 12U) {
    return false;
  }

  *days = prv_days(y >= 80U ? 1900 + y : 2000 + y, m, d);

  return true;
}

// ddmm.mmmm or dddmm.mmmm with its hemisphere to 1e-7 degrees
static bool prv_coord(const char *s, const char *hemi, int32_t *e7) {
  int64_t v;
  int64_t deg;
  int64_t min_e7;
  int64_t out;

  if (!prv_fixed(s, 7U, &v) || v < 0) {
    return false;
  }

  deg = v / 1000000000;
  min_e7 = v % 1000000000;
  out = deg * 10000000 + (min_e7 + 30) / 60;

  switch (hemi[0]) {
    case 'N':
    case 'E':
      break;
    case 'S':
    case 'W':
      out = -out;
      break;
    default:
      return false;
  }

  if (out > 1800000000 || out < -1800000000) {
    return false;
  }

  *e7 = (int32_t)out;

  return true;
}

// A position of the GGA or RMC fields at lat, N/S, lon, E/W
static bool prv_position(char **f, int lat, int32_t *lat_e7, int32_t *lon_e7) {
  int32_t la;
  int32_t lo;

  if (!prv_coord(f[lat], f[lat + 1], &la) || !prv_coord(f[lat + 2], f[lat + 3], &lo)) {
    return false;
  }
  if (la > 900000000 || la < -900000000) {
    return false;
  }

  *lat_e7 = la;
  *lon_e7 = lo;

  return true;
}

static uint16_t prv_x10(const char *s) {
  int64_t v;

  if (!prv_fixed(s, 2U, &v) || v < 0) {
    return 0U;
  }
  v = (v + 5) / 10;

  return v > UINT16_MAX ? UINT16_MAX : (uint16_t)v;
}

// GGA: time, lat, N/S, lon, E/W, quality, satellites, HDOP, altitude, M, separation, M, ...
static void prv_gga(struct nmea_epoch *ep, char **f, int n) {
  uint32_t v;
  int64_t alt;

  if (n < 10) {
    return;
  }

  ep->has_gga = true;
  if (prv_uint(f[6], &v)) {
    ep->gga_quality = (uint8_t)(v > 9U ? 9U : v);
  }
  if (prv_uint(f[7], &v)) {
    ep->fix.sats_used = (uint8_t)(v > UINT8_MAX ? UINT8_MAX : v);
  }
  ep->fix.hdop_x10 = prv_x10(f[8]);
  ep->gga_pos = prv_position(f, 2, &ep->fix.lat_e7, &ep->fix.lon_e7);
  ep->gga_alt = prv_fixed(f[9], 2U, &alt) && alt > INT32_MIN && alt < INT32_MAX;
  if (ep->gga_alt) {
    ep->fix.alt_cm = (int32_t)alt;
  }
}

// RMC: time, status, lat, N/S, lon, E/W, speed (knots), course, date, variation, E/W, mode, ...
static void prv_rmc(struct nmea_epoch *ep, char **f, int n, uint32_t time_ms, bool has_time) {
  int64_t v;
  int64_t days;

  if (n < 10) {
    return;
  }

  ep->has_rmc = true;
  ep->rmc_status = f[2][0];
  ep->rmc_mode = n > 12 ? f[12][0] : '\0';
  if (!ep->gga_pos) {
    ep->rmc_pos = prv_position(f, 3, &ep->fix.lat_e7, &ep->fix.lon_e7);
  }
  if (prv_fixed(f[7], 3U, &v) && v >= 0) {
    // A knot is 514.444 mm/s
    v = (v * 514444 + 500000) / 1000000;
    ep->fix.speed_mm_s = v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
  }
  if (prv_fixed(f[8], 2U, &v) && v >= 0 && v < 36000) {
    ep->fix.course_cdeg = (uint16_t)v;
  }
  // Before it knows the time, the receiver counts from the start of GPS time, 1980-01-06
  if (has_time && prv_date(f[9], &days) && days >= NMEA_MIN_DAYS) {
    ep->fix.utc_ms = (uint64_t)days * MS_PER_DAY + time_ms;
  }
}

// GSA: mode, fix type (1 none, 2 2D, 3 3D), 12 PRNs, PDOP, HDOP, VDOP, system
static void prv_gsa(struct nmea_epoch *ep, char **f, int n) {
  uint32_t v;

  if (n < 3 || !prv_uint(f[2], &v) || v > 3U) {
    return;
  }
  if (v > ep->gsa_fix) {
    ep->gsa_fix = (uint8_t)v;
  }
  if (ep->fix.hdop_x10 == 0U && n > 16) {
    ep->fix.hdop_x10 = prv_x10(f[16]);
  }
}

// GSV: messages, number, in view, then PRN, elevation, azimuth, C/N0 of up to four, and in 4.11
// the signal
static void prv_gsv(struct nmea_epoch *ep, char **f, int n) {
  int sys = prv_system(f[0]);
  uint32_t in_view;

  if (sys < 0 || n < 4 || !prv_uint(f[3], &in_view)) {
    return;
  }

  if (in_view > ep->sky.in_view[sys]) {
    ep->sky.in_view[sys] = (uint8_t)(in_view > UINT8_MAX ? UINT8_MAX : in_view);
  }

  for (int i = 4; i + 3 < n; i += 4) {
    uint32_t prn;
    uint32_t cn0;

    if (!prv_uint(f[i], &prn) || prn > 255U || !prv_uint(f[i + 3], &cn0) || cn0 == 0U) {
      continue;
    }

    if ((ep->seen[sys][prn / 32U] & (1U << (prn % 32U))) == 0U) {
      ep->seen[sys][prn / 32U] |= 1U << (prn % 32U);
      if (ep->sky.tracked[sys] < UINT8_MAX) {
        ep->sky.tracked[sys]++;
      }
    }
    if (cn0 > ep->sky.best_cn0) {
      ep->sky.best_cn0 = (uint8_t)(cn0 > UINT8_MAX ? UINT8_MAX : cn0);
    }
  }
}

// What the sentences say of the fix, once they are all in
static void prv_finish(struct nmea_epoch *ep) {
  struct pbl_gnss_fix *fix = &ep->fix;
  uint8_t quality = PBL_GNSS_FIX_NONE;

  if (ep->has_gga) {
    // 1 GNSS, 2 differential, 3 PPS, 4 and 5 RTK; 6 dead reckoning, 7 manual, 8 simulated
    switch (ep->gga_quality) {
      case 1:
      case 3:
      case 4:
      case 5:
        quality = PBL_GNSS_FIX_3D;
        break;
      case 2:
        quality = PBL_GNSS_FIX_DGNSS;
        break;
      default:
        break;
    }
    if (!ep->gga_pos) {
      quality = PBL_GNSS_FIX_NONE;
    }
  } else if (ep->has_rmc && ep->rmc_status == 'A' && ep->rmc_pos) {
    quality = ep->rmc_mode == 'D' ? PBL_GNSS_FIX_DGNSS : PBL_GNSS_FIX_3D;
  }

  // The GSA says 2D or 3D; without one, an altitude makes it 3D
  if (quality == PBL_GNSS_FIX_3D) {
    if (ep->gsa_fix == 2U || (ep->gsa_fix == 0U && !ep->gga_alt)) {
      quality = PBL_GNSS_FIX_2D;
    }
  }

  // Dead reckoning, or the RMC saying the data is not valid
  if (ep->has_rmc && (ep->rmc_status == 'V' || ep->rmc_mode == 'E' || ep->rmc_mode == 'N')) {
    quality = PBL_GNSS_FIX_NONE;
  }

  fix->quality = quality;
  if (quality == PBL_GNSS_FIX_NONE) {
    fix->lat_e7 = 0;
    fix->lon_e7 = 0;
    fix->alt_cm = 0;
    fix->speed_mm_s = 0U;
    fix->course_cdeg = 0U;
  } else if (quality == PBL_GNSS_FIX_2D) {
    fix->alt_cm = 0;
  }

  fix->sats_in_view =
      (uint8_t)(nmea_sky_in_view(&ep->sky) > UINT8_MAX ? UINT8_MAX : nmea_sky_in_view(&ep->sky));
}

void nmea_epoch_init(struct nmea_epoch *ep) {
  *ep = (struct nmea_epoch){.time_ms = UINT32_MAX};
}

bool nmea_epoch_end(struct nmea_epoch *ep, struct nmea_epoch *done) {
  bool any = ep->sentences > 0U;

  if (any) {
    prv_finish(ep);
    *done = *ep;
  }
  nmea_epoch_init(ep);

  return any;
}

bool nmea_epoch_add(struct nmea_epoch *ep, char **fields, int count, struct nmea_epoch *done) {
  enum nmea_type type = nmea_type_of(fields[0]);
  bool ended = false;
  bool has_time = false;
  uint32_t time_ms = 0U;

  if (type == NMEA_GGA || type == NMEA_RMC) {
    has_time = count > 1 && prv_time(fields[1], &time_ms);
    // Another epoch's time, or a second GGA or RMC
    if ((has_time && ep->time_ms != UINT32_MAX && ep->time_ms != time_ms) ||
        (type == NMEA_GGA && ep->has_gga) || (type == NMEA_RMC && ep->has_rmc)) {
      ended = nmea_epoch_end(ep, done);
    }
    if (has_time) {
      ep->time_ms = time_ms;
    }
  }

  switch (type) {
    case NMEA_GGA:
      prv_gga(ep, fields, count);
      break;
    case NMEA_RMC:
      prv_rmc(ep, fields, count, time_ms, has_time);
      break;
    case NMEA_GSA:
      prv_gsa(ep, fields, count);
      break;
    case NMEA_GSV:
      prv_gsv(ep, fields, count);
      break;
    default:
      return ended;
  }

  ep->sentences++;

  return ended;
}

unsigned int nmea_sky_in_view(const struct nmea_sky *sky) {
  unsigned int n = 0U;

  for (size_t i = 0U; i < NMEA_SYS_COUNT; i++) {
    n += sky->in_view[i];
  }

  return n;
}

unsigned int nmea_sky_tracked(const struct nmea_sky *sky) {
  unsigned int n = 0U;

  for (size_t i = 0U; i < NMEA_SYS_COUNT; i++) {
    n += sky->tracked[i];
  }

  return n;
}
