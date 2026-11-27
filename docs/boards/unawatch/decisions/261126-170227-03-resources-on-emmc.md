# Keep the system resources on the eMMC

- Status: Superseded by 261127-131711-01-resources-in-the-ota
- Date: 2026-11-26 17:02 UTC
- Decided by: maintainer

## Context
With the resources in the image (261126-130659-05-resources-in-image), the
image needs a 4 MiB part that isn't confirmed, and the resources take
embedded flash. With storage on the eMMC
(261126-170227-02-storage-on-emmc), they can live there too.

## Decision
The system resources move to the eMMC GUI region. How they get there on each
install is settled by the agent implementing it and recorded in its own
decision.

## Consequences
The aim is an image that fits a 2 MiB part, with more embedded flash for
code.
