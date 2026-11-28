#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0
"""Decode FIT files with public decoders, fitdecode and fitparse, and check them.

    check_fit.py FILE.fit...

Each file must decode with both, its CRCs checked, with file_id, records, laps, a session and an
activity. Files named sport_fit_{run,walk,bike}.fit, which test_sport_fit writes, are checked
against the journals it encoded; others, such as files exported from a watch, only for their
structure and consistency. Exits non-zero on the first failure.
"""

import datetime
import os
import sys

import fitdecode
import fitparse

START = datetime.datetime(2026, 11, 28, 10, 0, 0, tzinfo=datetime.UTC)


def decode_fitdecode(path):
    msgs = []
    with fitdecode.FitReader(path, check_crc=fitdecode.CrcCheck.ENABLED) as fit:
        for frame in fit:
            if isinstance(frame, fitdecode.FitDataMessage):
                fields = {f.name: f.value for f in frame.fields}
                msgs.append((frame.name, fields))
    return msgs


def decode_fitparse(path):
    fit = fitparse.FitFile(path, check_crc=True)
    fit.parse()
    return [(m.name, m.get_values()) for m in fit.messages]


def utc(v):
    if v is None:
        return None
    if v.tzinfo is None:
        v = v.replace(tzinfo=datetime.UTC)
    return v


def check(cond, what):
    if not cond:
        raise AssertionError(what)


def by_name(msgs, name):
    return [f for n, f in msgs if n == name]


def check_structure(path, msgs):
    names = [n for n, _ in msgs]
    check(names[0] == "file_id", "file_id first")
    fid = msgs[0][1]
    check(fid["type"] == "activity", f"file type {fid['type']}")
    records = by_name(msgs, "record")
    laps = by_name(msgs, "lap")
    sessions = by_name(msgs, "session")
    activities = by_name(msgs, "activity")
    check(len(records) > 0, "records")
    check(len(laps) >= 1, "a lap")
    check(len(sessions) == 1, "one session")
    check(len(activities) == 1, "one activity")
    s = sessions[0]
    check(s["num_laps"] == len(laps), "num_laps")
    check(
        abs(sum(lap["total_timer_time"] for lap in laps) - s["total_timer_time"])
        <= len(laps),
        "laps' timer add up to the session's",
    )
    times = [utc(r["timestamp"]) for r in records]
    check(times == sorted(times), "records in time order")
    check(utc(s["start_time"]) <= times[0], "session starts before its records")
    check(utc(s["timestamp"]) >= times[-1], "session ends after its records")
    dists = [r["distance"] for r in records if r.get("distance") is not None]
    check(dists == sorted(dists), "distance never decreases")
    check(
        abs(dists[-1] - s["total_distance"]) <= 5.0 + 0.01 * s["total_distance"],
        f"last record's distance {dists[-1]} near the session's {s['total_distance']}",
    )
    timers = [e for e in by_name(msgs, "event") if e["event"] == "timer"]
    check(timers and timers[0]["event_type"] == "start", "timer started first")
    check(timers[-1]["event_type"] in ("stop_all", "stop"), "timer stopped last")
    return records, laps, s, activities[0], timers


def check_known(name, records, laps, s, act, timers):
    if name == "sport_fit_run.fit":
        check(s["sport"] == "running", "running")
        check(len(records) == 570, f"570 records, {len(records)}")
        check(
            utc(s["start_time"]) == START + datetime.timedelta(seconds=3), "GNSS time"
        )
        check(s["total_timer_time"] == 570.0, f"timer {s['total_timer_time']}")
        check(s["total_elapsed_time"] == 600.0, f"elapsed {s['total_elapsed_time']}")
        check(
            abs(s["total_distance"] - 1710.0) < 0.01, f"distance {s['total_distance']}"
        )
        check(len(laps) == 2, "two laps")
        check(laps[0]["lap_trigger"] == "manual", "manual lap")
        check(laps[1]["lap_trigger"] == "session_end", "final lap")
        check(abs(s["avg_speed"] - 3.0) < 0.01, f"avg speed {s['avg_speed']}")
        check(s["total_ascent"] == 12, f"ascent {s['total_ascent']}")
        r = records[0]
        check(abs(r["position_lat"] / 2**31 * 180 - 47.3769) < 1e-4, "latitude")
        check(abs(r["position_long"] / 2**31 * 180 - 8.5417) < 1e-4, "longitude")
        check(abs(r["altitude"] - 408.0) < 0.3, f"altitude {r['altitude']}")
        check(abs(r["speed"] - 3.0) < 0.001, "speed")
        # 171 steps a minute: 85.5 strides.
        check(
            r["cadence"] == 85 and abs(r.get("fractional_cadence", 0) - 0.5) < 0.01,
            "cadence",
        )
        stops = [e for e in timers if e["event_type"] == "stop_all"]
        check(len(stops) == 2, "a stop for the pause and one at the end")
    elif name == "sport_fit_walk.fit":
        check(s["sport"] == "walking", "walking")
        check(len(records) == 180, "180 records")
        check(utc(s["start_time"]) == START, "RTC time")
        check(
            sum(1 for r in records if r.get("position_lat") is None) == 60,
            "60 records without a position",
        )
        check(
            len(laps) == 1 and laps[0]["lap_trigger"] == "session_end",
            "one lap from the samples",
        )
        check(abs(s["total_distance"] - 252.0) < 0.01, "distance")
        check(s["avg_cadence"] == 55, f"avg cadence {s['avg_cadence']}")
    elif name == "sport_fit_bike.fit":
        check(s["sport"] == "cycling", "cycling")
        check(len(records) == 200, f"200 records, {len(records)}")
        check(s["total_timer_time"] == 200.0, "timer")
        check(s["total_elapsed_time"] == 300.0, "elapsed")
        check(all(r.get("cadence") is None for r in records), "no cadence")
        stops = [e for e in timers if e["event_type"] == "stop_all"]
        check(len(stops) == 3, "stops: restart, auto-pause, end")


def main(paths):
    for path in paths:
        name = os.path.basename(path)
        a = decode_fitdecode(path)
        b = decode_fitparse(path)
        check([n for n, _ in a] == [n for n, _ in b], f"{name}: the decoders agree")
        parts = check_structure(path, a)
        check_known(name, *parts)
        s = parts[2]
        print(
            f"{name}: ok, {len(parts[0])} records, {len(parts[1])} laps, "
            f"{s['sport']} {s['total_distance']:.0f} m in {s['total_timer_time']:.0f} s, "
            f"from {utc(s['start_time']).isoformat()}"
        )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except AssertionError as e:
        print(f"check_fit: {e}", file=sys.stderr)
        sys.exit(1)
