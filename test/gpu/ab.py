"""A/B benchmark of hashwarp builds or option sets, running them in rotation so
background load on the GPU affects all alike. The first variant is the reference.

  python3 test/gpu/ab.py A=build-a/hashwarp/hashwarp B=build-b/hashwarp/hashwarp --rounds 4
  python3 test/gpu/ab.py "A=hw" "B=hw --cuda-grid-size 16384" --args=--noeval
"""
import argparse
import os
import re
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RESULT = re.compile(r"([\d.]+) MH/s\s+sd\s+([\d.]+)\s+n\s+\d+\s+([\d.]+) W")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("variants", nargs="+", help="NAME=path/to/hashwarp [options]")
    ap.add_argument("--backend", default="cuda", choices=["cuda", "opencl"])
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--seconds", type=int, default=30)
    ap.add_argument("--warmup", type=int, default=15)
    ap.add_argument("--args", default="", help="options for every variant")
    args = ap.parse_args()

    variants = []
    extra = []
    for v in args.variants:
        name, rest = v.split("=", 1)
        parts = rest.split()
        variants.append((name, parts[0]))
        extra.append(parts[1:] + args.args.split())
    results = {name: [] for name, _ in variants}
    for r in range(args.rounds):
        for i, (name, path) in enumerate(variants):
            out = subprocess.run([sys.executable, "-I", os.path.join(HERE, "bench.py"), path,
                                  args.backend, "--seconds", str(args.seconds), "--warmup",
                                  str(args.warmup), "--"] + extra[i],
                                 capture_output=True, text=True).stdout
            m = RESULT.search(out)
            if not m:
                print(out)
                return 1
            results[name].append((float(m.group(1)), float(m.group(3))))
            print("round %d %-6s %8.2f MH/s %6.1f W" % (r + 1, name, results[name][-1][0],
                                                       results[name][-1][1]), flush=True)
    base = None
    for name, _ in variants:
        rates = [x[0] for x in results[name]]
        watts = [x[1] for x in results[name]]
        mean = statistics.mean(rates)
        line = "%-6s mean %8.2f MH/s (sd %.2f) %6.1f W %6.1f kH/W" % (
            name, mean, statistics.pstdev(rates), statistics.mean(watts),
            mean * 1e3 / statistics.mean(watts))
        if base is None:
            base = mean
        else:
            line += "  %+.2f%%" % ((mean / base - 1) * 100)
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
