# Build in Forgejo CI and publish .ota files as releases

- Status: Accepted
- Date: 2026-11-26 13:07 UTC
- Decided by: maintainer (CI build, downloads in the repo); agent (design)

## Context
The maintainer wanted CI to build the firmware and keep it in the repository for
easy download. Forgejo reads `.forgejo/workflows`; the runners are arm64 and
the job image has no Node. Recorded after the fact.

## Decision
`.forgejo/workflows/unawatch.yml` builds on pushes to main, `v*` tags and by
hand, with shell steps only (no JavaScript actions). A main build replaces
the rolling prerelease `unawatch-latest`; a tag gets its own release. Each
release has the `.ota`, ELF, HEX, log dictionary, resource pack and
`SHA256SUMS`.

## Consequences
Releases are the download place, not files committed to git. The rolling
release is deleted and recreated on each main build.
