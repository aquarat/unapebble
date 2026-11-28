# Mend the opcode of the DTM's Command Status for LE Read Remote Features

- Status: Accepted
- Date: 2026-11-27 19:40 UTC
- Decided by: agent

## Context
On the first connections from a central, NimBLE sent LE Read Remote
Features (`0x2016`) after the connection complete event. The DTM answered
with a Command Status of Controller Busy (`0x3A`) carrying the opcode
`0x0016`, its OGF bits clear. NimBLE took it for the answer to another
command, reset its host (`BLE_HS_ECONTROLLER`) and dropped the connection.

## Decision
The transport keeps the opcode of the last command it sent. A Command
Status whose opcode is that one's OCF alone is given that opcode before
the host sees it.

## Consequences
The host takes the refusal as one: it does not retry LE Read Remote
Features, so it has no remote features for the link (the controller does
its own feature exchange). A Command Complete with the same fault would
not be mended; none has been seen.
