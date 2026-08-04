#!/usr/bin/env python3
import socket
import struct
import time
from concurrent.futures import ThreadPoolExecutor

HOST = '127.0.0.1'
PORT = 8080
MAGIC = 0x4D454449
VERSION = 1
CMD_QUERY_METADATA = 0x0020

def fire_metadata_query(client_id):
    # Payload: Type 0x0001, Length 6, Value "Batman"
    tlv_payload = struct.pack("<HH6s", 0x0001, 6, b"Batman")
    
    header = struct.pack(
        "<IHHIIQQQ24s", MAGIC, VERSION, CMD_QUERY_METADATA, len(tlv_payload), 0, 
        0, 0, 0, b'\x00' * 24
    )
    
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.connect((HOST, PORT))
            s.sendall(header + tlv_payload)
            response = s.recv(1024)
            
            # Decode the JSON response from the SQLite Engine
            decoded_response = response.decode('utf-8').strip()
            
            if "FOUND" in decoded_response:
                return f"[Query {client_id}] SUCCESS: {decoded_response}"
            elif "NOT_FOUND" in decoded_response:
                return f"[Query {client_id}] SUCCESS (No match): {decoded_response}"
            else:
                return f"[Query {client_id}] FAILED (Unknown response): {decoded_response}"
                
    except Exception as e:
        return f"[Query {client_id}] ERROR: {str(e)}"

if __name__ == "__main__":
    print("=== INITIATING METADATA CONTENTION STORM (100 Threads) ===")
    start = time.time()
    
    with ThreadPoolExecutor(max_workers=100) as executor:
        futures = [executor.submit(fire_metadata_query, i) for i in range(100)]
        for f in futures:
            # We print the result so you can physically see the JSON from the Worker
            print(f.result()) 
            
    print(f"\nExecuted 100 concurrent SQLite queries in {time.time() - start:.3f} seconds.")
