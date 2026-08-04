#!/usr/bin/env python3
import socket
import time

HOST = '127.0.0.1'
PORT = 8080

print("Initiating File Descriptor Exhaustion...")
sockets = []

try:
    for i in range(2000): # We attempt to open 2,000 concurrent sockets
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((HOST, PORT))
        sockets.append(s)
        if i % 100 == 0:
            print(f"Holding {i} sockets open...")
except Exception as e:
    print(f"OS denied further connections: {e}")

print("Holding the barrage for 10 seconds to observe server behavior...")
time.sleep(10)
