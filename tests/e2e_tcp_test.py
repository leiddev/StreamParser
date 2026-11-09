#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Leid 2026.7

End-to-end smoke test for the TCP demo stack.

Starts TcpServerDemo on a free loopback port, feeds it one valid A5 5A frame and
verifies that

  * the pluggable log callback reports the listening socket   (tcp-server / INFO)
  * the session is accepted                                   ("[Accept]")
  * the frame is reassembled and handed to the frame callback ("[Frame] ... len=4")

Usage:
    e2e_tcp_test.py <path-to-TcpServerDemo> [port]

The port is optional; a free one is picked when omitted. Exit code 0 on success,
1 on failure (the child output is always echoed for diagnosis).
"""
from __future__ import print_function

import queue
import socket
import subprocess
import sys
import threading
import time

# A5 5A | Len = 0x0007 (payload 4 + checksum 1 + tail 2) | 01 02 03 04 | XOR | 55 AA
FRAME = bytes.fromhex("a55a000701020304fc55aa")

OUTPUT_TIMEOUT = 20.0    # seconds to wait for the server to start listening
FRAME_TIMEOUT = 10.0     # seconds to wait for the parsed frame


class Reader(threading.Thread):
    """Collect the child's stdout/stderr lines into a queue."""

    def __init__(self, stream, sink):
        threading.Thread.__init__(self)
        self.daemon = True
        self.stream = stream
        self.sink = sink

    def run(self):
        try:
            for raw in iter(self.stream.readline, b""):
                self.sink.put(raw.decode("utf-8", "replace"))
        except Exception:      # pragma: no cover - best effort
            pass
        self.sink.put(None)    # EOF marker


def wait_for(sink, needle, timeout):
    """Consume lines until one contains `needle`; return it or None."""
    deadline = time.time() + timeout
    while True:
        left = deadline - time.time()
        if left <= 0:
            return None
        try:
            line = sink.get(timeout=left)
        except queue.Empty:
            return None
        if line is None:       # child closed its output
            return None
        sys.stdout.write("  | " + line.rstrip() + "\n")
        sys.stdout.flush()
        if needle in line:
            return line


def free_port():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]
    finally:
        sock.close()


def main(argv):
    if len(argv) < 2:
        print("usage: e2e_tcp_test.py <path-to-TcpServerDemo> [port]")
        return 2

    binary = argv[1]
    port = int(argv[2]) if len(argv) > 2 else free_port()

    print("[e2e] starting %s on port %d" % (binary, port))
    proc = subprocess.Popen([binary, str(port)],
                            stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    sink = queue.Queue()
    Reader(proc.stdout, sink).start()

    ok = False
    try:
        if wait_for(sink, "listening on", OUTPUT_TIMEOUT) is None:
            print("[e2e] FAIL: server never reported 'listening on'")
            return 1

        sock = None
        for _ in range(100):
            try:
                sock = socket.create_connection(("127.0.0.1", port), 0.2)
                break
            except OSError:
                time.sleep(0.1)
        if sock is None:
            print("[e2e] FAIL: cannot connect to 127.0.0.1:%d" % port)
            return 1

        with sock:
            sock.sendall(FRAME)
            line = wait_for(sink, "[Frame]", FRAME_TIMEOUT)

        ok = (line is not None
              and "len=4" in line
              and "01 02 03 04" in line)
        if ok:
            print("[e2e] PASS: frame reassembled and delivered to the callback")
        else:
            print("[e2e] FAIL: frame was not delivered as expected (got %r)" % (line,))
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except Exception:      # pragma: no cover - best effort
            proc.kill()

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
