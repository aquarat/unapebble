/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "resource_storage_flash.h"

//! @file resource_storage_volume.h
//!
//! System resources taken from a file on a FAT volume of the eMMC, as the update drive of the UNA
//! Watch, which the resources are copied to next to the firmware.

//! Copy the resource pack of version @p version from the root directory of the volume into
//! @p bank, overwriting it. Any file whose manifest has that version is taken, whatever its name.
//! Returns whether one was copied; the bank is then still to be checked.
bool resource_storage_volume_install(const SystemResourceBank *bank,
                                     const ResourceVersion *version);
