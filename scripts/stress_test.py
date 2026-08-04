#!/usr/bin/env python3
import socket
import struct
import time
import threading
from concurrent.futures import ThreadPoolExecutor

# --- Configuration ---
HOST = '127.0.0.1'
PORT = 8080
MAGIC = 0x4D454449
VERSION = 1
CMD_STREAM_FILE = 0x0010
CMD_TEARDOWN = 0x0040

# The matrix now includes the dummy pollution file
TEST_FILES = [
    "The_Dark_Knight_4k.mkv",
    "Inception.mkv",
    "subtitles.srt" 
]

def djb2_hash(filename):
    h = 5381
    for char in filename:
        h = ((h << 5) + h) + ord(char)
        h &= 0xFFFFFFFFFFFFFFFF 
    return h

def build_header(command, file_id, start_byte, end_byte):
    header_format = "<IHHIIQQQ24s"
    return struct.pack(
        header_format, MAGIC, VERSION, command, 0, 0, 
        file_id, start_byte, end_byte, b'\x00' * 24
    )

def simulate_client_stream(client_id, filename, file_id, request_size):
    header = build_header(CMD_STREAM_FILE, file_id, 0, request_size)
    
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.connect((HOST, PORT))
            s.sendall(header)
            
            bytes_received = 0
            while bytes_received < request_size:
                data = s.recv(1024 * 1024) 
                if not data:
                    break
                bytes_received += len(data)
                
            teardown_header = build_header(CMD_TEARDOWN, 0, 0, 0)
            s.sendall(teardown_header)
            
            # Subtitle Validation Matrix
            if filename.endswith(".srt"):
                if bytes_received == 0:
                    return f"[Client {client_id}] SUCCESS (Filter blocked {filename})"
                else:
                    return f"[Client {client_id}] FAILED (Filter leaked {bytes_received} bytes of {filename})"
            
            # Media Validation Matrix
            if bytes_received == request_size:
                return f"[Client {client_id}] SUCCESS: Streamed {bytes_received / (1024*1024):.1f} MB of {filename}"
            else:
                return f"[Client {client_id}] FAILED: Expected {request_size}, got {bytes_received}"
    except Exception as e:
        return f"[Client {client_id}] ERROR: {str(e)}"

def run_concurrency_storm(num_clients):
    print(f"\n=== INITIATING CONCURRENT PACING STORM ({num_clients} Threads) ===")
    
    hashed_files = {name: djb2_hash(name) for name in TEST_FILES}
    for name, fid in hashed_files.items():
        print(f"Targeting: {name} -> Hash ID: {fid}")

    # Request exactly 5MB. Pacing should force this to take ~5.1 seconds.
    request_size = 5 * 1024 * 1024 
    
    start_time = time.time()
    
    with ThreadPoolExecutor(max_workers=num_clients) as executor:
        futures = []
        for i in range(num_clients):
            target_file = TEST_FILES[i % len(TEST_FILES)]
            target_id = hashed_files[target_file]
            futures.append(executor.submit(simulate_client_stream, i, target_file, target_id, request_size))
            
        for future in futures:
            print(future.result())
            
    total_time = time.time() - start_time
    
    # We only count bandwidth for the valid media files (2/3 of the threads)
    valid_clients = (num_clients // 3) * 2
    total_mb = (valid_clients * request_size) / (1024 * 1024)
    
    print(f"=== STORM COMPLETE ===")
    print(f"Total Time: {total_time:.2f} seconds (Expected ~5-6 seconds due to async pacing).")
    print(f"Aggregate Paced Throughput: {total_mb / total_time:.2f} MB/s.")

if __name__ == "__main__":
    run_concurrency_storm(50)
