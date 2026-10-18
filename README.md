# Hashwarp

[![Releases](https://img.shields.io/github/downloads/ParallaxProtocol/hashwarp/total.svg)][Releases]
[![Build](https://github.com/ParallaxProtocol/hashwarp/actions/workflows/build.yml/badge.svg?branch=main)][Build]

> The official GPU miner for [Parallax](https://parallaxprotocol.org) (XHash): open source, no fee.

Hashwarp mines Parallax on NVIDIA GPUs (CUDA) and on AMD, Intel and NVIDIA GPUs (OpenCL), on pools
or solo on your own node.

* **Pools and solo**: stratum (EthProxy, EthereumStratum/1.0.0 NiceHash and 2.0.0) with TLS and
  failover pools, or getwork on a Parallax node
* **Easy to start**: a short wizard on first run, `--pool`/`--wallet` options, JSON configuration files
* **Clear output**: hashrate with 1 and 15 minute averages, effective (pool side) hashrate, a per GPU
  table with temperatures, fans, power and efficiency, one line per share with its difficulty,
  latency and the pool's reason when rejected, keys to pause GPUs or show statistics
* **Safe**: temperature limits per GPU (core, memory or hotspot), a shutdown temperature, a watchdog
  that restarts failed GPUs and exits with a distinct code when a GPU hangs, every share checked on
  the CPU before it is submitted, `--selftest` to check GPUs offline
* **Monitoring**: a web dashboard, a JSON API and Prometheus metrics
* **Rig operating systems**: HiveOS and mmpOS packages, a Docker image

## Table of Contents

* [Install](#install)
* [Quick start](#quick-start)
* [Configuration](#configuration)
* [Monitoring](#monitoring)
* [Safety](#safety)
* [Exit codes](#exit-codes)
* [Build](#build)
* [License](#license)

## Install

Download the archive for your operating system from [Releases] and extract it. It contains the
`hashwarp` executable, a sample configuration (`hashwarp.json.example`) and start scripts.

* **NVIDIA**: driver 525 or newer
* **AMD**: the AMD driver with OpenCL (ROCm or Adrenalin)
* **Linux**: glibc 2.28 or newer (Ubuntu 20.04, Debian 10, RHEL 8 and later, current HiveOS and
  mmpOS images); the other libraries are built in
* **Windows**: Windows 10 or newer; the Visual C++ runtime comes in the archive

## Quick start

Run `hashwarp` without arguments: it asks whether you mine on a pool or solo, saves the answers in
`hashwarp.json` and starts mining. Next time it starts with that configuration.

Or give the options on the command line:

```sh
# On a pool (Parallax pools: see docs/MINING.md or https://miningpoolstats.stream/parallax)
hashwarp --pool pool.example.com:3333 --wallet 0xYourParallaxAddress --worker rig1

# TLS, and a failover pool
hashwarp --pool pool.example.com:4443,backup.example.com:4443 --tls --wallet 0xYourParallaxAddress

# Solo on your own node, started with
#   parallaxd --http --http.api eth,net,web3,miner --mine --miner.coinbase 0xYourParallaxAddress
hashwarp --solo
```

`hashwarp --list-devices` lists the GPUs, `hashwarp --selftest` checks them, `hashwarp --help` shows
every option. See [docs/MINING.md](docs/MINING.md) for pools, solo mining, GPU selection and tuning.

While mining, press `h` for the keys: `s` statistics, `p`/`r` pause/resume all GPUs, `0`-`9`
pause/resume one GPU, `c` connection details, `l` debug output, `q` quit.

## Configuration

Every option can be given on the command line or in a JSON file, under the option's name without
the leading dashes. Options on the command line override the file.

```json
{
    "pool": ["pool.example.com:3333"],
    "wallet": "0xYourParallaxAddress",
    "worker": "rig1",
    "temp-stop": [85],
    "api-bind": "127.0.0.1:21550"
}
```

```sh
hashwarp --config hashwarp.json        # explicit file
hashwarp                               # hashwarp.json next to the executable or in the working directory
hashwarp --pool ... --save-config hashwarp.json   # write the options given to a file
```

Options that take one value per GPU (tuning, temperature limits) accept a comma separated list in
the order of `--gpu-id` (or of `--list-devices`); a single value applies to all GPUs.

## Monitoring

`--api-bind 127.0.0.1:21550` serves a dashboard on http://127.0.0.1:21550, a JSON summary on
`/api/v1/summary` and Prometheus metrics on `/metrics`. Add `--api-token` to require a token and
`--api-allow-control` to allow pausing, resuming and restarting from the API. See
[docs/API.md](docs/API.md).

## Safety

* `--temp-stop 85 --temp-resume 70` pauses a GPU at 85°C and resumes it at 70°C;
  `--mem-temp-stop` does the same with the memory (or hotspot) temperature
* `--shutdown-temp 95` stops hashwarp if any GPU reaches 95°C
* the watchdog restarts a GPU whose mining failed (`--watchdog-restarts` times per hour) and exits
  when a GPU stops responding for `--watchdog-timeout` seconds, optionally running
  `--watchdog-script` first (e.g. to reboot the rig)
* every solution is checked on the CPU before it's submitted; incorrect ones (often from too much
  overclocking) are reported per GPU

## Exit codes

| Code | Meaning |
| ---- | ------- |
| 0 | Normal exit (or `--selftest` passed) |
| 1 | Invalid options (or `--selftest` failed) |
| 2 | Runtime error, e.g. no usable GPU |
| 5 | Watchdog: a GPU kept failing or stopped responding |
| 6 | A GPU reached `--shutdown-temp` |

## Build

See [docs/BUILD.md](docs/BUILD.md). Builds of the `main` branch are attached to its
[workflow runs][Build].

## License

Licensed under the [GNU General Public License, Version 3](LICENSE). Hashwarp started as a fork of
[ethminer](https://github.com/ethereum-mining/ethminer).

[Build]: https://github.com/ParallaxProtocol/hashwarp/actions/workflows/build.yml
[Releases]: https://github.com/ParallaxProtocol/hashwarp/releases
