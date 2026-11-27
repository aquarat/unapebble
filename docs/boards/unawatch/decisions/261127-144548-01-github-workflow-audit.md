# Upstream GitHub workflows on the mirror: keep the builds and tests

- Status: Accepted
- Date: 2026-11-27 14:45 UTC
- Decided by: agent (within the maintainer's request to audit `.github/`)

## Context
The repository was made public on GitHub, where Forgejo push-mirrors it.
`.github/` came from upstream PebbleOS, and its workflows started running
there. Forgejo reads only `.forgejo/workflows` when that directory exists
(`ListWorkflows` in Forgejo's `modules/actions/workflows.go` takes the first
of `.forgejo/workflows`, `.gitea/workflows`, `.github/workflows` it finds),
so nothing under `.github/` runs on Forgejo. The mirror pushes with
`git push --mirror`, which deletes refs made on GitHub: dependabot's first
branch was deleted, closing its pull request, within a minute.

On the first push to GitHub the builds of upstream's boards, PRF, QEMU with
its integration tests, the SDK shell, native and wasm, the samples and the
unit tests (462, under 10 minutes) passed on GitHub-hosted runners. The
compliance checks failed on the port's own code (clang-format in about 25
files, ruff in the tether tools, devicetree schemas missing for the STM32U5
and UNA Watch bindings), and the Nix check failed to install Nix on macOS.

## Decision
Kept, with `permissions: contents: read` at the top: `build-firmware`,
`build-native`, `build-prf`, `build-qemu`, `build-qemu-sdkshell`,
`build-samples` and `test`, and the `ccache`, `ccache-save` and `rebase`
actions they use. They show whether the port breaks upstream's boards. The
steps and jobs needing upstream's secrets are removed: the log dictionary
upload to S3 and the Weblate translation sync.

Removed:

- `backport`, `pr-labels`, `hw-itest`: `pull_request_target` with write
  tokens or PR code on self-hosted hardware; upstream's labels and app.
- `release`, `nightly`, `release-qa`, `docs-playground-rtd`, the
  `publish-engdash` action: upstream's releases, eng-dash, hardware lab and
  Read the Docs, with their secrets. `v*` releases here come from
  `unawatch.yml`.
- `compliance`: runs on pull requests only, which can't be merged here,
  and fails on the port's code as it is.
- `nix`, `docs-linkcheck`, `stale-pr`: the fork maintains no Nix
  environment, links or pull requests of its own.
- `dependabot.yml`: its pull requests can't be merged on a mirror.
- `CODEOWNERS`, `labeler.yml`, `blocking-labels.yml`,
  `scripts/check-submodules.sh`, the issue templates: upstream's people,
  labels and watches.

## Consequences
Merges from upstream conflict on the removed files when upstream changes
them; they stay removed. The compliance findings are real: fixing the
formatting and adding the bindings, then running the checks in Forgejo CI,
is left to the maintainer.
