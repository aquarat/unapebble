# The SPI's CRCSIZE kept at its reset value: the last byte of a read mended at its cause

- Status: Accepted
- Date: 2026-11-28 10:01 UTC
- Decided by: agent

## Context
261128-092026-01-bluenrg2-last-byte-of-a-read found the last byte of every
SPI transfer from the BlueNRG-2 coming back with only its two low bits, and
read one dummy byte past each packet. The coordinator asked for the cause,
since other devices will use `drivers/spi/stm32.c`. Instrumented on the
watch (each case a restart of the chip, the identity's last byte `0xcd`
read or not):

- At the last read, SR has RXP, EOT and TXC set, one frame in the FIFO: the
  frame is complete, not read early. Waiting for EOT first changes nothing.
- One byte ahead instead of eight, and up to 15 idle clocks between frames
  (CFG2 MIDI), change nothing; one-byte transfers cut every byte.
- With TSIZE 0, the transfer ended by CSUSP, the last byte is right.

ST lists for the newer SPI of the STM32U5 family that with the CRC off and
TSIZE above 0, a CRCSIZE below 8 bits corrupts the last data received; the
workaround is to keep CRCSIZE at its reset value, `00111` (STM32U535/545
errata ES0587, and the U3 sheets; the STM32U59x/5Ax sheet ES0553 was not
reachable to check, inferred to apply). The driver wrote all of CFG1,
CRCSIZE `00000` included.

## Decision
The driver keeps CRCSIZE at 8 bits in CFG1. On the watch: the identity
address reads `cd:ce:e6:ec:45:ce` without a dummy byte, values written by
a central arrive whole (`fa`, `0a 77`), pairing and the PPoGATT session run
without a retransmission. The dummy byte is gone from `bnrg_read()`.

## Consequences
- 261128-092026-01 is superseded by this record; its findings stand,
  their cause is this.
- Any other device on this SPI driver reads its last byte right too.
