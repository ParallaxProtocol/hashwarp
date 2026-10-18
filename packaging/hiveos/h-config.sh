#!/usr/bin/env bash
# Writes hashwarp.json from the flight sheet. Sourced by HiveOS before the
# miner starts and before each statistics call: keep it quiet.
#
# Flight sheet: wallet and worker template %WAL%.%WORKER_NAME%, pool URL
# host:port (space separated for failover pools), extra config arguments are
# passed to hashwarp as they are.

[[ -z $CUSTOM_TEMPLATE ]] && echo "Set the wallet and worker template in the flight sheet" && return 1
[[ -z $CUSTOM_URL ]] && echo "Set the pool URL in the flight sheet" && return 1

hashwarp_wallet=${CUSTOM_TEMPLATE%%.*}
if [[ $CUSTOM_TEMPLATE == *.* ]]; then
    hashwarp_worker=${CUSTOM_TEMPLATE#*.}
else
    hashwarp_worker=$WORKER_NAME
fi

# shellcheck disable=SC2086 # pools are space separated
jq -n \
    --argjson pool "$(printf '%s\n' $CUSTOM_URL | jq -R . | jq -s .)" \
    --arg wallet "$hashwarp_wallet" \
    --arg worker "$hashwarp_worker" \
    --arg password "${CUSTOM_PASS:-x}" \
    --arg api "127.0.0.1:${MINER_API_PORT}" \
    '{pool: $pool, wallet: $wallet, worker: $worker, password: $password, "api-bind": $api}' \
    > "$CUSTOM_CONFIG_FILENAME"

echo "$CUSTOM_USER_CONFIG" > "$(dirname "$CUSTOM_CONFIG_FILENAME")/hashwarp.args"
