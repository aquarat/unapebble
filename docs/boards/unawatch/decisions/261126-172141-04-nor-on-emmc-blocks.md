# NOR flash on eMMC blocks, written through

- Status: Accepted
- Date: 2026-11-26 17:21 UTC
- Decided by: agent

## Context
PebbleOS's flash users (PFS, the flash log, the coredump) expect NOR: bytes
programmed by clearing bits, reads anywhere, 4 KiB subsectors and 64 KiB
sectors erased to 0xFF. The eMMC rewrites whole 512-byte blocks and levels its
own wear. PFS relies on the order of its writes for consistency after a
power loss, and any reset of the SoC cuts the eMMC's supply.

## Decision
`drivers/flash/blk_nor.c` emulates NOR on blocks, and `drivers/flash/mmc_nor.c`
(`pebble,mmc-nor`) exposes a range of an MMC as a flash with it. A write reads
the blocks it touches, ANDs the data in and writes back those that changed
before returning; an erase writes blocks of 0xFF. Runs of whole blocks move 8
blocks (4 KiB) a device access. 16 blocks used in part are cached, always
holding what the card holds, for small reads and appends. There is no write
back cache: unlike the embedded flash's `ecc_nor`, writes reach the card in
the order they were made, and a reset loses nothing that returned. With
reliable writes (261126-172141-02), a power loss leaves a block either old or
new. A subsector erase does not use the card's erase or trim, whose erased
value is the card's choice and usually 0x00.

## Consequences
Every write costs a block read and write, unless cached: a write of a few
bytes takes a couple of milliseconds. A 2.4 MiB core dump takes some seconds.
Unit-tested in `tests/drivers/flash/test_blk_nor.c`.
