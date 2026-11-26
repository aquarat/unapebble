/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "tether_blob.h"

#include <string.h>

#ifndef BLOB_NAME
#define BLOB_NAME "blob"
#endif

extern char _image_end[], _sbss[], _ebss[];

const struct tether_api *tether;

uint32_t blob_start(const struct tether_api *api, const uint32_t *args, uint32_t nargs);

__attribute__((section(".blob_header"), used)) const struct tether_blob_header blob_header = {
  .magic = TETHER_BLOB_MAGIC,
  .version = 1U,
  .load_addr = (uint32_t)&blob_header,
  .entry = (uint32_t)blob_start,
  .image_end = (uint32_t)_image_end,
  .bss_end = (uint32_t)_ebss,
  .name = BLOB_NAME,
};

uint32_t blob_start(const struct tether_api *api, const uint32_t *args, uint32_t nargs) {
  memset(_sbss, 0, (size_t)(_ebss - _sbss));
  tether = api;
  if (api == NULL || api->magic != TETHER_API_MAGIC) {
    return 0xdeadbeefU;
  }
  return blob_main(args, nargs);
}
