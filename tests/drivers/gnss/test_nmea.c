/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "nmea.h"

#include <stdio.h>
#include <string.h>

#include <clar.h>

// A sentence with its checksum, from its body
static char s_line[NMEA_MAX_LEN + 8];

static char *prv_sentence(const char *body) {
  snprintf(s_line, sizeof(s_line), "$%s*%02X", body, nmea_checksum(body));
  return s_line;
}

static struct nmea_epoch s_epoch;
static struct nmea_epoch s_done;
static int s_ended;

// Checks, splits and adds a sentence to the epoch
static void prv_add(const char *body) {
  char *fields[NMEA_MAX_FIELDS];
  char *line = prv_sentence(body);
  int n;

  cl_assert(nmea_checksum_ok(line));
  n = nmea_split(line, fields, NMEA_MAX_FIELDS);
  cl_assert(n > 0);
  if (nmea_epoch_add(&s_epoch, fields, n, &s_done)) {
    s_ended++;
  }
}

static void prv_end(void) {
  cl_assert(nmea_epoch_end(&s_epoch, &s_done));
  s_ended++;
}

void test_nmea__initialize(void) {
  nmea_epoch_init(&s_epoch);
  memset(&s_done, 0, sizeof(s_done));
  s_ended = 0;
}

// Sentences //////////////////////////////////////////////////////////////////////////////////////

void test_nmea__checksum(void) {
  char good[] = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";
  char lower[] = "$PAIR001,002,0*39";
  char bad[] = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*48";
  char missing[] = "$GPGGA,123519,4807.038,N";
  char short_sum[] = "$PAIR001,002,0*3";
  char trailing[] = "$PAIR001,002,0*39x";
  char no_dollar[] = "PAIR001,002,0*39";

  cl_assert(nmea_checksum_ok(good));
  cl_assert(nmea_checksum_ok(lower));
  cl_assert(!nmea_checksum_ok(bad));
  cl_assert(!nmea_checksum_ok(missing));
  cl_assert(!nmea_checksum_ok(short_sum));
  cl_assert(!nmea_checksum_ok(trailing));
  cl_assert(!nmea_checksum_ok(no_dollar));
  cl_assert_equal_i(nmea_checksum("PAIR002"), 0x38);
}

void test_nmea__split(void) {
  char line[] = "$GNRMC,,V,,,,,,,,,,N,V*37";
  char *fields[NMEA_MAX_FIELDS];
  char *few[4];
  char copy[sizeof(line)];

  memcpy(copy, line, sizeof(line));
  cl_assert_equal_i(nmea_split(line, fields, NMEA_MAX_FIELDS), 14);
  cl_assert_equal_s(fields[0], "GNRMC");
  cl_assert_equal_s(fields[1], "");
  cl_assert_equal_s(fields[2], "V");
  cl_assert_equal_s(fields[12], "N");
  cl_assert_equal_s(fields[13], "V");

  cl_assert_equal_i(nmea_split(copy, few, 4), -1);
}

void test_nmea__types(void) {
  cl_assert_equal_i(nmea_type_of("GNGGA"), NMEA_GGA);
  cl_assert_equal_i(nmea_type_of("GPRMC"), NMEA_RMC);
  cl_assert_equal_i(nmea_type_of("GNGSA"), NMEA_GSA);
  cl_assert_equal_i(nmea_type_of("GBGSV"), NMEA_GSV);
  cl_assert_equal_i(nmea_type_of("PAIR001"), NMEA_PAIR);
  cl_assert_equal_i(nmea_type_of("GNVTG"), NMEA_OTHER);
  cl_assert_equal_i(nmea_type_of("PAIR"), NMEA_OTHER);
  cl_assert_equal_i(nmea_type_of("GGA"), NMEA_OTHER);
}

// Epochs /////////////////////////////////////////////////////////////////////////////////////////

// The classic example: 48 deg 07.038' N, 11 deg 31.000' E, its date moved to 23 March 2026, at
// 12:35:19
void test_nmea__fix_3d(void) {
  prv_add("GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230326,003.1,W");
  prv_add("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,");
  prv_add("GPGSA,A,3,04,05,,09,12,,,24,,,,,2.5,1.3,2.1");
  cl_assert_equal_i(s_ended, 0);
  prv_end();

  cl_assert_equal_i(s_done.fix.quality, PBL_GNSS_FIX_3D);
  cl_assert_equal_i(s_done.fix.lat_e7, 481173000);
  cl_assert_equal_i(s_done.fix.lon_e7, 115166667);
  cl_assert_equal_i(s_done.fix.alt_cm, 54540);
  cl_assert_equal_i(s_done.fix.sats_used, 8);
  cl_assert_equal_i(s_done.fix.hdop_x10, 9);
  // 22.4 knots
  cl_assert_equal_i(s_done.fix.speed_mm_s, 11524);
  cl_assert_equal_i(s_done.fix.course_cdeg, 8440);
  // 2026-03-23T12:35:19Z
  cl_assert_equal_i(s_done.fix.utc_ms / 1000U, 1774269319);
}

