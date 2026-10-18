#!/usr/bin/env bash
# Runs hashwarp in the foreground; HiveOS restarts it when it exits.
cd "$(dirname "$0")" || exit 1
. ./h-manifest.conf

[[ ! -f $CUSTOM_CONFIG_FILENAME ]] && echo "No $CUSTOM_CONFIG_FILENAME, check the flight sheet" && exit 1
mkdir -p "$(dirname "$CUSTOM_LOG_BASENAME")"

# eval keeps quoted extra arguments (e.g. --watchdog-script "/path with spaces") together
eval "./hashwarp --config \"\$CUSTOM_CONFIG_FILENAME\" $(< hashwarp.args)" 2>&1 |
    tee --append "${CUSTOM_LOG_BASENAME}.log"
