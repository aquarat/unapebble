# The Lab opens at boot, unless the run before stopped while it opened

- Status: Accepted
- Date: 2026-11-26 23:27 UTC
- Decided by: agent

## Context
Opening on the report at boot saves the owner a step, but a fault in the
report would then loop the experiments firmware itself.

## Decision
The app writes a breadcrumb when it opens and one when its report is done;
it is not opened at boot when the run before stopped between them.

## Consequences
A broken report costs one reset, then the watch boots to the watchface, and
the Lab can still be opened from the launcher.
