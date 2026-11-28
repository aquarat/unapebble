#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0
#
# The watch for a BLE test session, under the tether lock ($UNA_TETHER_LOCK) that the agents and
# people sharing the watch take, through bridge.py's ports ($UNA_TETHER_TCP, $UNA_CONSOLE_TCP).
#
#   watch.sh flash IMAGE [SECONDS]   flash a slot image, boot it, log the console SECONDS (20)
#   watch.sh run CMD...              run CMD under the lock, e.g. a client of this directory
#   watch.sh measure SECONDS LABEL   `power measure`, then its result and the CPU's states
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
tether=$here/../tether
lock=${UNA_TETHER_LOCK:-$HOME/Projects/unapebble-firmware/.tether.lock}
tcp=${UNA_TETHER_TCP:-127.0.0.1:4500}
con=${UNA_CONSOLE_TCP:-127.0.0.1:4501}

locked() { flock -w 900 "$lock" "$@"; }

case ${1:-} in
flash)
    img=$2 secs=${3:-20}
    locked bash -c '
        timeout 300 python3 "$0/tether.py" --tcp "$1" flash "$3" --boot 2>&1 | tr "\r" "\n" | tail -1
        sleep 1
        timeout $(($4 + 30)) python3 "$0/console.py" --tcp "$2" -t 20 --follow --for "$4" || true
    ' "$tether" "$tcp" "$con" "$img" "$secs"
    ;;
run)
    shift
    locked "$@"
    ;;
measure)
    secs=$2 label=$3
    locked bash -c '
        timeout 30 python3 "$0/console.py" --tcp "$1" "power measure $2 $3" || true
        sleep $(($2 + 45))
        for i in $(seq 1 20); do
            out=$(timeout 20 python3 "$0/console.py" --tcp "$1" power 2>&1) && break
            sleep 5
        done
        echo "$out"
        timeout 20 python3 "$0/console.py" --tcp "$1" "sys cpustats" || true
    ' "$tether" "$con" "$secs" "$label"
    ;;
*)
    sed -n '4,11p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
