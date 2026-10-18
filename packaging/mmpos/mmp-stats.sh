#!/usr/bin/env bash
# Prints hashwarp's statistics for mmpOS. Called as mmp-stats.sh <DEVICE_COUNT> <LOG_FILE>
cd "$(dirname "$0")" || exit 1
. ./mmp-external.conf

port=$(cat api-port 2>/dev/null || echo 21550)
summary=$(curl --connect-timeout 2 --max-time 10 --noproxy '*' -s "http://127.0.0.1:$port/api/v1/summary")
[[ -z $summary ]] && exit 1

# PCI bus numbers in decimal ("0a:00.0" -> 10), in GPU order
buses=$(jq -r '.gpus[].pci' <<< "$summary" | while read -r pci; do
    # bb:dd.f, or a GPU without a PCI id (reported as 0)
    if [[ $pci =~ ^([0-9a-fA-F]{2}): ]]; then echo $((16#${BASH_REMATCH[1]})); else echo 0; fi
done | jq -s -c .)

jq -c --argjson busid "$buses" --arg name "$EXTERNAL_NAME" '{
    busid: $busid,
    hash: [.gpus[].hashrate.now / 1000],
    units: "khs",
    air: [(.shares.accepted | tostring), (.shares.failed | tostring), (.shares.rejected | tostring)],
    temp: [.gpus[].temperature.core],
    watt: [.gpus[].power | floor],
    miner_name: $name,
    miner_version: .miner.version
}' <<< "$summary"
