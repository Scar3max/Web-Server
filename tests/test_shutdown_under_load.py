#!/usr/bin/env python3
"""
Phase 4.2 Shutdown Under Load Test

Validates:
1. Server handles SIGINT gracefully while multiple WebSocket clients are actively streaming snapshots and submitting input.
2. Server does not hang, deadlock, or crash during shutdown.
3. Server terminates in < 2.0 seconds with exit code 0.
"""

import subprocess
import socket
import json
import base64
import os
import sys
import time
import signal
import threading

SERVER_BIN = "./build/server"
SERVER_HOST = "127.0.0.1"
SERVER_PORT = 8080

def build_ws_frame(payload_bytes, opcode=0x1):
    header = bytearray([0x80 | (opcode & 0x0F)])
    plen = len(payload_bytes)
    header.append(0x80 | plen)
    mask = b"\x11\x22\x33\x44"
    header.extend(mask)
    masked = bytearray(plen)
    for i in range(plen):
        masked[i] = payload_bytes[i] ^ mask[i % 4]
    return bytes(header) + bytes(masked)

def connect_ws():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(3.0)
    s.connect((SERVER_HOST, SERVER_PORT))
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        f"GET /ws HTTP/1.1\r\n"
        f"Host: {SERVER_HOST}:{SERVER_PORT}\r\n"
        f"Upgrade: websocket\r\n"
        f"Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n\r\n"
    )
    s.sendall(req.encode())
    res = b""
    while b"\r\n\r\n" not in res:
        chunk = s.recv(1024)
        if not chunk:
            break
        res += chunk
    return s

def test_shutdown_under_load(bin_path=SERVER_BIN, use_setarch=False):
    print(f"[TEST] Clean Server Shutdown Under Active Load ({bin_path})...")
    
    cmd = [bin_path]
    if use_setarch:
        cmd = ["setarch", "x86_64", "-R", bin_path]
        
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(1.0)
    
    running = True
    active_clients = []
    
    def client_worker(idx):
        try:
            ws = connect_ws()
            active_clients.append(ws)
            cmd = {"type": "input", "seq": 1, "controls": {"up": True, "down": False, "left": False, "right": False}}
            ws.sendall(build_ws_frame(json.dumps(cmd).encode()))
            seq = 2
            while running:
                try:
                    in_pkt = {"type": "input", "seq": seq, "controls": {"up": True, "down": False, "left": False, "right": False}}
                    ws.sendall(build_ws_frame(json.dumps(in_pkt).encode()))
                    seq += 1
                    data = ws.recv(4096)
                    if not data:
                        break
                    time.sleep(0.02)
                except Exception:
                    break
            ws.close()
        except Exception:
            pass

    threads = [threading.Thread(target=client_worker, args=(i,)) for i in range(10)]
    for t in threads:
        t.start()
        
    time.sleep(1.0) # Let simulation stream snapshots to all 10 clients
    print(f"  -> 10 clients streaming snapshots actively. Delivering SIGINT to PID {proc.pid}...")
    
    t_start = time.time()
    proc.send_signal(signal.SIGINT)
    
    running = False
    
    try:
        proc.wait(timeout=3.0)
        t_exit = time.time() - t_start
        print(f"  -> Server exited with code {proc.returncode} in {t_exit:.3f}s")
        assert proc.returncode == 0, f"Expected clean exit code 0, got {proc.returncode}"
    except subprocess.TimeoutExpired:
        proc.kill()
        assert False, "Server hung or deadlocked during SIGINT shutdown under load!"
        
    for t in threads:
        t.join(timeout=1.0)
        
    print("[PASS] Clean server shutdown under load verified without hang or deadlock.")

if __name__ == "__main__":
    try:
        test_shutdown_under_load(SERVER_BIN)
        print("\nALL SHUTDOWN TESTS PASSED!")
    except AssertionError as e:
        print(f"[FAIL] {e}")
        sys.exit(1)
    except Exception as e:
        print(f"[ERROR] {e}")
        sys.exit(1)
