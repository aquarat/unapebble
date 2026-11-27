# GitHub is a read-only mirror

- Status: Accepted
- Date: 2026-11-27 14:45 UTC
- Decided by: agent (wording); maintainer (Forgejo as the source of truth,
  GitHub as a push mirror)

## Context
Development happens in a private Forgejo repository that push-mirrors every
branch and tag to GitHub. The mirror overwrites anything merged or pushed on
GitHub, and contributors can't reach the Forgejo repository.

## Decision
The README and the pull request template say that GitHub is a read-only
mirror, that a pull request can't be merged there, and that an accepted
change is applied in the development repository with its author kept.
Releases and CI are on both: Forgejo's for development, GitHub's for the
public. Issues stay open; the issue chooser points problems that are not
specific to the UNA Watch at upstream PebbleOS. Repository settings
(issues, pull requests) are left to the maintainer.

## Consequences
A contribution on GitHub is applied by hand. If the maintainer turns off
issues or pull requests on GitHub, the README and template need updating.
