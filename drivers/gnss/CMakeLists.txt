# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0

pbl_library()
pbl_library_sources(gnss.c)
pbl_library_sources_ifdef(CONFIG_GNSS_AG3335 ag3335.c nmea.c)
pbl_library_sources_ifdef(CONFIG_GNSS_SIM sim.c sim_track.c)
pbl_library_sources_ifdef(CONFIG_GNSS_REPLAY sim_track.c)
pbl_library_sources_ifdef(CONFIG_GNSS_SHELL shell.c)
