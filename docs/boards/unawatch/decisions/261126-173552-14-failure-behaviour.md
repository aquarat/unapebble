# Fail towards not charging and not reporting full

- Status: Accepted
- Date: 2026-11-26 17:35 UTC
- Decided by: agent

## Context
An I2C failure or a misread must neither leave the charger on unsafe settings
nor show a battery fuller than it is. A failed device init in PebbleOS makes
every later call on the charger assert.

## Decision
- The charger is configured with charging off: CHG_CNTL1 to CHG_CNTL6 are
  written behind CHG_LOCK, read back and locked again; charging is only
  enabled on a configuration that read back. A failure leaves it off and is
  retried on the next use. Every status poll checks the charge current and
  voltage registers and reconfigures a PMIC that reset to its defaults.
- Neither driver fails its init on a chip that does not answer: they report
  errors on use instead, and the battery service keeps polling.
- A gauge reading is refused when Status.POR is set, the battery is absent,
  DesignCap is not ours, or the voltage (2.0 to 4.7 V) or temperature (-40 to
  85 degrees C) is implausible. A refused reading keeps the last good one.
- Until a first good reading the watch shows 5%: low enough to ask for a
  charge, never full, above the low power threshold so it does not shut down
  on a gauge it cannot read. The percentage rounds down, so 100% shows only
  once the gauge holds 100% (it holds 99% until the end of charge).
- Without a good temperature reading charging stays off (record 10).

## Consequences
A dead gauge stops charging: the watch then runs down and has to go back to
the stock firmware, rather than charge with no temperature check. The
battery is never shown full on a reading that is not.
