"""Minimal EthereumStratum/1.0.0 (NiceHash flavour) pool mock for exercising hashwarp.

Sends mining.set_difficulty (in 2^32 hash units, as pools do) and a new
mining.notify job every --job-seconds. Accepts every mining.submit and logs it,
so callers can check the share rate matches the difficulty and that nonces
carry the extranonce prefix.
"""
import argparse
import hashlib
import json
import os
import socketserver
import threading
import time

EPOCH_LENGTH = 720
MAGIC = b"PARALLAX\x01"


def seedhash(block):
    s = b"\x00" * 32
    for _ in range(block // EPOCH_LENGTH):
        s = hashlib.sha3_256(MAGIC + s).digest()
    return s


def log(msg):
    print("[mock %s] %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def make_handler(args):
    seed = seedhash(args.block).hex()

    class Handler(socketserver.StreamRequestHandler):
        def send(self, obj):
            self.wfile.write((json.dumps(obj) + "\n").encode())
            self.wfile.flush()

        def notify(self, job):
            self.send({"id": None, "method": "mining.notify",
                       "params": ["%x" % job, seed, os.urandom(32).hex(), True]})

        def jobs(self):
            job = 1
            while not self.stop.wait(args.job_seconds):
                job += 1
                try:
                    self.notify(job)
                except OSError:
                    return

        def handle(self):
            self.stop = threading.Event()
            submits = 0
            for line in self.rfile:
                req = json.loads(line)
                method, rid = req.get("method"), req.get("id")
                if method == "mining.subscribe":
                    self.send({"id": rid, "error": None, "result": [
                        ["mining.notify", "ae6812eb4cd7735a302a8a9dd95cf71f", "EthereumStratum/1.0.0"],
                        args.extranonce]})
                elif method == "mining.extranonce.subscribe":
                    if args.rplant:
                        # rplant answers with the id of mining.authorize and "error": false
                        self.send({"id": 3, "result": True, "error": False})
                    else:
                        self.send({"id": rid, "error": None, "result": True})
                elif method == "mining.authorize":
                    self.send({"id": rid, "error": None, "result": True})
                    self.send({"id": None, "method": "mining.set_difficulty",
                               "params": [args.difficulty]})
                    self.notify(1)
                    threading.Thread(target=self.jobs, daemon=True).start()
                elif method == "mining.submit":
                    submits += 1
                    log("submit #%d %s" % (submits, json.dumps(req["params"])))
                    self.send({"id": rid, "error": None, "result": True})
                elif method == "mining.hello":
                    self.send({"id": rid, "error": {"code": -1, "message": "unsupported"},
                               "result": None})
            self.stop.set()

    return Handler


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=13333)
    ap.add_argument("--block", type=int, default=60268)
    ap.add_argument("--difficulty", type=float, default=0.05, help="in 2^32 hash units")
    ap.add_argument("--extranonce", default="0807")
    ap.add_argument("--job-seconds", type=float, default=20)
    ap.add_argument("--rplant", action="store_true", help="reply like pool.rplant.xyz does")
    args = ap.parse_args()
    log("EthereumStratum/1.0.0 on %d, difficulty %g, extranonce %s" %
        (args.port, args.difficulty, args.extranonce))
    socketserver.ThreadingTCPServer.allow_reuse_address = True
    socketserver.ThreadingTCPServer(("127.0.0.1", args.port), make_handler(args)).serve_forever()


if __name__ == "__main__":
    main()
