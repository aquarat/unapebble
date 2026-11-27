/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "core.h"

static int16_t prv_i16(int32_t v) {
  return (int16_t)((v > INT16_MAX) ? INT16_MAX : ((v < INT16_MIN) ? INT16_MIN : v));
}

static uint16_t prv_u16(uint32_t v) {
  return (uint16_t)((v > UINT16_MAX) ? UINT16_MAX : v);
}

static int8_t prv_celsius(int32_t temp_mc) {
  const int32_t c = temp_mc / 1000;

  return (int8_t)((c > INT8_MAX) ? INT8_MAX : ((c < INT8_MIN) ? INT8_MIN : c));
}

static void prv_temp(struct bt_run *run, int8_t c, bool first) {
  if (first || c < run->tmin_c) {
    run->tmin_c = c;
  }
  if (first || c > run->tmax_c) {
    run->tmax_c = c;
  }
}

static void prv_full(struct bt_full *f, const struct bt_sample *s) {
  *f = (struct bt_full){
    .time = s->time,
    .full_uah = s->r.full_uah,
    .nominal_uah = s->r.full_nominal_uah,
    .design_uah = s->r.design_uah,
    .mv = prv_u16((uint32_t)s->r.voltage_mv),
    .soc_cpct = s->r.soc_cpct,
    .cycles_pct = s->r.cycles_pct,
    .temp_dc = prv_i16(s->r.temp_mc / 100),
  };
}

static int32_t prv_uah(int64_t uas) {
  return (int32_t)(uas / 3600);
}

int32_t bt_avg_ua(int32_t uah, uint32_t start, uint32_t end) {
  if (end <= start) {
    return 0;
  }

  return (int32_t)(((int64_t)uah * 3600) / (int64_t)(end - start));
}

void bt_start(struct bt_state *st, struct bt_run *run, uint32_t magic, bool heavy) {
  *st = (struct bt_state){
    .magic = magic,
    .phase = BT_CHARGE,
    .flags = heavy ? BT_F_HEAVY : 0U,
  };
  *run = (struct bt_run){0};
}

// Count from the previous sample: the gauge's counter, and the integral of its averaged
// current. A counter that started again leaves the integral to stand in; a gap leaves the
// counter to bridge the integral.
static uint8_t prv_count(struct bt_state *st, struct bt_run *run, const struct bt_sample *s) {
  const struct pbl_gauge_reading *r = &s->r;
  const int32_t dt = (int32_t)(s->time - run->time);
  uint8_t lost = 0U;
  int32_t dq = 0;
  int64_t dsw;

  if ((r->resets != run->resets) || (r->counter_uah == 0U)) {
    lost = BT_S_LOST;
  } else {
    dq = bt_counter_out_uah(run->counter, r->counter, r->counter_uah);
  }

  if ((dt > 0) && (dt <= BT_GAP_S)) {
    dsw = bt_integral_uas(run->avg_ua, r->avg_current_ua, (uint32_t)dt);
  } else {
    dsw = (int64_t)dq * 3600;
    st->gap_uah += dq;
  }

  if (lost != 0U) {
    dq = prv_uah(dsw);
    st->flags |= BT_F_LOST;
  }

  run->out_uah += dq;
  run->sw_uas += dsw;

  return lost;
}

