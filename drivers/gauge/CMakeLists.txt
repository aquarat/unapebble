# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0

pbl_library()
pbl_library_sources(gauge.c)
pbl_library_sources_ifdef(CONFIG_GAUGE_MAX17262 max17262.c)
pbl_library_sources_ifdef(CONFIG_GAUGE_SHELL shell.c)
