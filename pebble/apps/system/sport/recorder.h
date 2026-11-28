/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "engine.h"
#include "journal.h"

#include <stdbool.h>
#include <stdint.h>

//! Writes the activity being recorded to its journal (journal.h). The state is static, outside
//! the app's heap, so that the restart and power-off handlers can write what is buffered.
//! Samples are buffered and written every CONFIG_APP_SPORT_FLUSH_S seconds, events at once.

//! Start a new activity: delete the oldest past the limit, create its first segment, write the
//! start event.
bool sport_recorder_start(uint8_t sport, uint32_t start_utc);

//! Carry on recording an activity that has no summary, after its last record, with a restarted
//! event. @p time_s is the activity's time now.
bool sport_recorder_resume(uint32_t start_utc, uint32_t time_s);

//! Write what is buffered and stop recording, leaving the activity unfinished, as a restart would.
void sport_recorder_abandon(void);

//! Whether an activity is being recorded, and which.
bool sport_recorder_is_active(uint32_t *start_utc);

//! Whether the storage or the time limit ended the recording.
bool sport_recorder_is_truncated(void);

//! Add the second the engine just processed.
void sport_recorder_sample(const SportEngine *engine, const SportSample *sample, uint32_t time_s);

//! Add an event without statistics, and write.
void sport_recorder_event(uint8_t event, uint32_t time_s);

//! Add a closed lap, and write.
void sport_recorder_lap(const SportLap *lap, uint32_t time_s);

//! Add a time record: the GNSS's UTC at @p time_s, with the RTC's.
void sport_recorder_time(uint32_t gnss_utc, uint32_t rtc_utc, uint32_t time_s);

//! Write what is buffered.
void sport_recorder_flush(void);

//! Write the stop event with the engine's totals, then the summary, and end.
void sport_recorder_finish(const SportEngine *engine, uint32_t time_s);

//! Delete the activity being recorded, and end.
void sport_recorder_discard(void);

//! Statistics of the journal for the engine's: a lap or the totals.
void sport_recorder_pack_stats(const SportStats *stats, uint8_t event, uint8_t info, uint16_t laps,
                               SportJournalStats *out);

#if UNITTEST
//! What a fault or a sudden power loss does: forget the state and what is buffered.
void sport_recorder_test_lose_buffer(void);
#endif
