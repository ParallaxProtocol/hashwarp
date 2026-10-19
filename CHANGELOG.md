# Changelog

## v2.0.0

Hashwarp 2.0 is a rewrite of nearly everything around the XHash kernels: setup, output, monitoring, safety and packaging, with fixes for crashes and wrong results, overclocking, and a dashboard for one rig or many. Every option of 1.x still works.

### Download

- **Linux**: `hashwarp-v2.0.0-linux.tar.gz`, for glibc 2.28 or newer (Ubuntu 20.04, Debian 10, RHEL 8 and later). Everything else is built in: no CUDA toolkit or OpenSSL needed.
- **Windows**: `hashwarp-v2.0.0-windows.zip`, for Windows 10 or newer. The Visual C++ runtime is in the archive.
- **HiveOS**: `hashwarp-2.0.0.tar.gz` (HiveOS needs this name). In the flight sheet choose the **Custom** miner and paste this file's link as the installation URL.
- **mmpOS**: `hashwarp-2.0.0-mmpos.tgz`.

One build per system mines on **NVIDIA** GPUs with CUDA (driver 525 or newer, every GPU from Pascal to Blackwell) and on **AMD** and other GPUs with OpenCL. There are no separate CUDA and OpenCL builds anymore.

### Highlights

- **Easy to start.** Run `hashwarp` without options: a short wizard asks whether you mine on a pool or solo and saves `hashwarp.json`. Or use `--pool`, `--wallet` and `--worker`, and keep options in a JSON configuration file (`--save-config` writes one).
- **Clear output.** Each line has its time, level and source (`gpu0`, `pool`, `share` ...); a line per share with its difficulty and latency, or the pool's reason when rejected; the hashrate with its 1 and 15 minute averages and the effective (pool side) hashrate; temperatures, fan, power and efficiency; a table per GPU every minute. Keys: `h` for help, `s` statistics, `p`/`r` pause/resume, `0`-`9` one GPU. `--log-format json` writes one JSON object per line.
- **Dashboard and API.** `--api-bind 127.0.0.1:21550` serves a web dashboard, a JSON API and Prometheus metrics. `--rig` adds your other rigs to the dashboard, and `--monitor` serves that view without mining.
- **Overclocking.** Memory and core clocks, power limits and fans per GPU (NVIDIA, and AMD on Linux), restored on exit. `--oc-tune` finds each GPU's best memory clock and the most efficient core clock; mine with `--oc-profile tuned` or `efficient`.
- **Safety.** Temperature limits per GPU (core, memory or hotspot), `--shutdown-temp`, and a watchdog that restarts a failed GPU and exits with its own code when a GPU hangs. Every solution is checked on the CPU before it is submitted. `--selftest` checks the GPUs offline.
- **Good for the network.** Hashwarp warns when your pool finds most of Parallax's blocks, which lets it reverse payments, and names the smaller pools. The wizard lists such a pool last.
- **Solo mining.** `--solo` mines on your own node. With `--solo ws://127.0.0.1:8546` a node that supports it pushes each new block's work at once instead of being polled every second.
- **Rig operating systems.** Packages for HiveOS and mmpOS.

### Performance

115.5 MH/s on a stock RTX 4090, about 94% of its memory bandwidth and as fast as SRBMiner-Multi on the same card, without a dev fee: Hashwarp has none. A second CUDA kernel is timed against the standard one on each GPU, and the faster one is used.

### Fixes

- The epoch now comes from the seed hash, and pool difficulty is converted to an exact target: shares on some pools were rejected or the wrong DAG was built.
- Multi-GPU rigs on getwork and EthProxy no longer hash the same nonces twice.
- A malformed message from a pool no longer crashes Hashwarp.
- A GPU no longer stays paused for good after a failed DAG build, and out of bounds DAG writes are fixed on CUDA and OpenCL.
- GPU memory is reported per GPU (it was the first GPU's on mixed rigs).
- TLS sends the server name (SNI) and recognizes certificate errors; reconnects back off instead of hammering a pool, and a single pool is never dropped.
- Stratum: autodetection of the protocol, and pools such as rplant that answer in their own way.

### Upgrading from 1.x

- Command lines and configuration from 1.x keep working: the ethminer style options (`-P`, `-U`, `--cu-devices` ...) are still accepted.
- The output is different. If a script parses Hashwarp's log, use `--log-format json` or the API's `/api/v1/summary` instead. Hashrates are written MH/s and difficulties GH.
- The API listens only on `127.0.0.1` unless `--api-bind` says otherwise, and is read only unless `--api-allow-control` is given (with `--api-token` on other addresses). `--api-port` still works, read only.
- The Windows archive no longer has `fix-opencl-icd.vbs`: NVIDIA GPUs mine with CUDA, which needs no OpenCL registration. See [Troubleshooting Hashwarp on Windows](https://docs.parallaxprotocol.org/v2.1.x/guides/mining/troubleshooting) if an AMD GPU isn't found.

### Documentation

- [Mining with Hashwarp](https://github.com/ParallaxProtocol/hashwarp/blob/v2.0.0/docs/MINING.md): pools, solo mining, GPUs, tuning, overclocking, logs, HiveOS and mmpOS.
- [API](https://github.com/ParallaxProtocol/hashwarp/blob/v2.0.0/docs/API.md): dashboard, rigs, JSON API, Prometheus.
- [Parallax mining guides](https://docs.parallaxprotocol.org/v2.1.x/guides/mining/gpu)
