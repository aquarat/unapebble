#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Installs the firmware build environment on Ubuntu 24.04. The Dockerfile and
# CI both use this, so they can't drift apart.
#
#   docker/setup.sh apt|sdk|venv [REPO]...   run the given steps
#
# venv installs requirements.txt from REPO (default: the current directory).

set -euo pipefail

SDK_VERSION=${SDK_VERSION:-0.1.17}
SDK_SHA256_aarch64=8c7bd2cc5abc99afe4dc61b56442cafbd505c78aeb5739a86e52b5e3480aed63
SDK_SHA256_x86_64=22f9748f7a47d2b344caf9878cfebaaf266e2cfb1c7a5834d9df97278a5a80b1
PEBBLEOS_SDK_ROOT=${PEBBLEOS_SDK_ROOT:-/opt/pebbleos-sdk-$SDK_VERSION}
VIRTUAL_ENV=${VIRTUAL_ENV:-/opt/venv}

# Host packages the firmware build uses, from docs/getting_started. Left out:
# gcc-multilib (amd64 only), libgtk-3-dev and libsdl2-dev (emulator UI),
# openocd (flashing). openssl is for tools/una/pack_ota.py.
apt_step() {
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y --no-install-recommends \
        bison ca-certificates ccache clang cmake curl device-tree-compiler \
        flex gcc gettext git gperf libc6-dev libfreetype6-dev libglib2.0-dev \
        libncurses-dev librsvg2-bin libusb-1.0-0 make openssl pkg-config \
        python3-dev python3-venv xz-utils
    rm -rf /var/lib/apt/lists/*
}

# The PebbleOS SDK: arm-none-eabi toolchain, picolibc, QEMU, sftool. The
# version must satisfy SDK_VERSION; checksums come from the release.
sdk_step() {
    local arch sha name tmp
    arch=$(uname -m)
    case $arch in
        aarch64) sha=$SDK_SHA256_aarch64 ;;
        x86_64) sha=$SDK_SHA256_x86_64 ;;
        *) echo "unsupported architecture: $arch" >&2; exit 1 ;;
    esac
    name=pebbleos-sdk-$SDK_VERSION-linux-$arch
    tmp=$(mktemp -d)
    curl -fsSL --retry 3 -o "$tmp/sdk.tar.gz" \
        "https://github.com/coredevices/PebbleOS-SDK/releases/download/v$SDK_VERSION/$name.tar.gz"
    echo "$sha  $tmp/sdk.tar.gz" | sha256sum -c -
    tar xzf "$tmp/sdk.tar.gz" -C "$tmp"
    "$tmp/$name/install.sh" --defaults --prefix "$PEBBLEOS_SDK_ROOT"
    rm -rf "$tmp"
}

# requirements.txt installs packages of the tree in editable mode, so they
# resolve to whichever checkout is at REPO.
venv_step() {
    local repo=${1:-.}
    python3 -m venv "$VIRTUAL_ENV"
    "$VIRTUAL_ENV/bin/pip" install --no-cache-dir -U pip
    (cd "$repo" && "$VIRTUAL_ENV/bin/pip" install --no-cache-dir -r requirements.txt)
}

for step in "$@"; do
    case $step in
        apt) apt_step ;;
        sdk) sdk_step ;;
        venv) venv_step "${REPO:-.}" ;;
        *) echo "unknown step: $step" >&2; exit 1 ;;
    esac
done