void test_nmea__southern_western_hemispheres(void) {
  prv_add("GNGGA,000001.000,3355.0000,S,01825.5000,W,1,05,1.4,10.0,M,30.0,M,,");
  prv_end();

  cl_assert_equal_i(s_done.fix.lat_e7, -339166667);
  cl_assert_equal_i(s_done.fix.lon_e7, -184250000);
  // No GSA: an altitude makes it 3D
  cl_assert_equal_i(s_done.fix.quality, PBL_GNSS_FIX_3D);
}

void test_nmea__fix_2d_from_gsa(void) {
  prv_add("GNGGA,101010.000,5130.0000,N,00007.5000,W,1,03,4.0,,M,,M,,");
  prv_add("GNGSA,A,2,10,12,15,,,,,,,,,,4.2,4.0,1.0,1");
  prv_end();

  cl_assert_equal_i(s_done.fix.quality, PBL_GNSS_FIX_2D);
  cl_assert_equal_i(s_done.fix.alt_cm, 0);
  cl_assert_equal_i(s_done.fix.lat_e7, 515000000);
}

void test_nmea__differential(void) {
  prv_add("GNGGA,101010.000,5130.0000,N,00007.5000,W,2,12,0.7,35.0,M,47.0,M,1.0,0000");
  prv_end();

  cl_assert_equal_i(s_done.fix.quality, PBL_GNSS_FIX_DGNSS);
}

// What the receiver says while it searches: no position, nor time
void test_nmea__searching(void) {
  prv_add("GNRMC,,V,,,,,,,,,,N,V");
  prv_add("GNGGA,,,,,,0,0,,,M,,M,,");
  prv_add("GNGSA,A,1,,,,,,,,,,,,,,,,1");
  prv_add("GPGSV,1,1,0,1");
  prv_end();

  cl_assert_equal_i(s_done.fix.quality, PBL_GNSS_FIX_NONE);
  cl_assert_equal_i(s_done.fix.utc_ms, 0);
  cl_assert_equal_i(s_done.fix.sats_used, 0);
  cl_assert_equal_i(s_done.fix.sats_in_view, 0);
  cl_assert_equal_i(s_done.sentences, 4);
}

// Time before a fix: the date and time are kept, the position is not
void test_nmea__time_without_fix(void) {
  prv_add("GNRMC,184501.000,V,5130.0000,N,00007.5000,W,0.00,0.00,271126,,,N,V");
  prv_add("GNGGA,184501.000,,,,,0,3,,,M,,M,,");
  prv_end();

  cl_assert_equal_i(s_done.fix.quality, PBL_GNSS_FIX_NONE);
  cl_assert_equal_i(s_done.fix.lat_e7, 0);
  cl_assert_equal_i(s_done.fix.sats_used, 3);
  // 2026-11-27T18:45:01Z
  cl_assert_equal_i(s_done.fix.utc_ms / 1000U, 1795805101);
}

void test_nmea__dead_reckoning_is_no_fix(void) {
  prv_add("GNRMC,120000.000,A,5130.0000,N,00007.5000,W,5.0,90.0,271126,,,E,V");
  prv_add("GNGGA,120000.000,5130.0000,N,00007.5000,W,6,00,,35.0,M,47.0,M,,");
  prv_end();

  cl_assert_equal_i(s_done.fix.quality, PBL_GNSS_FIX_NONE);
  cl_assert_equal_i(s_done.fix.speed_mm_s, 0);
}

void test_nmea__rmc_alone(void) {
  prv_add("GNRMC,120000.000,A,5130.0000,N,00007.5000,W,1.0,270.5,271126,,,A,V");
  prv_end();

  // Without GGA nor GSA, no altitude: 2D
  cl_assert_equal_i(s_done.fix.quality, PBL_GNSS_FIX_2D);
  cl_assert_equal_i(s_done.fix.lon_e7, -1250000);
  cl_assert_equal_i(s_done.fix.speed_mm_s, 514);
  cl_assert_equal_i(s_done.fix.course_cdeg, 27050);
}

