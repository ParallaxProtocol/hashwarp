#!/bin/sh
# Solo mine on your own Parallax node. Start the node with mining enabled:
#   parallaxd --http --http.api eth,net,web3,miner --mine --miner.coinbase <your address>
# Rewards go to the node's --miner.coinbase address.
NODE=http://127.0.0.1:8545

cd "$(dirname "$0")" || exit 1
exec ./hashwarp --solo "$NODE" "$@"
