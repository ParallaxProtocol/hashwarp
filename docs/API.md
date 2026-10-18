# Hashwarp API

Hashwarp serves a web dashboard, a JSON API, Prometheus metrics and a JSON-RPC interface on one TCP
port. It's off by default.

* [Enabling the API](#enabling-the-api)
* [Dashboard](#dashboard)
* [HTTP API](#http-api)
* [Prometheus](#prometheus)
* [JSON-RPC](#json-rpc)

## Enabling the API

| Option | Meaning |
| ------ | ------- |
| `--api-bind address:port` | Listen on this address and port, e.g. `127.0.0.1:21550` (this computer only) or `0.0.0.0:21550` (every network interface) |
| `--api-token secret` | Require this secret: `Authorization: Bearer secret` on HTTP, `api_authorize` on JSON-RPC |
| `--api-allow-control` | Allow clients to pause, resume and restart the miners and to change pools |

Without `--api-allow-control` the API is read only. Control on an address other than loopback
needs `--api-token`: anyone able to change pools could send your hashrate elsewhere. The API has no
TLS: the token travels in clear text, so prefer a VPN or SSH tunnel over exposing it to the
internet.

Ethminer's `--api-port N` still works and listens on `127.0.0.1:N` (read only).

## Dashboard

Open `http://127.0.0.1:21550/` in a browser: total and per GPU hashrate, averages, effective
hashrate, shares, temperatures, fans, power and efficiency, refreshed every 5 seconds. With
`--api-allow-control` it also has pause, resume and restart buttons. If a token is set the page asks
for it and keeps it for the browser session.

## HTTP API

### GET /api/v1/summary

```sh
curl -s http://127.0.0.1:21550/api/v1/summary
curl -s -H "Authorization: Bearer secret" http://127.0.0.1:21550/api/v1/summary
```

```json
{
    "control": false,
    "miner": { "name": "hashwarp", "version": "2.0.0", "algorithm": "xhash", "uptime": 3600 },
    "pool": {
        "connected": true,
        "host": "pool.example.com:3333",
        "url": "stratum+tcp://0xYourParallaxAddress.rig1:***@pool.example.com:3333",
        "difficulty": 4294967296.0,
        "epoch": 83,
        "block": -1,
        "switches": 1
    },
    "hashrate": { "now": 115480000.0, "60s": 115400000.0, "15m": 115210000.0, "effective": 112300000.0 },
    "shares": { "accepted": 42, "rejected": 1, "failed": 0 },
    "gpus": [
        {
            "index": 0,
            "name": "NVIDIA GeForce RTX 4090",
            "pci": "01:00.0",
            "backend": "cu",
            "hashrate": { "now": 115480000.0, "60s": 115400000.0, "15m": 115210000.0 },
            "shares": { "accepted": 42, "rejected": 1, "failed": 0 },
            "paused": false,
            "pause_reason": "",
            "temperature": { "core": 65, "hotspot": 0, "memory": 0 },
            "fan": 70,
            "power": 280.5,
            "power_limit": 450.0,
            "clocks": { "core": 2790, "memory": 10501 }
        }
    ]
}
```

* hashrates are in hashes per second; `effective` is what the accepted shares of the last 15
  minutes are worth, 0 during the first 2 minutes
* `difficulty` is the expected number of hashes per share
* `block` is the height being mined, -1 when the pool doesn't send it
* `failed` counts solutions the CPU found incorrect (not submitted)
* sensors are 0 when the GPU doesn't report them; `pause_reason` is empty while mining
* the pool password is never shown

### POST /api/v1/control

Needs `--api-allow-control` (403 otherwise).

```sh
curl -s -X POST -d '{"action":"pause","gpu":1}' http://127.0.0.1:21550/api/v1/control
curl -s -X POST -d '{"action":"resume"}' http://127.0.0.1:21550/api/v1/control
curl -s -X POST -d '{"action":"restart"}' http://127.0.0.1:21550/api/v1/control
```

`action` is `pause`, `resume` (one GPU with `gpu`, all without) or `restart` (all miners, as
`miner_restart`). Answers `{"result":true}`, or 400 with the reason.

### GET /getstat1

The HTML statistics page of earlier versions.

### Errors

| Status | Meaning |
| ------ | ------- |
| 400 | Malformed request or JSON body |
| 401 | Missing or wrong token |
| 403 | Control not allowed |
| 404 | Unknown path |
| 405 | Wrong method (GET or POST) |

## Prometheus

```yaml
scrape_configs:
  - job_name: hashwarp
    static_configs:
      - targets: ["rig1:21550", "rig2:21550"]
    # with --api-token:
    # authorization:
    #   credentials: secret
```

| Metric | Labels | Meaning |
| ------ | ------ | ------- |
| `hashwarp_hashrate` | `window` (`now`, `60s`, `15m`), `gpu`, `name` | Hashes per second, total (no `gpu` label) and per GPU |
| `hashwarp_effective_hashrate` | | Worth of the accepted shares |
| `hashwarp_shares_total` | `result` (`accepted`, `rejected`, `failed`), `gpu`, `name` | Shares |
| `hashwarp_gpu_paused` | `gpu`, `name` | 1 when paused |
| `hashwarp_gpu_temperature_celsius` | `gpu`, `name`, `sensor` (`core`, `hotspot`, `memory`) | Temperatures reported by the GPU |
| `hashwarp_gpu_fan_percent`, `hashwarp_gpu_power_watts`, `hashwarp_gpu_power_limit_watts` | `gpu`, `name` | Fan, power draw and limit |
| `hashwarp_gpu_clock_mhz` | `gpu`, `name`, `clock` (`core`, `memory`) | Clocks |
| `hashwarp_pool_connected` | `host` | 1 when connected |
| `hashwarp_pool_difficulty_hashes` | | Expected hashes per share |
| `hashwarp_pool_switches_total` | | Pool switches |
| `hashwarp_epoch`, `hashwarp_block_height` | | XHash epoch and height being mined |
| `hashwarp_uptime_seconds`, `hashwarp_info` | `version` | |

## JSON-RPC

Send JSON-RPC 2.0 requests, one per line, on the same port:

```sh
echo '{"id":1,"jsonrpc":"2.0","method":"miner_ping"}' | nc 127.0.0.1 21550
```

With `--api-token`, send `api_authorize` first on each connection:

```json
{"id":0,"jsonrpc":"2.0","method":"api_authorize","params":{"psw":"secret"}}
```

| Method | Params | Control | Result |
| ------ | ------ | ------- | ------ |
| `miner_ping` | | | `"pong"` |
| `miner_getstat1` | | | Claymore compatible array: version, uptime (minutes), "total kh/s;accepted;rejected", per GPU kh/s, unused, unused, "temperature;fan" pairs, pool, "failed;switches;0;0" |
| `miner_getstatdetail` | | | Detailed host, connection, mining and per GPU data |
| `miner_getconnections` | | | Pools (`index`, `active`, `uri` with the password masked) |
| `miner_getscramblerinfo` | | | Start nonce and segment width of each GPU |
| `miner_setactiveconnection` | `{"index": n}` or `{"URI": "regex"}` | yes | Switch to that pool |
| `miner_addconnection` | `{"uri": "stratum+tcp://..."}` | yes | Add a pool |
| `miner_removeconnection` | `{"index": n}` | yes | Remove a pool (not the active one) |
| `miner_pausegpu` | `{"index": n, "pause": true}` | yes | Pause or resume a GPU |
| `miner_restart` | | yes | Restart the miners (DAGs are rebuilt, the pool connection stays) |
| `miner_reboot` | | yes | Run `reboot.sh` (`reboot.bat` on Windows) next to the executable with `api_miner_reboot` as argument |
| `miner_shuffle` | | yes | Pick a new random start nonce |
| `miner_setscramblerinfo` | `{"noncescrambler": n, "segmentwidth": w}` | yes | Set the start nonce and narrow the segments (10 to 63 bits) |
| `miner_setverbosity` | `{"verbosity": n}` | yes | Debug categories (sum of 1 json, 2 per GPU, 32 connect, 64 switch, 128 submit, 256 flow) |

Methods marked "yes" need `--api-allow-control`. Errors follow JSON-RPC 2.0 (`{"error": {"code": ...,
"message": ...}}`).
