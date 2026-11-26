# The Lab is a debug build, without Moddable XS

- Status: Accepted
- Date: 2026-11-26 23:27 UTC
- Decided by: agent

## Context
With the resources in the image the code has 1664 KiB, up to 0x08200000.
A debug build with Moddable XS and the Lab overflows by 3 KiB.

## Decision
The Lab leaves Moddable XS out (about 700 KiB) and stays a debug build:
asserts with their file and line, which the trace records. It takes 966 KiB.

## Consequences
No JavaScript apps on the Lab; they have nothing to do with its experiments.
