# Battery test behind CONFIG_BATTERY_TEST, on for the UNA Watch

- Status: Accepted
- Date: 2026-11-26 19:08 UTC
- Decided by: maintainer (on for the watch); agent (option)

## Context
The test needs a fuel gauge chip (BATTERY_SOC_GAUGE), which only the UNA
Watch has, and code space is short (261126-172141-06).

## Decision
`CONFIG_BATTERY_TEST` builds the app, its logger and the `battery test`
shell commands; `pebble/boards/unawatch.conf` turns it on.

## Consequences
About 6 KiB of code. Off again, or removed, once the cell is known.
