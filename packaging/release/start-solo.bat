@echo off
rem Solo mine on your own Parallax node. Start the node with mining enabled:
rem   parallaxd --http --http.api eth,net,web3,miner --mine --miner.coinbase ^<your address^>
rem Rewards go to the node's --miner.coinbase address.
set NODE=http://127.0.0.1:8545

cd /d "%~dp0"
hashwarp.exe --solo %NODE% %*
pause
