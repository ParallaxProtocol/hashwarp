"""Minimal Parallax getwork node mock for exercising hashwarp.

eth_getWork returns [header, seed, target, block]; the header changes every
--job-seconds (0 = once after --first-job-seconds, then never). eth_submitWork
and eth_submitHashrate are accepted and counted.
"""
import argparse
import hashlib
import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

EPOCH_LENGTH = 720
MAGIC = b"PARALLAX\x01"


def seedhash(block):
    s = b"\x00" * 32
    for _ in range(block // EPOCH_LENGTH):
        s = hashlib.sha3_256(MAGIC + s).digest()
    return s


class State:
    def __init__(self, args):
        self.args = args
        self.block = args.block
        self.seed = "0x" + seedhash(self.block).hex()
        diff_hashes = int(args.difficulty * 2**32)
        self.target = "0x%064x" % (((1 << 256) - 1) // diff_hashes)
        self.header = "0x" + os.urandom(32).hex()
        self.start = time.time()
        self.switched = False
        self.submits = 0
        self.lock = threading.Lock()

    def work(self):
        with self.lock:
            elapsed = time.time() - self.start
            if self.args.job_seconds:
                idx = int(elapsed // self.args.job_seconds)
                if idx != getattr(self, "_idx", 0):
                    self._idx = idx
                    self.header = "0x" + os.urandom(32).hex()
                    log("new job %s" % self.header[:12])
            elif not self.switched and elapsed >= self.args.first_job_seconds:
                self.switched = True
                self.header = "0x" + os.urandom(32).hex()
                log("new job %s" % self.header[:12])
            return [self.header, self.seed, self.target, hex(self.block)]


def log(msg):
    print("[mock %s] %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def make_handler(state):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_POST(self):
            body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
            req = json.loads(body)
            method = req.get("method")
            if method == "eth_getWork":
                result = state.work()
            elif method == "eth_submitWork":
                with state.lock:
                    state.submits += 1
                    n = state.submits
                log("submitWork #%d nonce=%s header=%s" % (n, req["params"][0], req["params"][1][:12]))
                result = True
            elif method == "eth_submitHashrate":
                result = True
            else:
                result = None
            out = json.dumps({"id": req.get("id"), "jsonrpc": "2.0", "result": result}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(out)))
            self.end_headers()
            self.wfile.write(out)

    return Handler


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=18545)
    ap.add_argument("--block", type=int, default=60268)
    ap.add_argument("--difficulty", type=float, default=1.0, help="in 2^32 hash units")
    ap.add_argument("--job-seconds", type=float, default=0)
    ap.add_argument("--first-job-seconds", type=float, default=10)
    args = ap.parse_args()
    state = State(args)
    log("block %d epoch %d seed %s" % (args.block, args.block // EPOCH_LENGTH, state.seed[:12]))
    ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(state)).serve_forever()


if __name__ == "__main__":
    sys.exit(main())
