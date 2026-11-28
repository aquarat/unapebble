# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0

pbl_clar_test(test_sport_engine
  SOURCES
    pebble/apps/system/sport/engine.c
    pebble/apps/system/sport/fields.c
)

pbl_clar_test(test_sport_fit
  SOURCES
    pebble/apps/system/sport/fit.c
    pebble/apps/system/sport/journal.c
)

pbl_clar_test(test_sport_recorder
  MODULES pfs
  SOURCES
    pebble/apps/system/sport/engine.c
    pebble/apps/system/sport/fit.c
    pebble/apps/system/sport/journal.c
    pebble/apps/system/sport/recorder.c
    pebble/apps/system/sport/store.c
    tests/fakes/fake_rtc.c
  PLATFORMS emery
  DEFINES DUMA_DISABLED CONFIG_APP_SPORT_FLUSH_S=10 CONFIG_APP_SPORT_MAX_ACTIVITIES=30
          CONFIG_APP_SPORT_FREE_RESERVE_KIB=1024
  OVERRIDES dummy_board
)

# The encoder's files decoded by public FIT decoders, fitdecode and fitparse, with the Python that
# has them: SPORT_FIT_PYTHON, by default the build's.
set(SPORT_FIT_PYTHON ${PYTHON_EXECUTABLE} CACHE FILEPATH "Python with fitdecode and fitparse")
execute_process(COMMAND ${SPORT_FIT_PYTHON} -c "import fitdecode, fitparse"
                RESULT_VARIABLE _sport_fit_decoders OUTPUT_QUIET ERROR_QUIET)
if(_sport_fit_decoders EQUAL 0)
  set(_dir ${CMAKE_CURRENT_BINARY_DIR}/test_sport_fit_decode)
  add_test(NAME test_sport_fit_decode
    COMMAND sh -c "rm -rf \"$0\" && mkdir -p \"$0\" && cd \"$0\" && SPORT_FIT_OUT=\"$0\" \"$1\" >/dev/null && SPORT_FIT_OUT=\"$0\" \"$2\" >/dev/null && \"$3\" -I \"$4\" \"$0\"/*.fit"
            ${_dir} $<TARGET_FILE:test_sport_fit> $<TARGET_FILE:test_sport_recorder_emery>
            ${SPORT_FIT_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/check_fit.py)
  set_tests_properties(test_sport_fit_decode PROPERTIES LABELS "tests/pebble/apps/system_apps/sport")
else()
  message(STATUS "fitdecode and fitparse not found: test_sport_fit_decode left out")
endif()
