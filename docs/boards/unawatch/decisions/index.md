# UNA Watch decision log

Every decision about the UNA Watch port is recorded here, one file per
decision, so that the reasons survive the conversations and agents that made
them. Read the log before changing something it covers.

## Writing a record

- Name the file `YYMMDD-HHMMSS-NN-short-title.md`, with the UTC time the
  decision was made (`date -u +%y%m%d-%H%M%S`). `NN` numbers decisions made in
  the same second, starting at `01`; if two agents pick the same name, the
  later one takes the next `NN`. Names sort in time order.
- Use the sections of the template below. Keep it short: what was decided,
  why, and what follows from it.
- A record isn't rewritten when the decision changes. Write a new record, set
  the old one's status to `Superseded by <new file name>`, and link both ways.
- Status is one of `Proposed`, `Accepted`, `Superseded by ...` or `Rejected`.
- Say who decided: `owner` for the project owner, `agent` for a choice an
  agent made within what the owner asked for.

```text
# Title

- Status: Accepted
- Date: 2026-11-26 13:06 UTC
- Decided by: owner | agent

## Context
## Decision
## Consequences
```

Records written after the fact say so in their context section.

## Records

```{toctree}
:glob:
:maxdepth: 1

2*
```
