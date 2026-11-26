# End the charge at 14 mA with a 6.4 minute top-off

- Status: Accepted
- Date: 2026-11-26 17:35 UTC
- Decided by: agent

## Context
The stock firmware ends at 10 mA (ICHG_TOPOFF) with the longest top-off timer,
19.2 min, and has its gauge's IChgTerm at 14 mA. The gauge only detects the end
of a charge while the current is between IChgTerm/8 and 1.25 x IChgTerm, and
Maxim asks for IChgTerm equal to the charger's termination current (UG6597,
IChgTerm). Time held at the charge voltage ages the cell.

## Decision
`charge-term-current-microamp = <14000>`: ICHG_TOPOFF 14 mA (0.07C), the same
value as IChgTerm, and T_TOPOFF 6.4 min, the PCA9420 default.

## Consequences
The cell spends less time at its top voltage than with the stock firmware and
ends slightly less full. The gauge detects the end of each charge, so it
learns the capacity.