unsigned int bt_step(struct bt_state *st, struct bt_run *run, const struct bt_sample *s,
                     struct bt_record *rec) {
  const bool plugged = (s->flags & BT_S_PLUGGED) != 0U;
  const bool full = plugged && ((s->flags & BT_S_FULL) != 0U);
  const int8_t c = prv_celsius(s->r.temp_mc);
  const struct bt_run prev = *run;
  unsigned int ret = BT_STEP_LOG;
  uint8_t lost = 0U;

  if ((st->phase == BT_IDLE) || (st->phase == BT_DONE)) {
    return 0U;
  }

  if (prev.valid) {
    lost = prv_count(st, run, s);
  }
  run->valid = true;
  run->time = s->time;
  run->counter = s->r.counter;
  run->resets = s->r.resets;
  run->avg_ua = s->r.avg_current_ua;
  run->flags = s->flags;

  switch (st->phase) {
    case BT_CHARGE:
      if (full) {
        st->phase = BT_FULL;
        ret |= BT_STEP_SAVE;
      }
      break;
    case BT_FULL:
      if (!plugged) {
        // Counted from here: since the last sample at full, the battery stood idle
        st->phase = BT_DISCHARGE;
        st->dis = (struct bt_span){.start = s->time};
        run->out_uah = 0;
        run->sw_uas = 0;
        run->loads = 0U;
        prv_temp(run, c, true);
        ret |= BT_STEP_SAVE;
      } else if (!full) {
        st->phase = BT_CHARGE;
        ret |= BT_STEP_SAVE;
      }
      break;
    case BT_DISCHARGE:
      if (plugged) {
        // It ended with the previous sample, the last before the power-off
        const struct bt_run *end = prev.valid ? &prev : run;

        st->dis.end = end->time;
        st->dis.uah = end->out_uah;
        st->dis.sw_uah = prv_uah(end->sw_uas);
        st->dis.tmin_c = end->tmin_c;
        st->dis.tmax_c = end->tmax_c;
        st->loads = end->loads;
        if ((end->flags & BT_S_EMPTY) != 0U) {
          st->flags |= BT_F_COMPLETE;
        }
        st->phase = BT_RECHARGE;
        st->rec = (struct bt_span){.start = s->time};
        prv_temp(run, c, true);
        ret |= BT_STEP_SAVE;
      }
      break;
    case BT_RECHARGE:
      if (full) {
        prv_temp(run, c, false);
        st->rec.end = s->time;
        st->rec.uah = st->dis.uah - run->out_uah;
        st->rec.sw_uah = st->dis.sw_uah - prv_uah(run->sw_uas);
        st->rec.tmin_c = run->tmin_c;
        st->rec.tmax_c = run->tmax_c;
        prv_full(&st->full[1], s);
        st->phase = BT_DONE;
        ret |= BT_STEP_SAVE;
      }
      break;
    default:
      break;
  }

  switch (st->phase) {
    case BT_CHARGE:
    case BT_FULL:
      // Counted from the last sample at full
      run->out_uah = 0;
      run->sw_uas = 0;
      if (full) {
        prv_full(&st->full[0], s);
      }
      break;
    case BT_DISCHARGE:
      run->loads |= ((s->flags & BT_S_HEAVY) != 0U) ? BT_LOAD_HEAVY : BT_LOAD_NORMAL;
      prv_temp(run, c, false);
      break;
    case BT_RECHARGE:
      prv_temp(run, c, false);
      break;
    default:
      break;
  }

  *rec = (struct bt_record){
    .time = s->time,
    .out_uah = run->out_uah,
    .sw_uah = prv_uah(run->sw_uas),
    .mv = prv_u16((uint32_t)s->r.voltage_mv),
    .avg_mv = prv_u16((uint32_t)s->r.avg_voltage_mv),
    .i_10ua = prv_i16(s->r.current_ua / 10),
    .avg_i_10ua = prv_i16(s->r.avg_current_ua / 10),
    .soc_cpct = s->r.soc_cpct,
    .remaining_dmah = prv_u16(s->r.remaining_uah / 100U),
    .full_dmah = prv_u16(s->r.full_uah / 100U),
    .counter = s->r.counter,
    .temp_dc = prv_i16(s->r.temp_mc / 100),
    .phase = st->phase,
    .flags = (uint8_t)(s->flags | lost),
    .loads = run->loads,
    .tmin_c = run->tmin_c,
    .tmax_c = run->tmax_c,
    .resets = s->r.resets,
  };

  return ret;
}

void bt_run_restore(struct bt_run *run, const struct bt_record *rec) {
  *run = (struct bt_run){
    .valid = true,
    .flags = rec->flags,
    .loads = rec->loads,
    .resets = 0U,
    .counter = rec->counter,
    .tmin_c = rec->tmin_c,
    .tmax_c = rec->tmax_c,
    .time = rec->time,
    .avg_ua = (int32_t)rec->avg_i_10ua * 10,
    .out_uah = rec->out_uah,
    .sw_uas = (int64_t)rec->sw_uah * 3600,
  };
}
