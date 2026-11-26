# SPDX-License-Identifier: Apache-2.0
#
# Firmware build environment for PebbleOS, for linux/arm64 and linux/amd64.
# Build from the repository root (see docker/README.md):
#
#   docker build -f docker/Dockerfile -t unapebble-build .

FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

# Host packages and the PebbleOS SDK; CI runs the same script.
ARG SDK_VERSION=0.1.17
COPY docker/setup.sh /usr/local/bin/unapebble-setup
RUN unapebble-setup apt sdk

ENV PEBBLEOS_SDK_ROOT=/opt/pebbleos-sdk-${SDK_VERSION}

# Python dependencies. requirements.txt installs three packages of the tree
# in editable mode, so those (small) directories are copied in too, at the
# path the repository is mounted at: the editable installs then resolve to
# the bind-mounted checkout's live copies.
ENV VIRTUAL_ENV=/opt/venv
COPY requirements.txt /work/requirements.txt
COPY tools/libs/pbl-cli /work/tools/libs/pbl-cli
COPY tools/libs/pbl-console /work/tools/libs/pbl-console
COPY tools/libs/pebble-loghash /work/tools/libs/pebble-loghash
RUN cd /work && unapebble-setup venv \
    && rm -rf /work/tools/libs/*/build

# The checkout is owned by whoever runs the container (git >= 2.35.2 refuses
# it otherwise), and that user need not exist in /etc/passwd: give it a
# writable HOME and keep the compiler cache in the (git-ignored) build dir.
RUN git config --system --add safe.directory '*'
ENV PATH=$VIRTUAL_ENV/bin:$PEBBLEOS_SDK_ROOT/arm-none-eabi/bin:$PATH \
    HOME=/tmp \
    CCACHE_DIR=/work/build/.ccache

WORKDIR /work
