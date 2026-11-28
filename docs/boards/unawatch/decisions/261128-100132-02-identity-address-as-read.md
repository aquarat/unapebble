# The identity address: config 0x80 as it reads

- Status: Accepted
- Date: 2026-11-28 10:01 UTC
- Decided by: agent

## Context
261127-193700-01-identity-from-config-0x80 took config 0x80 as the
identity, read `ce 45 ec e6 ce 01`, and set the two top bits it found
clear, giving `C1:CE:E6:EC:45:CE`. The last byte was cut by the SPI
(261128-100132-01-spi-crcsize-erratum): it is `0xcd`. The module's address
is `CD:CE:E6:EC:45:CE`, a static random address as it is, which is what the
stock firmware requires of it.

## Decision
The identity address is config 0x80 as it reads: `CD:CE:E6:EC:45:CE` on
this watch. Setting the two top bits stays, as a no-op on a valid address;
a random part of all zeros or all ones still gives no address.

## Consequences
261127-193700-01 is superseded. The watch's address changed with the SPI
fix; a central bonded with `C1:CE:E6:EC:45:CE` pairs again.
