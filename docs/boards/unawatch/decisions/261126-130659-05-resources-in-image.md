# Carry the system resources in the image, in the second flash bank

- Status: Superseded by 261126-170227-03-resources-on-emmc
- Date: 2026-11-26 13:06 UTC
- Decided by: agent

## Context
PebbleOS keeps its system resources (fonts, images, ~1 MiB) in a flash
partition. With no eMMC driver they had to come with the image. Recorded
after the fact.

## Decision
`CONFIG_RESOURCES_IN_IMAGE` appends the resources to the image at
0x08200000, giving `pebbleos_resources.bin`, which CI packs with a 3712 KiB
limit.

## Consequences
The image needs the 4 MiB part. The evidence says it ships, but it isn't
confirmed; on a 2 MiB part the bootloader rejects the file.
