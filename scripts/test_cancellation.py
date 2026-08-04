#!/usr/bin/env python3
import socket
import struct
import time
import os

HOST = '127.0.0.1'
PORT = 8080
MAGIC = 0x4D454449
VERSION = 1
CMD_STREAM_FILE = 0x0010

# NOTE: Replace this with an actual valid File ID printed by your server during boot
VALID_FILE_ID = 5096559374773741189

def violent_disconnect():
    print("Initiating connection...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, PORT))
    
    # 40-byte header
    header = struct.pack(
        "<IHHIIQQQ24s", MAGIC, VERSION, CMD_STREAM_FILE, 0, 0, 
        VALID_FILE_ID, 0, 1024*1024*100, b'\x00' * 24 # Request 100MB
    )
    
    s.sendall(header)
    
    print("Reading first chunk to engage the io_uring pipeline...")
    data = s.recv(65536)
    
    print("Violently severing process at OS level...")
    # os._exit immediately kills the python process, bypassing clean garbage collection
    # and sending a brutal TCP RST to the server.
    os._exit(0)

if __name__ == "__main__":
    violent_disconnect()
