"""Throws malformed RFB sessions at a running server and checks that it still serves a normal client afterwards.

usage: python scripts/fuzz.py <port> [sessions] [host]
Start the server with --view-only first: random bytes could otherwise be read as key or mouse events.
Exit code 0 = the server survived and stayed healthy.
"""
import random
import socket
import struct
import sys
import time

PORT = int(sys.argv[1])
N = int(sys.argv[2]) if len(sys.argv) > 2 else 100
HOST = sys.argv[3] if len(sys.argv) > 3 else "127.0.0.1"
random.seed(1234)


def rd(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise EOFError
        b += c
    return b


def handshake(s):
    rd(s, 12)
    s.sendall(b"RFB 003.008\n")
    rd(s, 2)
    s.sendall(b"\x01")
    rd(s, 4)
    s.sendall(b"\x01")
    h = rd(s, 24)
    rd(s, struct.unpack(">I", h[20:24])[0])


def connect():
    return socket.create_connection((HOST, PORT), timeout=5)


def healthy():
    try:
        s = connect()
        handshake(s)
        s.sendall(bytes([3, 0]) + struct.pack(">HHHH", 0, 0, 100, 100))
        ok = rd(s, 1) == b"\x00"
        s.close()
        return ok
    except Exception:
        return False


for i in range(N):
    mode = random.choice(["garbage-before", "garbage-after", "truncated", "bigcounts", "badpf"])
    try:
        s = connect()
        if mode == "garbage-before":
            s.sendall(bytes(random.randrange(256) for _ in range(random.randrange(1, 200))))
        else:
            handshake(s)
            if mode == "garbage-after":
                body = bytearray()
                for _ in range(random.randrange(1, 30)):
                    body.append(random.choice([0, 2, 3, 150, 248, 251, 255, 99]))
                    body += bytes(random.randrange(256) for _ in range(random.randrange(0, 40)))
                s.sendall(bytes(body))
            elif mode == "truncated":
                s.sendall(bytes([2, 0]) + struct.pack(">H", 5000) + b"\x00\x00\x00\x07")
            elif mode == "bigcounts":
                s.sendall(bytes([2, 0, 0xFF, 0xFF]) + bytes(random.randrange(256) for _ in range(100)))
                s.sendall(bytes([251, 0, 0xFF, 0xFF, 0xFF, 0xFF, 255, 0]))
                s.sendall(bytes([248, 0, 0, 0, 0x80, 0, 0, 1, 255]) + b"x" * 10)
            elif mode == "badpf":
                bad = bytes([0, 0, 0, 0, random.choice([8, 16, 32, 24, 64]), 24, random.randrange(2), 1]) + bytes(random.randrange(256) for _ in range(12))
                s.sendall(bad)
                s.sendall(bytes([3, 0]) + struct.pack(">HHHH", 0, 0, 50, 50))
        time.sleep(0.05)
        s.close()
    except Exception:
        pass  # the server is allowed to hang up on us
    if i % 25 == 24 and not healthy():
        print("server unhealthy after %d malformed sessions" % (i + 1))
        sys.exit(1)

time.sleep(0.5)
if healthy():
    print("server healthy after %d malformed sessions" % N)
    sys.exit(0)
print("FAILED: server does not answer a normal client after %d malformed sessions" % N)
sys.exit(1)
