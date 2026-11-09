#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Leid 2026.10

End-to-end smoke test for the Transport facade (TransportDemo).

Starts TransportDemo in tcp_server mode on a free loopback port, feeds it one
valid A5 5A frame and verifies that

  * the facade reports the listening socket       ([lib][INFO][transport] ... listening on)
  * the link is announced                         ("[LinkUp] <ip:port>")
  * the frame is reassembled and delivered        ("[Frame] tag=... len=4 ... 01 02 03 04")
  * the echo is handed to the OS                  ("[Sent] tag=... len=9")

Usage:
    e2e_transport_test.py <path-to-TransportDemo> [port]
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

START_TIMEOUT = 20.0
FRAME_TIMEOUT = 10.0


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
        self.sink.put(None)


def wait_for(sink, needle, timeout):
    deadline = time.time() + timeout
    while True:
        left = deadline - time.time()
        if left <= 0:
            return None
        try:
            line = sink.get(timeout=left)
        except queue.Empty:
            return None
        if line is None:
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
        print("usage: e2e_transport_test.py <path-to-TransportDemo> [port]")
        return 2

    binary = argv[1]
    port = int(argv[2]) if len(argv) > 2 else free_port()

    print("[e2e] starting %s tcp_server on port %d" % (binary, port))
    proc = subprocess.Popen([binary, "tcp_server", str(port)],
                            stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT,
                            stdin=subprocess.PIPE)
    sink = queue.Queue()
    Reader(proc.stdout, sink).start()

    ok = False
    try:
        if wait_for(sink, "listening on", START_TIMEOUT) is None:
            print("[e2e] FAIL: transport never reported 'listening on'")
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
            frame_line = wait_for(sink, "[Frame]", FRAME_TIMEOUT)
            sent_line = wait_for(sink, "[Sent]", FRAME_TIMEOUT)

            # The echoed frame must come back on the wire too.
            sock.settimeout(FRAME_TIMEOUT)
            echoed = b""
            try:
                while len(echoed) < len(FRAME):
                    chunk = sock.recv(4096)
                    if not chunk:
                        break
                    echoed += chunk
            except socket.timeout:
                pass

        ok = (frame_line is not None
              and "len=4" in frame_line
              and "01 02 03 04" in frame_line
              and sent_line is not None
              and echoed == FRAME)
        if ok:
            print("[e2e] PASS: frame delivered to the callback and echoed back")
        else:
            print("[e2e] FAIL: frame=%r sent=%r echo=%r" % (frame_line, sent_line, echoed))
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except Exception:      # pragma: no cover - best effort
            proc.kill()

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
