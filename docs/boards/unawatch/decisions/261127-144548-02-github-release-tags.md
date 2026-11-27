# GitHub releases on the tags Forgejo pushes

- Status: Accepted
- Date: 2026-11-27 14:45 UTC
- Decided by: agent (the maintainer asked for GitHub releases equivalent to
  Forgejo's)

## Context
`.github/workflows/unawatch.yml` builds what `.forgejo/workflows/unawatch.yml`
builds (decision 261126-130734-03). A GitHub release needs a tag. The Forgejo
release job deletes and recreates the tag `unawatch-latest` on each main
build, and the mirror then pushes it to GitHub; the mirror's
`git push --mirror` also deletes any tag created only on GitHub, which turns
its release into a draft. A GitHub-only tag such as `unawatch-main` would
therefore not last.

## Decision
GitHub releases use only tags that come from Forgejo, and the workflow never
creates or moves one (`gh release create --verify-tag`):

- `unawatch-latest`: when the mirror pushes the moved tag, the workflow
  builds the commit it points at and replaces the GitHub prerelease of that
  tag. The GitHub and Forgejo rolling releases are then of the same commit.
- `v*`: a pushed tag gets a release, as on Forgejo.

Pushes to main and pull requests build and test without publishing; the
build is kept for 14 days as a workflow artifact. Before publishing, the job
checks that the tag still points at the commit it built, and skips if not.
Any release of the tag is deleted first, drafts included.

## Consequences
Each main commit is built twice on GitHub, for the push and for the tag;
GitHub's runners are free for a public repository. The rolling release
follows Forgejo's: if Forgejo's build fails, the tag does not move and
neither does the GitHub release. A mirror sync that catches the tag deleted
makes the release a draft until the recreated tag's run replaces it.
