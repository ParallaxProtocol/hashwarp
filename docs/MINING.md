# Mining with Hashwarp

* [Pools](#pools)
* [Solo mining](#solo-mining)
* [GPUs](#gpus)
* [Tuning](#tuning)
* [Overclocking](#overclocking)
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

### Pools with the majority

A pool that finds more than half of the blocks can reverse payments and block transactions, which
puts the coin you mine at risk. Hashwarp reads the pools' shares of the last 7 days' blocks from
[status.parallaxprotocol.org](https://status.parallaxprotocol.org) every 10 minutes. When yours
has the majority it says so at startup and then every hour, with the smaller pools:

```
05:26:47  WARN  pool   rplant.xyz found 86% of the network's blocks in the last 7 days. A pool with the majority can reverse payments and block transactions, which puts the coin you mine at risk
05:26:47  WARN  pool   Please mine on a smaller pool (coolpool.top 13% · gtpool.io 1%) or solo (--solo). https://status.parallaxprotocol.org has the figures; --no-pool-check hides this
```

The dashboard shows it too, and the first run wizard lists such a pool last. Hashwarp never changes
pools by itself. The request sends nothing about you or your rig (only hashwarp's version, as the
User-Agent); if it fails, hashwarp carries on mining without a word. `--no-pool-check` turns it off.
It doesn't run when solo mining, in simulation, `--selftest` or `--oc-tune`.

| Option | Meaning |
| ------ | ------- |
| `-o, --pool` | `host:port`, or a URL (see below). Repeat it or separate pools with commas: the next pool is used when one fails |
| `-u, --wallet` | Your Parallax address (also read from the `HASHWARP_WALLET` environment variable) |
| `-w, --worker` | The rig's name on the pool dashboard |
| `-p, --password` | Pool password, `x` by default |
| `--tls` | Connect to `host:port` pools with TLS |
| `--esm` | Stratum flavour when the pool needs one: `0` EthProxy, `1`/`2` EthereumStratum/1.0.0 (NiceHash), `3` EthereumStratum/2.0.0. By default hashwarp tries EthereumStratum/1.0.0, then EthProxy, then plain Stratum; 2.0.0 only with `--esm 3` |
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

### New work without polling: WebSocket

Over the node's WebSocket, the node pushes each new block's work to hashwarp the moment it has it
(`eth_subscribe` to `newWork`), instead of hashwarp asking every second, and blocks go back on the
same connection:

```sh
parallaxd --ws --ws.api eth,net,web3,miner --mine --miner.coinbase 0xYourParallaxAddress
hashwarp --solo ws://127.0.0.1:8546
```

`wss://` works with a node behind a TLS proxy. A node without the `newWork` subscription is asked
for work on the same connection every `--getwork-interval` milliseconds; hashwarp says so when it
connects.

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

## Overclocking

XHash is limited by the memory: a higher memory clock raises the hashrate, while the core clock can
usually go well below its default without costing anything, which saves power. Hashwarp sets both
itself, on NVIDIA GPUs (Linux and Windows) and AMD GPUs (Linux), and puts the GPU back to its
previous settings when it exits.

Changing them needs root on Linux (`sudo hashwarp ...`) or administrator rights on Windows. Rig
operating systems that overclock themselves (HiveOS, mmpOS) don't need these options.

### Finding the settings: --oc-tune

```sh
sudo hashwarp --oc-tune         # memory and core clocks, 10 to 20 minutes
sudo hashwarp --oc-tune=core    # only the core clock, which is safe, about 5 minutes
```

The tuner mines offline at an easy difficulty and checks every result on the CPU. It raises the
memory clock step by step while the results stay correct and the hashrate grows (GDDR6X memory
corrects its own errors, which shows as a hashrate that falls again), keeps the best value or the
one below it, then lowers the core clock as far as the hashrate stays within 1%. The results go to
`hashwarp-oc.json` next to the configuration file (`--oc-tune-file`), per GPU, and are used with:

```sh
sudo hashwarp --oc-profile tuned --pool ... --wallet ...
```

Tune with the GPUs otherwise idle: a desktop, video or game on the same GPU makes the hashrate
unsteady, which the tuner notices and measures longer for, but its results are then less precise.
`--oc-tune=core` keeps a memory clock tuned before. A memory clock that is too high can hang a GPU:
the watchdog then exits hashwarp (exit code 5), which restores the settings.

### Profiles

`--oc-profile` takes one profile per GPU, or one for all:

| Profile | Settings |
| ------- | -------- |
| `tuned` | The `--oc-tune` results of the GPU |
| `efficient` | The tuned core clock, never the memory. On GPUs that weren't tuned, 80% of the default power limit |
| `stock` | Default clocks, power limit and fan control |

Your own profiles go into the configuration file, and can be selected per GPU:

```json
{
    "oc-profile": ["fast", "fast", "quiet"],
    "oc-profiles": {
        "fast": {"core-clock": 1650, "mem-offset": 2500, "power": 320},
        "quiet": {"core-clock": 1400, "power": "70%", "fan": 60}
    }
}
```

### Settings

These take one value per GPU, or one for all; `x` leaves a GPU alone (`--oc-mem-offset 2000,x,1500`).
They override the profile's values.

| Option | Meaning |
| ------ | ------- |
| `--oc-core-clock` | Core clock in MHz: locked on NVIDIA, highest state on AMD. 0 = default |
| `--oc-core-offset` | Core clock offset in MHz (NVIDIA) |
| `--oc-mem-offset` | Memory clock offset in MHz (NVIDIA), in the units of nvidia-settings and HiveOS, which are twice MSI Afterburner's: Afterburner's +1000 is +2000 |
| `--oc-mem-clock` | Memory clock of the highest state in MHz (AMD). 0 = default |
| `--oc-power` | Power limit in W, or in percent of the default (`75%`) |
| `--oc-fan` | Fan speed in percent, or `auto` |

AMD GPUs only accept clock changes with overdrive enabled: add `amdgpu.ppfeaturemask=0xffffffff` to
the kernel command line. If hashwarp is killed (`kill -9`, power loss) it can't restore the
settings: reboot, or run `sudo hashwarp --oc-reset`.

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

Each line has the time, the level, the source and the message. The source is `gpu0`, `gpu1` ...
for a GPU (each GPU's in its own color), `pool` (or `node` when solo mining), `share`, `block`,
`miner`, `api`, `hwmon`, `oc` or `tune`:

```
12:00:00  INFO  pool   Connected to eu.rplant.xyz:17159
12:00:00  INFO  pool   Protocol EthereumStratum/1.0.0
12:00:01  INFO  gpu0   NVIDIA GeForce RTX 4090 · PCI 01:00.0 · compute 8.9 · 23.59 GB
12:00:01  INFO  gpu0   DAG epoch 83 (1.00 GB) built in 235 ms · 20.59 GB free
12:00:04  INFO  share  ✔ accepted gpu0 · diff 4.29 GH · 42 ms · A1
12:00:10  INFO  miner  115.48 MH/s · 60s 115.40 · 15m 115.21 · eff 112.30 · A42 R1
12:00:10  INFO  gpu0   115.48 MH/s · 65°C · fan 70% · 280 W · 412 kH/J
```

Every 10 seconds (`--display-interval`) hashwarp prints the current speed, the 1 and 15 minute
averages, the effective hashrate (what the accepted shares of the last 15 minutes are worth, from
2 minutes on) and the accepted (`A`), rejected (`R`) and wrong (`F`) shares, then a line per GPU
with its speed, temperature, fan, power and efficiency. With one GPU it all fits on one line.
Every 60 seconds (`--stats-interval`) a table per GPU follows, with the clocks too, the pool, its
difficulty and the blocks left before the next epoch.

| Option | Meaning |
| ------ | ------- |
| `--log-file hashwarp.log` | Also write the log to a file, with ISO 8601 times and without colors |
| `--log-max-size 20`, `--log-keep 3` | Rotate the file at 20 MB, keep 3 old ones |
| `--log-level` | `error`, `warn`, `info` (default), `debug` or `trace`. Debug lines also name the thread |
| `--log-format json` | One JSON object per line, on the console and in the log file (see below) |
| `--log-debug json,per-gpu` | Extra debug output: stratum `json` messages, `per-gpu` solutions |
| `--no-color`, `--syslog`, `--stdout` | No colors; for syslog and journald (no time, and the level as a `<N>` priority); log to stdout instead of stderr |

On a console without UTF-8 (`LANG` not set to a UTF-8 locale) the symbols are replaced by ASCII:
`|` for `·`, `+` for `✔`, `x` for `✘`.

### JSON lines

`--log-format json` writes each line as an object with `time`, `level`, `source` and `msg` (the
text above, without colors), for log collectors and scripts:

```json
{"time":"2026-10-18T12:00:04.512+02:00","level":"info","source":"share","msg":"✔ accepted gpu0 · diff 4.29 GH · 42 ms · A1","event":"share","result":"accepted","gpu":0,"difficulty":4294967296,"ms":42}
```

The lines worth reading by a program also have an `event` and its values:

| `event` | Values |
| ------- | ------ |
| `connected`, `disconnected` | `host` |
| `difficulty` | `epoch`, `difficulty` (hashes per share) |
| `job` (debug level) | `block` |
| `share` | `result` (`accepted`, `stale` or `rejected`), `gpu`, `difficulty`, `ms`, `reason` |
| `block` | `gpu`, `height`, `ms` |
| `hashrate` | `hashrate`, `hashrate60`, `hashrate900` (H/s), `accepted`, `rejected`, `failed`, `effective` |
| `gpu` | `gpu`, `hashrate`, `paused`, `temp` (°C), `fan` (%), `power` (W) |
| `tune_step`, `tuned` | `--oc-tune`'s measurements and results |

With `--log-level debug` the lines also have the `thread`.

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
| `Rusticl warning` or `pci id for fd` lines at start (Linux) | Printed by Mesa's OpenCL driver, harmless. Mine with `--cuda` on NVIDIA only rigs, or set `OCL_ICD_VENDORS` to the vendor's `.icd` file (e.g. `nvidia.icd`) |
