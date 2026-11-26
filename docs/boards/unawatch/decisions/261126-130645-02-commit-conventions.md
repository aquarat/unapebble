# Follow upstream PebbleOS commit conventions

- Status: Accepted
- Date: 2026-11-26 13:06 UTC
- Decided by: agent

## Context
The repository is a fork of coredevices/PebbleOS, whose `AGENTS.md` sets the
rules for commits made with an assistant. Recorded after the fact.

## Decision
Commits are signed off (`git commit -s`), carry an `Assisted-by: LLM`
trailer and no `Co-Authored-By`, and are titled `area: description`.

## Consequences
Commits can be sent upstream without being rewritten.
