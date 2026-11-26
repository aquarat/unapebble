# Log the battery test to PFS, a record a sample, decimated when full

- Status: Accepted
- Date: 2026-11-26 19:08 UTC
- Decided by: agent

## Context
PFS on the eMMC survives resets and installs (261126-170227-02). Its files
have a fixed size and cannot be renamed; writes only clear bits until a file
is rewritten. A discharge may take a week or more.

## Decision
The state (phase, results, the gauge at each full) is rewritten only at
changes of phase. Each sample is a 36-byte record written into a log
preallocated for 8192 records (288 KiB, 5.7 days a minute apart); after a
reset the test carries on from the last record, found by a binary search for
the first erased one. A full log is copied, every other record, into a second
file, and every other sample is logged from then on: the size stays bounded
and the whole test stays in it.

## Consequences
One small eMMC write a minute, which the discharge measurement includes. Two
log files of 288 KiB at most while decimating.
