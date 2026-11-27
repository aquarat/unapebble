/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

// The battery capacity test's state machine and arithmetic, apart from the storage and the
// battery service so the unit tests can check them. Charge is counted out of the battery:
// positive while discharging, from the last sample at full.

#include <stdbool.h>
#include <stdint.h>

#include <pbl/drivers/gauge.h>

// Samples further apart than this are a gap (the watch off, or a clock change): the software
// integral bridges it with the gauge's counter
#define BT_GAP_S 600

enum bt_phase {
  BT_IDLE,
  BT_CHARGE,
  BT_FULL,
  BT_DISCHARGE,
  BT_RECHARGE,
  BT_DONE,
};

// Sample flags, in the records too
#define BT_S_PLUGGED 0x01U
#define BT_S_FULL    0x02U // the charger ended its charge
#define BT_S_EMPTY   0x04U // the battery service has the battery empty: it shuts down
#define BT_S_HEAVY   0x08U // the heavy load is on
#define BT_S_LOST    0x10U // records only: the counter started again, the integral stands in

// Loads seen during the discharge
#define BT_LOAD_NORMAL 0x01U
#define BT_LOAD_HEAVY  0x02U

// Test flags
#define BT_F_COMPLETE 0x01U // the discharge ran to the low battery shutdown
#define BT_F_LOST     0x02U // the counter started again during the test
#define BT_F_HEAVY    0x04U // the user chose the heavy load

struct bt_sample {
  uint32_t time;
  uint8_t flags;
  struct pbl_gauge_reading r;
};

//! A log record, 36 bytes
struct bt_record {
  uint32_t time;
  int32_t out_uah; // counter: charge out since full
  int32_t sw_uah;  // software integral of the averaged current: the same
  uint16_t mv;
  uint16_t avg_mv;
  int16_t i_10ua; // positive while discharging
  int16_t avg_i_10ua;
  uint16_t soc_cpct;
  uint16_t remaining_dmah; // RepCap, 0.1 mAh
  uint16_t full_dmah;      // FullCapRep, 0.1 mAh
  uint16_t counter;        // QH, raw
  int16_t temp_dc;         // 0.1 degree C
  uint8_t phase;
  uint8_t flags;
  uint8_t loads;
  int8_t tmin_c; // of the phase
  int8_t tmax_c;
  uint8_t resets;
};

//! The gauge at a full charge
struct bt_full {
  uint32_t time;
  uint32_t full_uah;
  uint32_t nominal_uah;
  uint32_t design_uah;
  uint16_t mv;
  uint16_t soc_cpct;
  uint16_t cycles_pct;
  int16_t temp_dc;
};

//! A discharge or a recharge
struct bt_span {
  uint32_t start;
  uint32_t end;
  int32_t uah;    // counter: out of the battery for the discharge, into it for the recharge
  int32_t sw_uah; // software integral: the same
  int8_t tmin_c;
  int8_t tmax_c;
};

//! What is saved at each change of phase
struct bt_state {
  uint32_t magic;
  uint8_t phase;
  uint8_t flags;
  uint8_t loads;
  uint8_t log_shift;      // the log keeps every 2^log_shift-th sample
  int32_t gap_uah;        // bridged in the integral by the counter
  struct bt_full full[2]; // at the start of the discharge, at the end of the recharge
  struct bt_span dis;
  struct bt_span rec;
};

//! The running sums, restored from the last record after a reset
struct bt_run {
  bool valid;
  uint8_t flags;
  uint8_t loads;
  uint8_t resets;
  uint16_t counter;
  int8_t tmin_c;
  int8_t tmax_c;
  uint32_t time;
  int32_t avg_ua;
  int32_t out_uah;
  int64_t sw_uas;
};

#define BT_STEP_LOG  0x01U // a record was filled
#define BT_STEP_SAVE 0x02U // the phase changed: save the state, log the record whatever the rate

//! Charge out of the battery between two readings of a coulomb counter that wraps at 16 bits
static inline int32_t bt_counter_out_uah(uint16_t prev, uint16_t now, uint16_t uah) {
  return -(int32_t)(int16_t)(uint16_t)(now - prev) * (int32_t)uah;
}

//! Trapezoidal integral of two averaged currents over dt seconds, in uAs
static inline int64_t bt_integral_uas(int32_t prev_ua, int32_t now_ua, uint32_t dt_s) {
  return (((int64_t)prev_ua + now_ua) * (int64_t)dt_s) / 2;
}

//! Average current over a span, in uA; 0 for an empty one
int32_t bt_avg_ua(int32_t uah, uint32_t start, uint32_t end);

//! Start a test: charge to full first
void bt_start(struct bt_state *st, struct bt_run *run, uint32_t magic, bool heavy);

//! Take a sample: count, move between phases and fill its record. Returns BT_STEP_* flags.
unsigned int bt_step(struct bt_state *st, struct bt_run *run, const struct bt_sample *s,
                     struct bt_record *rec);

//! Carry on from the last record, after a reset: the new boot's gauge resets count from 0
void bt_run_restore(struct bt_run *run, const struct bt_record *rec);
