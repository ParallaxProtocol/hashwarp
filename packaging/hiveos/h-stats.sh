#!/usr/bin/env bash
# Sets khs (total kH/s) and stats (JSON) for HiveOS from hashwarp's API.
# Sourced by HiveOS.

hashwarp_summary=$(curl --connect-timeout 2 --max-time "${API_TIMEOUT:-15}" --noproxy '*' -s \
    "http://127.0.0.1:${MINER_API_PORT:-21550}/api/v1/summary")

if [[ -z $hashwarp_summary ]] || ! jq -e .gpus <<< "$hashwarp_summary" > /dev/null 2>&1; then
    khs=0
    stats="null"
else
    # PCI bus numbers in decimal ("0a:00.0" -> 10), in GPU order
    hashwarp_buses=$(jq -r '.gpus[].pci' <<< "$hashwarp_summary" | while read -r pci; do
        # bb:dd.f, or a GPU without a PCI id (reported as 0)
        if [[ $pci =~ ^([0-9a-fA-F]{2}): ]]; then echo $((16#${BASH_REMATCH[1]})); else echo 0; fi
    done | jq -s -c .)

    khs=$(jq '.hashrate.now / 1000' <<< "$hashwarp_summary")
    stats=$(jq -c --argjson bus_numbers "$hashwarp_buses" '{
        hs: [.gpus[].hashrate.now / 1000],
        hs_units: "khs",
        temp: [.gpus[].temperature.core],
        fan: [.gpus[].fan],
        uptime: .miner.uptime,
        ver: .miner.version,
        ar: [.shares.accepted, .shares.rejected, .shares.failed,
             ([.gpus[].shares.failed | tostring] | join(";"))],
        algo: "xhash",
        bus_numbers: $bus_numbers
    }' <<< "$hashwarp_summary")
fi
