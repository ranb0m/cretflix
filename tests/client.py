#!/usr/bin/env python3
"""
tests/client.py

Protocol-aware client for the media server. Used as a library by test
scripts (`from client import Client`) or as a CLI for ad-hoc poking.

Critical bug-fix vs. the old test scripts: the server hashes the FULL
path (`/mnt/media_test/movies/inception.mkv`), not the basename. Pass
the full path to `Client.hash_path()`.

The wire format matches protocol.h:
  MediaHeader is 64 bytes, packed little-endian as:
    <I H H I I Q Q Q 24s
     |  | | | | | | | |
     |  | | | | | | | hardware_pad[24]
     |  | | | | | | end_byte
     |  | | | | | start_byte
     |  | | | | file_id
     |  | | | flags
     |  | | payload_len
     |  | command
     |  version
     magic ('MEDI' = 0x4D454449)
"""

import json
import socket
import struct
import time
from typing import Optional, Tuple

MAGIC = 0x4D454449
VERSION = 1

CMD_STREAM_FILE = 0x0010
CMD_QUERY_METADATA = 0x0020
CMD_AUTHENTICATE = 0x0030
CMD_TEARDOWN = 0x0040

HEADER_FMT = "<IHHIIQQQ24s"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
assert HEADER_SIZE == 64, f"Header should be 64 bytes, got {HEADER_SIZE}"

# Compile-time MEDIA_ROOT in the server. If you change worker.c's MEDIA_ROOT
# you must change this too. (Yes, this should be runtime-configurable. Future.)
DEFAULT_MEDIA_ROOT = "/mnt/media_test"


class Client:
    """One Client per connection. Use as a context manager."""

    def __init__(self, host: str = "127.0.0.1", port: int = 8080,
                 timeout: float = 10.0, media_root: str = DEFAULT_MEDIA_ROOT):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.media_root = media_root.rstrip("/")
        self.sock: Optional[socket.socket] = None

    # ---- lifecycle -------------------------------------------------------

    def connect(self) -> "Client":
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect((self.host, self.port))
        return self

    def close(self) -> None:
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def __enter__(self):
        return self.connect()

    def __exit__(self, *_):
        self.close()

    # ---- hashing (matches worker.c hash_path() exactly) -----------------

    @staticmethod
    def djb2(s: str) -> int:
        h = 5381
        for c in s:
            h = ((h << 5) + h + ord(c)) & 0xFFFFFFFFFFFFFFFF
        return h

    def hash_path(self, full_path: str) -> int:
        """Hash a full filesystem path — the server's identity unit."""
        return self.djb2(full_path)

    def hash_relative(self, rel: str) -> int:
        """Hash a path relative to MEDIA_ROOT (joined with media_root)."""
        rel = rel.lstrip("/")
        return self.djb2(f"{self.media_root}/{rel}")

    # ---- protocol helpers ------------------------------------------------

    @staticmethod
    def build_header(command: int, file_id: int = 0,
                     start_byte: int = 0, end_byte: int = 0,
                     payload_len: int = 0, flags: int = 0) -> bytes:
        return struct.pack(
            HEADER_FMT,
            MAGIC, VERSION, command, payload_len, flags,
            file_id, start_byte, end_byte,
            b"\x00" * 24,
        )

    def send_header(self, *args, **kwargs) -> None:
        assert self.sock is not None
        self.sock.sendall(self.build_header(*args, **kwargs))

    def send_raw(self, data: bytes) -> None:
        assert self.sock is not None
        self.sock.sendall(data)

    # ---- high-level operations ------------------------------------------

    def stream(self, file_id: int, start_byte: int = 0, end_byte: int = 0,
               max_bytes: Optional[int] = None) -> Tuple[bytes, float]:
        """
        Issue a CMD_STREAM_FILE and read the response. Returns (bytes, duration).

        If `max_bytes` is set, stops reading after that many bytes (without
        closing the connection — caller can do that). Otherwise reads until
        the server closes.
        """
        assert self.sock is not None
        self.send_header(CMD_STREAM_FILE, file_id, start_byte, end_byte)

        buf = bytearray()
        start = time.monotonic()
        while True:
            chunk_size = 65536
            if max_bytes is not None:
                remaining = max_bytes - len(buf)
                if remaining <= 0:
                    break
                chunk_size = min(chunk_size, remaining)
            try:
                data = self.sock.recv(chunk_size)
            except socket.timeout:
                break
            if not data:
                break
            buf.extend(data)
        return bytes(buf), time.monotonic() - start

    def query_metadata(self, search_string: str) -> dict:
        """
        Issue a CMD_QUERY_METADATA with TLV payload type 0x0001 (search).
        Returns parsed JSON response. Raises if the response isn't JSON.
        """
        assert self.sock is not None
        payload = struct.pack(f"<HH{len(search_string)}s",
                              0x0001, len(search_string),
                              search_string.encode("utf-8"))
        self.send_header(CMD_QUERY_METADATA, payload_len=len(payload))
        self.sock.sendall(payload)

        response = b""
        while True:
            try:
                data = self.sock.recv(4096)
            except socket.timeout:
                break
            if not data:
                break
            response += data

        text = response.decode("utf-8", errors="replace").strip()
        # The server may close after sending; trailing whitespace is fine.
        return json.loads(text) if text else {}

    def teardown(self) -> None:
        """Send CMD_TEARDOWN. Server closes the connection."""
        self.send_header(CMD_TEARDOWN)


# ---- CLI for ad-hoc poking -----------------------------------------------

def _cli():
    import argparse
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)

    p_hash = sub.add_parser("hash", help="Print djb2 hash of a path")
    p_hash.add_argument("path")

    p_stream = sub.add_parser("stream")
    p_stream.add_argument("path", help="Path under MEDIA_ROOT")
    p_stream.add_argument("--start", type=int, default=0)
    p_stream.add_argument("--end", type=int, default=0)
    p_stream.add_argument("--port", type=int, default=8080)

    p_query = sub.add_parser("query")
    p_query.add_argument("term")
    p_query.add_argument("--port", type=int, default=8080)

    args = p.parse_args()

    if args.cmd == "hash":
        c = Client()
        full = args.path if args.path.startswith("/") \
            else f"{c.media_root}/{args.path}"
        print(f"{full} -> {c.djb2(full)}")
        return

    c = Client(port=args.port)
    with c:
        if args.cmd == "stream":
            fid = c.hash_relative(args.path) if not args.path.startswith("/") \
                else c.djb2(args.path)
            data, dur = c.stream(fid, args.start, args.end)
            print(f"Received {len(data)} bytes in {dur:.3f}s "
                  f"({len(data) / dur / 1024:.1f} KB/s)")
        elif args.cmd == "query":
            print(json.dumps(c.query_metadata(args.term), indent=2))


if __name__ == "__main__":
    _cli()