// Satellites: counted per constellation, each once whatever its signals, the strongest kept
void test_nmea__satellites(void) {
  prv_add("GPGSV,3,1,10,01,40,083,46,02,17,308,41,12,07,344,39,14,22,228,,1");
  prv_add("GPGSV,3,2,10,15,61,177,30,17,10,103,,19,37,293,,24,24,045,,1");
  prv_add("GPGSV,3,3,10,25,01,138,,32,58,066,25,1");
  // L5 of some of them: no new satellites, and only 4 of them listed
  prv_add("GPGSV,1,1,04,01,40,083,40,24,24,045,38,25,01,138,,32,58,066,20,8");
  prv_add("GLGSV,1,1,02,65,30,040,33,66,50,120,,1");
  prv_add("GAGSV,1,1,03,04,40,220,28,09,10,310,,11,70,020,,7");
  prv_add("GBGSV,1,1,01,08,22,155,48,1");
  prv_end();

  cl_assert_equal_i(s_done.sky.in_view[NMEA_SYS_GPS], 10);
  cl_assert_equal_i(s_done.sky.tracked[NMEA_SYS_GPS], 6);
  cl_assert_equal_i(s_done.sky.in_view[NMEA_SYS_GLONASS], 2);
  cl_assert_equal_i(s_done.sky.tracked[NMEA_SYS_GLONASS], 1);
  cl_assert_equal_i(s_done.sky.in_view[NMEA_SYS_GALILEO], 3);
  cl_assert_equal_i(s_done.sky.tracked[NMEA_SYS_GALILEO], 1);
  cl_assert_equal_i(s_done.sky.tracked[NMEA_SYS_BEIDOU], 1);
  cl_assert_equal_i(s_done.sky.best_cn0, 48);
  cl_assert_equal_i(nmea_sky_in_view(&s_done.sky), 16);
  cl_assert_equal_i(nmea_sky_tracked(&s_done.sky), 9);
  cl_assert_equal_i(s_done.fix.sats_in_view, 16);
}

// The next epoch's time ends the last one, at 10 Hz where the line never goes quiet
void test_nmea__time_ends_the_epoch(void) {
  prv_add("GNRMC,120000.000,A,5130.0000,N,00007.5000,W,0.0,0.0,271126,,,A,V");
  prv_add("GNGGA,120000.000,5130.0000,N,00007.5000,W,1,07,1.0,35.0,M,47.0,M,,");
  prv_add("GPGSV,1,1,01,01,40,083,46,1");
  cl_assert_equal_i(s_ended, 0);

  prv_add("GNRMC,120000.100,A,5130.0010,N,00007.5000,W,0.0,0.0,271126,,,A,V");
  cl_assert_equal_i(s_ended, 1);
  cl_assert_equal_i(s_done.fix.lat_e7, 515000000);
  cl_assert_equal_i(s_done.sky.tracked[NMEA_SYS_GPS], 1);

  prv_add("GNGGA,120000.100,5130.0010,N,00007.5000,W,1,07,1.0,35.0,M,47.0,M,,");
  prv_end();
  cl_assert_equal_i(s_ended, 2);
  cl_assert_equal_i(s_done.fix.lat_e7, 515000167);
  // The new epoch started without the last one's satellites
  cl_assert_equal_i(s_done.sky.tracked[NMEA_SYS_GPS], 0);
}

void test_nmea__empty_epoch_does_not_end(void) {
  cl_assert(!nmea_epoch_end(&s_epoch, &s_done));
  prv_add("GNVTG,,T,,M,,N,,K,N");
  cl_assert(!nmea_epoch_end(&s_epoch, &s_done));
}

void test_nmea__garbage_fields(void) {
  prv_add("GNGGA,99x999,48A7.038,N,01131.000,Q,1,0x,abc,,M,,M,,");
  prv_add("GNRMC,250000,A,4807.038,N,01131.000,E,-1,999.9,320394,,,A,V");
  prv_end();

  // Neither position parses, nor the time
  cl_assert_equal_i(s_done.fix.quality, PBL_GNSS_FIX_NONE);
  cl_assert_equal_i(s_done.fix.utc_ms, 0);
  cl_assert_equal_i(s_done.fix.hdop_x10, 0);
}

void test_nmea__short_sentences_are_ignored(void) {
  prv_add("GNGGA,120000.000");
  prv_add("GNRMC");
  prv_add("GPGSV,1");
  prv_end();

  cl_assert_equal_i(s_done.fix.quality, PBL_GNSS_FIX_NONE);
  cl_assert_equal_i(s_done.sky.in_view[NMEA_SYS_GPS], 0);
}

// Before it knows the time, the AG3335 counts from 1980-01-06
void test_nmea__default_date_is_no_time(void) {
  prv_add("GNRMC,000010.000,V,,,,,0.00,0.00,060180,,,N,V");
  prv_end();

  cl_assert_equal_i(s_done.fix.utc_ms, 0);
}
