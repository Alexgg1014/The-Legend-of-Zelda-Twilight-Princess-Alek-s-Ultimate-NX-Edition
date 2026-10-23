#!/usr/bin/env python3
"""DM3: a small RFC 6455 server for the host test of the Switch WebSocket transport (test-dm3.sh).

Standard library only. Paths:
  /echo   echoes every text/binary message; on "frag" answers "frag-ment" in two fragments with a ping
          between them; on "close-me" starts the close handshake itself (code 4001, reason "bye").
  /deny   answers 403 instead of upgrading.
  /badkey upgrades with a wrong Sec-WebSocket-Accept.

usage: dm3_ws_server.py PORT   (prints "ready" when listening on 127.0.0.1:PORT)
"""
import base64
import hashlib
import socket
import struct
import sys
import threading

GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def recv_exact(conn, n):
    data = b""
    while len(data) < n:
        chunk = conn.recv(n - len(data))
        if not chunk:
            raise ConnectionError("closed")
        data += chunk
    return data


def read_frame(conn):
    b0, b1 = recv_exact(conn, 2)
    fin, opcode, masked, length = b0 >> 7, b0 & 15, b1 >> 7, b1 & 127
    if length == 126:
        length = struct.unpack(">H", recv_exact(conn, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", recv_exact(conn, 8))[0]
    if not masked:
        raise ValueError("client frame not masked")
    mask = recv_exact(conn, 4)
    payload = bytearray(recv_exact(conn, length))
    for i in range(length):
        payload[i] ^= mask[i & 3]
    return fin, opcode, bytes(payload)


def frame(opcode, payload, fin=True):
    head = bytes([(0x80 if fin else 0) | opcode])
    n = len(payload)
    if n < 126:
        head += bytes([n])
    elif n <= 0xFFFF:
        head += bytes([126]) + struct.pack(">H", n)
    else:
        head += bytes([127]) + struct.pack(">Q", n)
    return head + payload


def handle(conn):
    try:
        request = b""
        while b"\r\n\r\n" not in request:
            chunk = conn.recv(4096)
            if not chunk:
                return
            request += chunk
        head, _, rest = request.partition(b"\r\n\r\n")
        lines = head.decode("latin-1").split("\r\n")
        path = lines[0].split(" ")[1]
        headers = {k.strip().lower(): v.strip() for k, v in (l.split(":", 1) for l in lines[1:] if ":" in l)}
        if path == "/deny":
            conn.sendall(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            return
        key = headers.get("sec-websocket-key", "").encode()
        accept = base64.b64encode(hashlib.sha1(key + GUID).digest())
        if path == "/badkey":
            accept = base64.b64encode(b"x" * 20)
        proto = headers.get("sec-websocket-protocol", "")
        extra = f"Sec-WebSocket-Protocol: {proto.split(',')[0].strip()}\r\n" if proto else ""
        conn.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     b"Sec-WebSocket-Accept: " + accept + b"\r\n" + extra.encode() + b"\r\n")
        assembled, kind = b"", None
        while True:
            fin, opcode, payload = read_frame(conn)
            if opcode == 8:
                conn.sendall(frame(8, payload[:2]))
                return
            if opcode == 9:
                conn.sendall(frame(10, payload))
                continue
            if opcode == 10:
                continue
            if opcode in (1, 2):
                assembled, kind = payload, opcode
            else:
                assembled += payload
            if not fin:
                continue
            if assembled == b"frag":
                conn.sendall(frame(1, b"frag-", fin=False) + frame(9, b"p") + frame(0, b"ment"))
            elif assembled == b"close-me":
                conn.sendall(frame(8, struct.pack(">H", 4001) + b"bye"))
            else:
                conn.sendall(frame(kind, assembled))
    except (ConnectionError, ValueError, OSError):
        pass
    finally:
        conn.close()


def main():
    port = int(sys.argv[1])
    server = socket.socket()
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", port))
    server.listen(16)
    print("ready", flush=True)
    while True:
        conn, _ = server.accept()
        threading.Thread(target=handle, args=(conn,), daemon=True).start()


if __name__ == "__main__":
    main()
