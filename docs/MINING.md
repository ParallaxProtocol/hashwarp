# Mining with Hashwarp

* [Pools](#pools)
* [Solo mining](#solo-mining)
* [GPUs](#gpus)
* [Tuning](#tuning)
* [Temperatures and the watchdog](#temperatures-and-the-watchdog)
* [Logs and output](#logs-and-output)
* [HiveOS and mmpOS](#hiveos-and-mmpos)
* [Troubleshooting](#troubleshooting)

## Pools

Parallax pools are listed on [miningpoolstats](https://miningpoolstats.stream/parallax). These
take your Parallax address as the user name (checked in October 2026):

| Pool | Scheme, fee | Address | TLS address |
| ---- | ----------- | ------- | ----------- |
| [rplant.xyz](https://pool.rplant.xyz) | PPLNS 1% | `stratum-<region>.rplant.xyz:7159` | `stratum-<region>.rplant.xyz:17159` |
| [coolpool.top](https://coolpool.top/pool/prlx) | PPLNS 1% | `prlx.coolpool.top:3003` | |
| [coolpool.top](https://coolpool.top/pool/prlxsolo) | SOLO 1.5% | `prlx.coolpool.top:3002` | |
| [cedric-crispin.com](https://parallax.cedric-crispin.com) | PPLNS 0.1% | `parallax.cedric-crispin.com:4554` | |
| [pool.ion.cool](https://pool.ion.cool/parallax) | PPLNS 1% | `pool.ion.cool:33350` | |

rplant regions are `eu`, `na`, `asia` and `ru`; coolpool also has `eu.`, `us.` and `asia.`
hosts. [gtpool.io](https://gtpool.io) takes its own account ID instead of an address. The first
run wizard offers these pools. Give the pool's host and port, your Parallax address and a name
for the rig:

```sh
hashwarp --pool pool.example.com:3333 --wallet 0xYourParallaxAddress --worker rig1
```

| Option | Meaning |
| ------ | ------- |
| `-o, --pool` | `host:port`, or a URL (see below). Repeat it or separate pools with commas: the next pool is used when one fails |
| `-u, --wallet` | Your Parallax address (also read from the `HASHWARP_WALLET` environment variable) |
| `-w, --worker` | The rig's name on the pool dashboard |
| `-p, --password` | Pool password, `x` by default |
| `--tls` | Connect to `host:port` pools with TLS |
| `--esm` | Stratum flavour when the pool needs one: `0` EthProxy, `1`/`2` EthereumStratum/1.0.0 (NiceHash), `3` EthereumStratum/2.0.0. By default hashwarp detects it |
| `--failover-timeout` | Minutes on a failover pool before trying the first one again |
| `--retry-delay` | Seconds before reconnecting (doubles on each failed attempt, up to 120). A single pool is retried forever |

Pools can also be given as URLs, which is how ethminer took them (`-P` still works):

```sh
hashwarp --pool stratum+ssl://pool.example.com:4443 --wallet 0xYourParallaxAddress
hashwarp -P stratum2+tcp://0xYourParallaxAddress.rig1@pool.example.com:3333
```

The scheme is `stratum`, `stratum1` (EthProxy), `stratum2` (EthereumStratum/1.0.0) or `stratum3`
(EthereumStratum/2.0.0) followed by `+tcp`, or `+ssl` / `+tls` for TLS. If the URL has no wallet,
`--wallet` and `--worker` are added.

Pool difficulty is in units of 2^32 hashes, the same convention the pools use: difficulty 1 means
a share every 4.29 billion hashes on average.

## Solo mining

Solo mining asks your own node for work (getwork) and submits blocks to it. The block reward goes
to the node's `--miner.coinbase` address, not to an address given to hashwarp.

Start the node with the mining API enabled:

```sh
parallaxd --http --http.api eth,net,web3,miner --mine --miner.coinbase 0xYourParallaxAddress
```

then:

```sh
hashwarp --solo                          # node on http://127.0.0.1:8545
hashwarp --solo http://192.168.1.10:8545 # node on another computer
```

Each block found is logged as `BLOCK FOUND` with its height. Keep the node's HTTP port on your local
network: getwork has no authentication. `--getwork-interval` sets how often (in milliseconds) the
node is polled for new work.

You can combine solo mining with failover pools: `hashwarp --solo --pool pool.example.com:3333
--wallet 0x...` mines on the pool while the node is unreachable.

## GPUs

```sh
hashwarp --list-devices
```

```
Id  Pci Id    Backend Name                                Memory  Details
--  ------    ------- ----                                ------  -------
0   01:00.0   cu,cl   NVIDIA GeForce RTX 4090           23.59 GB  SM 8.9, NVIDIA CUDA OpenCL 3.0 CUDA
```

| Option | Meaning |
| ------ | ------- |
| `--gpu-id 0,2` | Mine on these GPUs only (ids from `--list-devices`) |
| `--backend` | `auto` (CUDA on NVIDIA GPUs, OpenCL on the others), `cuda` or `opencl`. `-U` and `-G` are shortcuts |
| `--disable-gpu-nvidia`, `--disable-gpu-amd`, `--disable-gpu-intel` | Skip GPUs of a vendor |

The XHash DAG grows by 8 MB every epoch (720 blocks, about 5 days): around 1.7 GB in late 2026,
2 GB around mid 2027. GPUs need that much free memory.

`hashwarp --selftest` mines offline for up to 30 seconds and checks every GPU's results on the CPU:
use it after changing drivers or overclocking. It exits with 0 if all GPUs are correct.

## Tuning

These options take one value per GPU (in `--gpu-id` order) or a single value for all:

| Option | Default | Meaning |
| ------ | ------- | ------- |
| `--cuda-grid-size` | 8192 | CUDA blocks per kernel launch |
| `--cuda-block-size` | 128 | CUDA threads per block (32, 64, 128 or 256) |
| `--cuda-streams` | 2 | CUDA kernels in flight |
| `--cl-global-work` | 65536 | OpenCL global work size multiplier |
| `--cl-local-work` | 128 | OpenCL work group size (64, 128 or 256) |

`--cuda-schedule` (`sync` by default) sets how the CPU waits for CUDA: `spin` lowers latency a bit at
the cost of a CPU core per GPU.

## Temperatures and the watchdog

Hardware monitoring is on by default (`--no-hwmon` turns it off): NVML on NVIDIA GPUs, sysfs on
Linux for AMD GPUs (core, junction and memory temperatures, fan, power, clocks). On Windows AMD GPUs
are read through ADL, which only reports temperature, fan and power of GPUs older than Vega.

| Option | Meaning |
| ------ | ------- |
| `--temp-stop 85` | Pause a GPU when its core reaches 85°C (per GPU list) |
| `--temp-resume 70` | Resume it at 70°C (default: stop - 15) |
| `--mem-temp-stop 100` | Pause on the memory temperature, or the hotspot on GPUs without a memory sensor |
| `--mem-temp-resume 90` | Default: stop - 10 |
| `--shutdown-temp 95` | Stop hashwarp (exit code 6) when any temperature reaches 95°C |
| `--watchdog-timeout 120` | Exit (code 5) when a GPU gives no sign of life for 120 seconds (0 = off) |
| `--watchdog-restarts 3` | Restart a GPU whose mining failed up to 3 times per hour, then exit (code 5) |
| `--watchdog-script` | Program run (with the GPU index and the reason) before exiting, e.g. to reboot |

Run hashwarp under a supervisor (a rig OS, systemd with `Restart=on-failure`, a loop in a script)
so it's started again after a watchdog exit.

## Logs and output

Every 10 seconds (`--display-interval`) hashwarp prints a summary line:

```
 m 12:00:10 hashwarp 115.48 Mh/s (60s 115.40, 15m 115.21) eff 112.30 A42 R1 - cu0 115.48 65C
```

current speed, 1 and 15 minute averages, effective hashrate (what the accepted shares of the last
15 minutes are worth, from 2 minutes on), accepted and rejected shares and each GPU's speed and
temperature. Every 60 seconds (`--stats-interval`) a table per GPU follows, with temperatures, fan,
power, efficiency (kh/W) and clocks, the pool, its difficulty and the blocks left before the next
epoch.

| Option | Meaning |
| ------ | ------- |
| `--log-file hashwarp.log` | Also write the log to a file, with full dates and without colors |
| `--log-max-size 20`, `--log-keep 3` | Rotate the file at 20 MB, keep 3 old ones |
| `--log-level` | `error`, `warn`, `info` (default), `debug` or `trace` |
| `--log-debug json,per-gpu` | Extra debug output: stratum `json` messages, `per-gpu` solutions |
| `--no-color`, `--syslog`, `--stdout` | Plain output, output for syslog, log to stdout instead of stderr |

## HiveOS and mmpOS

Each Linux release has a HiveOS package, `hashwarp-<version>.tar.gz`, and an mmpOS package,
`hashwarp-<version>-mmpos.tgz`. They report hashrate, shares, temperatures and fans to the rig OS
through hashwarp's API, on 127.0.0.1 only.

**HiveOS**: in the flight sheet choose the **Custom** miner, then

* Miner name: `hashwarp`
* Installation URL: the download URL of `hashwarp-<version>.tar.gz` from [Releases](https://github.com/ParallaxProtocol/hashwarp/releases)
* Hash algorithm: `xhash`
* Wallet and worker template: `%WAL%.%WORKER_NAME%`
* Pool URL: `pool.example.com:3333`, or several separated by spaces for failover
* Pass: the pool password, if any
* Extra config arguments: any hashwarp option, e.g. `--tls --temp-stop 85`

**mmpOS**: create a miner profile with the **Custom miner** type, give the download URL of
`hashwarp-<version>-mmpos.tgz` and a pool. Extra hashwarp options can be appended to the
arguments.

Both restart hashwarp when it exits, which is what the [watchdog](#temperatures-and-the-watchdog)
relies on.

## Troubleshooting

| Problem | What to do |
| ------- | ---------- |
| `No usable mining devices found` on Windows | The OpenCL runtime isn't registered, see [Troubleshooting Hashwarp on Windows](https://docs.parallaxprotocol.org/v2.1.x/guides/mining/troubleshooting) |
| `GPU n gave incorrect result` | Lower the memory overclock of that GPU, then check with `hashwarp --selftest` |
| Rejected shares with a `low difficulty` reason | Make sure the pool's stratum flavour is detected (`--esm`), and report it with `--log-debug json` output |
| `Epoch N requires ... memory` | The GPU doesn't have enough free memory for the DAG |
| Hashrate drops to 0 and `paused` | A temperature limit was reached (see the warning above it) |
