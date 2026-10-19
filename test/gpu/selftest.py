"""GPU self-test: mine against a mock node or pool and check the results.

Each solution a GPU finds is re-hashed on the CPU by hashwarp before it is
submitted; a mismatch is reported as "gave incorrect result". This script runs
the miner for a while at an easy difficulty and fails if any result is
incorrect or too few solutions were accepted.

With --protocol stratum it mines on an EthereumStratum/1.0.0 mock pool and also
checks the share rate matches the pool difficulty (2^32 hashes per difficulty
unit) and that nonces are submitted without the pool's extranonce.

  python3 test/gpu/selftest.py build/hashwarp/hashwarp cuda
  python3 test/gpu/selftest.py build/hashwarp/hashwarp opencl --protocol stratum --seconds 90
"""
import argparse
import json
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ANSI = re.compile(r"\x1b\[[0-9;]*m")
DIFFICULTY = 0.05  # in 2^32 hash units: a few solutions per second on a modern GPU
EXTRANONCE = "0807"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("miner", help="path to the hashwarp binary")
    ap.add_argument("backend", choices=["cuda", "opencl"])
    ap.add_argument("--protocol", choices=["getwork", "stratum"], default="getwork")
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--min-solutions", type=int, default=5)
    ap.add_argument("--block", type=int, default=60268)
    ap.add_argument("--port", type=int, default=18545)
    args = ap.parse_args()

    mocklog = tempfile.TemporaryFile(mode="w+")
    common = ["--port", str(args.port), "--block", str(args.block),
              "--difficulty", str(DIFFICULTY), "--job-seconds", "20"]
    if args.protocol == "getwork":
        mock_cmd = [os.path.join(HERE, "mock_getwork.py")] + common
        pool = "http://127.0.0.1:%d" % args.port
    else:
        mock_cmd = [os.path.join(HERE, "mock_stratum.py"), "--extranonce", EXTRANONCE] + common
        pool = "stratum2+tcp://0xabc.selftest@127.0.0.1:%d" % args.port
    mock = subprocess.Popen([sys.executable, "-I"] + mock_cmd, stdout=mocklog,
        stderr=subprocess.STDOUT)
    # Wait until the mock listens (it fails if the port is still in use)
    for _ in range(50):
        if mock.poll() is not None:
            mocklog.seek(0)
            print(mocklog.read())
            print("FAIL: the mock %s didn't start (port %d in use?)" % (args.protocol, args.port))
            return 1
        try:
            socket.create_connection(("127.0.0.1", args.port), 0.2).close()
            break
        except OSError:
            time.sleep(0.1)
    backend = {"cuda": "-U", "opencl": "-G"}[args.backend]
    miner = subprocess.Popen([args.miner, backend, "-P", pool],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        time.sleep(args.seconds)
    finally:
        miner.send_signal(signal.SIGINT)
        out, _ = miner.communicate(timeout=30)
        mock.terminate()
        mock.wait()
    mocklog.seek(0)
    mockout = mocklog.read()

    out = ANSI.sub("", out)
    accepted = len(re.findall(r"accepted(?: \(stale\))? gpu\d+|BLOCK FOUND by gpu\d+", out))
    rejected = len(re.findall(r"rejected gpu\d+", out))
    incorrect = len(re.findall(r"Wrong result", out))
    epoch = re.search(r"Epoch (\d+) ", out)

    print("epoch %s, accepted %d, rejected %d, incorrect %d" %
          (epoch.group(1) if epoch else "?", accepted, rejected, incorrect))
    ok = incorrect == 0 and rejected == 0 and accepted >= args.min_solutions
    if epoch and int(epoch.group(1)) != args.block // 720:
        print("wrong epoch, expected %d" % (args.block // 720))
        ok = False

    if args.protocol == "stratum":
        rates = [float(r) for r in re.findall(r"([\d.]+) MH/s (?:·|\|) 60s", out)]
        rates = [r for r in rates if r > 0]
        if rates:
            mined_seconds = args.seconds - 10  # roughly: startup and DAG generation
            expected = sum(rates) / len(rates) * 1e6 * mined_seconds / (DIFFICULTY * 2**32)
            print("shares %d, expected about %.0f at %.2f MH/s" %
                  (accepted, expected, sum(rates) / len(rates)))
            if not expected / 3 <= accepted <= expected * 3:
                print("share rate does not match the pool difficulty")
                ok = False
        nonces = [json.loads(m)[2] for m in re.findall(r"submit #\d+ (\[.*\])", mockout)]
        bad = [n for n in nonces if len(n) != 16 - len(EXTRANONCE)]
        if bad:
            print("nonces submitted with the wrong length: %s" % bad[:3])
            ok = False

    if not ok:
        print(out[-4000:])
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
