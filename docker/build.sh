#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Build the firmware for a board inside the build container.
#
#   BOARD=asterix [BUILD_DIR=build] docker/build.sh [extra pbl configure args...]
#   docker/build.sh -- <command...>      run any command in the container
#
# Run from the repository root. REBUILD=1 rebuilds the image.

set -euo pipefail

IMAGE=${IMAGE:-unapebble-build:latest}
cd "$(git rev-parse --show-toplevel)"

# Plain docker if this user may use it, sudo docker otherwise.
docker=(docker)
docker info >/dev/null 2>&1 || docker=(sudo docker)

if [[ -n ${REBUILD:-} ]] || ! "${docker[@]}" image inspect "$IMAGE" >/dev/null 2>&1; then
    "${docker[@]}" build --cgroup-parent=batch.slice -f docker/Dockerfile -t "$IMAGE" .
fi

# In a git worktree, .git points into the main repository's git directory:
# mount that too, read-only, at the same path, so git describe works.
gitdirs=()
common=$(git rev-parse --path-format=absolute --git-common-dir)
if [[ $common != "$PWD/.git" ]]; then
    gitdirs=(-v "$common:$common:ro")
fi

run() {
    "${docker[@]}" run --rm --init --cgroup-parent=batch.slice \
        --user "$(id -u):$(id -g)" \
        --security-opt label=disable \
        -v "$PWD:/work" -w /work "${gitdirs[@]}" \
        $([[ -t 0 ]] && echo -it) \
        "$IMAGE" "$@"
}

if [[ ${1:-} == -- ]]; then
    shift
    run "$@"
else
    : "${BOARD:?set BOARD, e.g. BOARD=asterix $0}"
    dir=${BUILD_DIR:-build}
    run bash -c 'dir=$0 board=$1; shift
        pbl configure -b "$dir" --board "$board" "$@" && pbl build -b "$dir"' "$dir" "$BOARD" "$@"
fi
