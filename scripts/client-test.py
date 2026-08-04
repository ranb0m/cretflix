#!/usr/bin/env python3
import socket
import struct
import time
import sys

# --- Network & Architecture Targets ---
HOST = '127.0.0.1'
PORT = 8080

# --- Protocol Definitions ---
MAGIC = 0x4D454449
VERSION = 1
CMD_STREAM_FILE = 0x0010
CMD_QUERY_METADATA = 0x0020
CMD_TEARDOWN = 0x0040

def djb2_hash(filename):
    """Mathematically replicates the Worker's C-level uint64_t hash."""
    h = 5381
    for char in filename:
        h = ((h << 5) + h) + ord(char)
        h &= 0xFFFFFFFFFFFFFFFF 
    return h

# --- Test Data ---
VALID_FILE_ID = djb2_hash("The_Dark_Knight_4k.mkv")
SUBTITLE_FILE_ID = djb2_hash("subtitles.srt")
INVALID_FILE_ID = 99999999999999999

def build_header(command, file_id, start_byte, end_byte, payload_len=0):
    header_format = "<IHHIIQQQ24s"
    return struct.pack(
        header_format, MAGIC, VERSION, command, payload_len, 0, 
        file_id, start_byte, end_byte, b'\x00' * 24
    )

def test_token_bucket_pacing():
    print("\n[TEST 1] Token Bucket Pacing (Requesting 20MB)...")
    # 20MB request. Exceeds the 6.8MB burst capacity. Must trigger pacing loop.
    target_bytes = 20 * 1024 * 1024 
    header = build_header(CMD_STREAM_FILE, VALID_FILE_ID, 0, target_bytes)
    
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((HOST, PORT))
        s.sendall(header)
        
        start_time = time.time()
        bytes_received = 0
        while bytes_received < target_bytes:
            data = s.recv(1024 * 1024)
            if not data: break
            bytes_received += len(data)
            
        duration = time.time() - start_time
        throughput = (bytes_received / (1024 * 1024)) / duration if duration > 0 else 0
        
        # We now assert that the duration strictly exceeds 3 seconds.
        if bytes_received == target_bytes and duration > 3.0:
            print(f"  -> PASS: Received {bytes_received} bytes. Pacing active (Duration: {duration:.2f}s, Speed: {throughput:.2f} MB/s).")
        else:
            print(f"  -> FAIL: Transfer completed abnormally fast ({duration:.2f}s) or failed. Pacing is broken.")

def test_extension_filter():
    print("\n[TEST 2] File Extension Filter (Requesting subtitles.srt)...")
    header = build_header(CMD_STREAM_FILE, SUBTITLE_FILE_ID, 0, 1024)
    
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((HOST, PORT))
        s.sendall(header)
        data = s.recv(1024)
        if not data:
            print("  -> PASS: Subtitle file was correctly ignored by the ingestion engine.")
        else:
            print(f"  -> FAIL: Dictionary pollution detected! Server leaked {len(data)} bytes of a non-media file.")

def test_random_access_seek():
    print("\n[TEST 3] Random Access Seek (Offset 1GB, Read 1MB)...")
    offset = 1024 * 1024 * 1024
    read_size = 1 * 1024 * 1024
    header = build_header(CMD_STREAM_FILE, VALID_FILE_ID, offset, offset + read_size)
    
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((HOST, PORT))
        s.sendall(header)
        
        bytes_received = 0
        while bytes_received < read_size:
            data = s.recv(1024 * 1024)
            if not data: break
            bytes_received += len(data)
            
        if bytes_received == read_size:
            print(f"  -> PASS: Successfully seeked and read exactly {bytes_received} bytes.")
        else:
            print(f"  -> FAIL: Expected {read_size}, got {bytes_received}.")

def test_hostile_client_invalid_id():
    print("\n[TEST 4] Hostile Dictionary Lookup (Invalid ID)...")
    header = build_header(CMD_STREAM_FILE, INVALID_FILE_ID, 0, 1024)
    
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((HOST, PORT))
        s.sendall(header)
        data = s.recv(1024)
        if not data:
            print("  -> PASS: Server forcefully dropped the connection (0 bytes received).")
        else:
            print(f"  -> FAIL: Server leaked {len(data)} bytes of data for an invalid ID.")

def test_ipc_worker_handoff():
    print("\n[TEST 5] Slow Path Mitosis (IPC Worker Handoff)...")
    tlv_payload = struct.pack("<HH6s", 0x0001, 6, b"Batman")
    payload_len = len(tlv_payload)
    header = build_header(CMD_QUERY_METADATA, 0, 0, 0, payload_len)
    
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((HOST, PORT))
        s.sendall(header + tlv_payload)
        response = s.recv(1024)
        if b"Metadata processed" in response:
            print(f"  -> PASS: Worker responded: {response.decode().strip()}")
        else:
            print(f"  -> FAIL: Unexpected Worker response: {response}")

def test_graceful_teardown():
    print("\n[TEST 6] Graceful Application Teardown...")
    header = build_header(CMD_TEARDOWN, 0, 0, 0)
    
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((HOST, PORT))
        s.sendall(header)
        data = s.recv(1024)
        if not data:
            print("  -> PASS: Server executed O(1) TCP teardown.")
        else:
            print("  -> FAIL: Server did not disconnect immediately.")

if __name__ == "__main__":
    print("=== INITIATING HPC INTEGRATION SUITE ===")
    test_token_bucket_pacing()
    test_extension_filter()
    test_random_access_seek()
    test_hostile_client_invalid_id()
    test_ipc_worker_handoff()
    test_graceful_teardown()
    print("\n=== SUITE COMPLETE ===")

