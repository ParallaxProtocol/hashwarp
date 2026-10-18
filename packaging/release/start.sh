#!/bin/sh
# Mine on a pool: set POOL and WALLET below, then run ./start.sh
POOL=pool.example.com:3333
WALLET=0xYourParallaxAddress
WORKER=$(hostname)

cd "$(dirname "$0")" || exit 1
exec ./hashwarp --pool "$POOL" --wallet "$WALLET" --worker "$WORKER" "$@"
