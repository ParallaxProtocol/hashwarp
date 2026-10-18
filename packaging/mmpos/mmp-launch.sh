#!/usr/bin/env bash
# mmpOS entry point. Default arguments in the miner profile:
#   ./mmp-launch.sh --coin %coin% %pool_protocol% --pool %pool_server%:%pool_port%
#     --user %user% --password %password% --api-port %api_port%
# Other arguments are passed to hashwarp as they are.
cd "$(dirname "$0")" || exit 1

args=()
api_port=21550
while [[ $# -gt 0 ]]; do
    case $1 in
        --coin) shift ;;                       # always XHash
        tcp) ;;
        tls) args+=(--tls) ;;
        --user)
            # wallet.worker
            args+=(--wallet "${2%%.*}")
            [[ $2 == *.* ]] && args+=(--worker "${2#*.}")
            shift ;;
        --api-port) api_port=$2; shift ;;
        *) args+=("$1") ;;
    esac
    shift
done

# mmp-stats.sh reads the API port from here
echo "$api_port" > api-port

exec ./hashwarp "${args[@]}" --api-bind "127.0.0.1:$api_port" --no-hotkeys
