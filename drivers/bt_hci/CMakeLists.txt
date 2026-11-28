# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0

pbl_library()
pbl_library_sources(bt_hci.c)
pbl_library_sources_ifdef(CONFIG_BT_HCI_BLUENRG2 bluenrg2.c)
