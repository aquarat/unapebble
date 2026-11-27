# Offer a reduced charge voltage as an option, off by default

- Status: Accepted
- Date: 2026-11-26 17:35 UTC
- Decided by: agent

## Context
Stopping short of full (such as at 80%) extends a cell's life further. The
PCA9420 can only stop at a voltage, not at a percentage, and the MAX17262 would
take a charge ending early for a full one, and learn a wrong capacity, if the
end came above its FullSOCThr.

## Decision
`CONFIG_CHARGER_PCA9420_CHARGE_LIMIT_MV`, 0 (off) by default: a voltage below
the battery's, such as 4100 for about 75 to 80% of a 4.4 V cell. With it set,
`CONFIG_GAUGE_MAX17262_FULL_SOC_THRESHOLD` defaults to 95%, so the gauge does
not count the early end as full. The percentage shown stays the gauge's, so a
limited watch shows about 80% when its charge is done. Not the default: 4.36 V
at 0.5C is already gentle, and a fifth less runtime on a 200 mAh watch is a
choice for its user.

## Consequences
A build option, not a setting on the watch; a user setting would need UI and
reconfiguring the charger at run time. With the option, switching to the stock
firmware makes it reload the gauge model (FullSOCThr differs).
