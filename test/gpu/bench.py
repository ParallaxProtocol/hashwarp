"""Benchmark hashwarp: offline hashrate (and NVIDIA power) of one configuration.

Runs hashwarp in simulation mode on the DAG of --block, ignores the first
--warmup seconds, then averages the 5 second hashrate samples and the power
reported by nvidia-smi.

  python3 test/gpu/bench.py build/hashwarp/hashwarp cuda
  python3 test/gpu/bench.py build/hashwarp/hashwarp cuda --seconds 90 -- --cuda-grid-size 16384
"""
import argparse
import re
import signal
import statistics
import subprocess
import sys
import threading
import time

ANSI = re.compile(r"\x1b\[[0-9;]*m")
SAMPLE = re.compile(r" m \S+ \S+ +([\d.]+) (h|Kh|Mh|Gh)/s \(60s")
SCALE = {"h": 1e-6, "Kh": 1e-3, "Mh": 1.0, "Gh": 1e3}


def power_sampler(stop, samples, gpu):
    while not stop.is_set():
        try:
            out = subprocess.run(["nvidia-smi", "-i", str(gpu), "--query-gpu=power.draw",
                                  "--format=csv,noheader,nounits"],
                                 capture_output=True, text=True, timeout=5).stdout.strip()
            samples.append((time.time(), float(out)))
        except (OSError, ValueError, subprocess.SubprocessError):
            pass
        stop.wait(2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("miner")
    ap.add_argument("backend", choices=["cuda", "opencl"])
    ap.add_argument("--seconds", type=int, default=60, help="measured time, after the warmup")
    ap.add_argument("--warmup", type=int, default=20)
    ap.add_argument("--block", type=int, default=60480)
    ap.add_argument("--gpu", type=int, default=0, help="nvidia-smi index for power")
    ap.add_argument("extra", nargs="*", help="extra hashwarp options (after --)")
    args = ap.parse_args()

    cmd = [args.miner, "-U" if args.backend == "cuda" else "-G", "-M", str(args.block),
           "--display-interval", "5", "--stats-interval", "0", "--no-hotkeys",
           "--watchdog-timeout", "0"] + args.extra
    start = time.time()
    miner = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    stop = threading.Event()
    power = []
    sampler = threading.Thread(target=power_sampler, args=(stop, power, args.gpu), daemon=True)
    sampler.start()

    rates = []
    lines = []

    def reader():
        for line in miner.stdout:
            line = ANSI.sub("", line)
            lines.append(line)
            m = SAMPLE.search(line)
            if m and time.time() - start >= args.warmup:
                rates.append(float(m.group(1)) * SCALE[m.group(2)])

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    time.sleep(args.warmup + args.seconds)
    measured_from = start + args.warmup
    miner.send_signal(signal.SIGINT)
    try:
        miner.wait(30)
    except subprocess.TimeoutExpired:
        miner.kill()
    stop.set()
    t.join(5)

    watts = [w for (ts, w) in power if ts >= measured_from]
    if not rates:
        print("".join(lines[-30:]))
        print("no hashrate samples")
        return 1
    mean = statistics.mean(rates)
    sd = statistics.pstdev(rates)
    w = statistics.mean(watts) if watts else 0
    print("%-40s %8.2f MH/s  sd %5.2f  n %3d  %6.1f W  %6.1f kH/W" %
          (" ".join(args.extra) or "(defaults)", mean, sd, len(rates), w, mean * 1e3 / w if w else 0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
