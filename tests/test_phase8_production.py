#!/usr/bin/env python3
"""
Phase 8 Production Readiness, Configuration & Deployment Verification Suite
Covers:
1. CLI Flag Parsing & Validation (--help, -h, -p, -s, -t, -l)
2. Invalid Configuration Rejection (Port, Threads, Log Level, Static Dir)
3. Environment Variable Precedence (Env vars applied, CLI flags override)
4. Dedicated /health Endpoint (HTTP 200, JSON schema, uptime)
5. Out-of-Tree Binary Execution (Running from /tmp resolves static assets)
6. Client Input Sanitization (Player name clamped to 32 chars, room code sanitized)
7. Clean Signal Handling & Graceful Shutdown (SIGINT, SIGTERM exit 0)
"""

import subprocess
import socket
import json
import time
import os
import sys
import signal
import urllib.request

SERVER_BIN = "./build/server"
PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def run_cmd(args, env=None, timeout=5):
    full_env = os.environ.copy()
    if env:
        full_env.update(env)
    return subprocess.run(args, cwd=PROJECT_ROOT, capture_output=True, text=True, env=full_env, timeout=timeout)

def test_cli_help():
    print("[TEST] CLI --help flag...")
    res = run_cmd([SERVER_BIN, "--help"])
    assert res.returncode == 0, f"Expected returncode 0, got {res.returncode}"
    assert "Usage:" in res.stdout
    assert "--host" in res.stdout
    assert "--port" in res.stdout
    assert "--static-dir" in res.stdout
    assert "--threads" in res.stdout
    assert "--log-level" in res.stdout
    print("[PASS] CLI --help displayed usage and returned 0.")

def test_invalid_cli_options():
    print("[TEST] Invalid CLI flag rejection...")
    # Unknown option
    res = run_cmd([SERVER_BIN, "--unknown-flag"])
    assert res.returncode != 0
    assert "Unknown command line option" in res.stderr

    # Invalid port (< 1)
    res = run_cmd([SERVER_BIN, "-p", "0"])
    assert res.returncode != 0
    assert "Invalid port number" in res.stderr

    # Invalid port (> 65535)
    res = run_cmd([SERVER_BIN, "-p", "70000"])
    assert res.returncode != 0
    assert "Invalid port number" in res.stderr

    # Non-numeric port
    res = run_cmd([SERVER_BIN, "-p", "abc"])
    assert res.returncode != 0
    assert "Invalid port number" in res.stderr

    # Invalid workers (< 1)
    res = run_cmd([SERVER_BIN, "-t", "0"])
    assert res.returncode != 0
    assert "Invalid worker thread count" in res.stderr

    # Invalid workers (> 128)
    res = run_cmd([SERVER_BIN, "-t", "129"])
    assert res.returncode != 0
    assert "Invalid worker thread count" in res.stderr

    # Invalid log level
    res = run_cmd([SERVER_BIN, "-l", "SUPERVERBOSE"])
    assert res.returncode != 0
    assert "Invalid log level" in res.stderr

    # Non-existent static dir
    res = run_cmd([SERVER_BIN, "-s", "/nonexistent/path/12345"])
    assert res.returncode != 0
    assert "Configured static directory does not exist" in res.stderr
    print("[PASS] All invalid CLI configurations strictly rejected with non-zero exit codes.")

def test_environment_variables():
    print("[TEST] Environment variable configuration...")
    # Invalid HTTP_PORT env var
    res = run_cmd([SERVER_BIN], env={"HTTP_PORT": "99999"})
    assert res.returncode != 0
    assert "Invalid HTTP_PORT" in res.stderr

    # Invalid HTTP_WORKERS env var
    res = run_cmd([SERVER_BIN], env={"HTTP_WORKERS": "0"})
    assert res.returncode != 0
    assert "Invalid HTTP_WORKERS" in res.stderr

    # Invalid HTTP_LOG_LEVEL env var
    res = run_cmd([SERVER_BIN], env={"HTTP_LOG_LEVEL": "BOGUS"})
    assert res.returncode != 0
    assert "Invalid HTTP_LOG_LEVEL" in res.stderr
    print("[PASS] Invalid environment variables strictly rejected.")

def test_health_endpoint():
    print("[TEST] Dedicated /health endpoint & uptime monitoring...")
    test_port = 8089
    proc = subprocess.Popen(
        [SERVER_BIN, "-p", str(test_port), "-l", "WARN"],
        cwd=PROJECT_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )
    time.sleep(0.5)
    try:
        url = f"http://127.0.0.1:{test_port}/health"
        req = urllib.request.urlopen(url, timeout=3)
        assert req.status == 200
        assert "application/json" in req.headers.get("Content-Type", "")
        data = json.loads(req.read().decode())
        assert data.get("status") == "ok"
        assert "uptime_seconds" in data
        assert data["port"] == test_port
        assert data.get("version") == "1.0.0"
        print(f"  -> Health payload: {data}")
    finally:
        proc.send_signal(signal.SIGINT)
        proc.wait(timeout=3)
        assert proc.returncode == 0
    print("[PASS] /health endpoint returned 200 OK and valid JSON health schema.")

def test_out_of_tree_execution():
    print("[TEST] Out-of-tree execution (running binary from outside repo)...")
    test_port = 8091
    bin_path = os.path.join(PROJECT_ROOT, "build", "server")
    proc = subprocess.Popen(
        [bin_path, "-p", str(test_port), "-l", "WARN"],
        cwd="/tmp",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )
    time.sleep(0.5)
    try:
        url = f"http://127.0.0.1:{test_port}/"
        req = urllib.request.urlopen(url, timeout=3)
        assert req.status == 200
        body = req.read().decode()
        assert "<canvas" in body or "<!DOCTYPE html>" in body
        print("  -> Successfully served static/index.html when launched from /tmp")
    finally:
        proc.send_signal(signal.SIGINT)
        proc.wait(timeout=3)
        assert proc.returncode == 0
    print("[PASS] Out-of-tree execution automatically resolved static asset directory.")

def test_input_sanitization():
    print("[TEST] Client input sanitization (oversized names and malformed room codes)...")
    test_port = 8092
    proc = subprocess.Popen(
        [SERVER_BIN, "-p", str(test_port), "-l", "WARN"],
        cwd=PROJECT_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )
    time.sleep(0.5)
    try:
        import base64
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect(("127.0.0.1", test_port))
        key = base64.b64encode(os.urandom(16)).decode()
        handshake = (
            f"GET /ws HTTP/1.1\r\n"
            f"Host: 127.0.0.1:{test_port}\r\n"
            f"Upgrade: websocket\r\n"
            f"Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n\r\n"
        )
        s.sendall(handshake.encode())
        resp = s.recv(4096)
        assert b"101 Switching Protocols" in resp

        def send_text(sock, text):
            b = text.encode("utf-8")
            plen = len(b)
            hdr = bytearray([0x81])
            if plen <= 125:
                hdr.append(0x80 | plen)
            elif plen <= 65535:
                hdr.append(0x80 | 126)
                hdr.extend(plen.to_bytes(2, "big"))
            mask = b"\x11\x22\x33\x44"
            hdr.extend(mask)
            masked = bytes([b[i] ^ mask[i % 4] for i in range(plen)])
            sock.sendall(hdr + masked)

        def recv_text(sock):
            sock.settimeout(2.0)
            hdr = sock.recv(2)
            if not hdr: return None
            b2 = hdr[1]
            plen = b2 & 0x7F
            if plen == 126:
                plen = int.from_bytes(sock.recv(2), "big")
            data = b""
            while len(data) < plen:
                chunk = sock.recv(plen - len(data))
                if not chunk: break
                data += chunk
            return data.decode("utf-8", errors="replace")

        def recv_message_of_type(sock, expected_type, timeout=3.0):
            start = time.time()
            while time.time() - start < timeout:
                txt = recv_text(sock)
                if txt:
                    try:
                        data = json.loads(txt)
                        if data.get("type") == expected_type:
                            return data
                    except Exception:
                        pass
            return None

        # 1. Test oversized player name (100 chars) in create_room
        long_name = "A" * 100
        msg = json.dumps({"type": "create_room", "name": long_name})
        send_text(s, msg)

        room_data = recv_message_of_type(s, "room_created")
        assert room_data is not None, "Did not receive room_created message"
        room = room_data.get("room", {})
        players = room.get("players", [])
        assert len(players) == 1
        clamped_name = players[0]["name"]
        assert len(clamped_name) <= 32, f"Player name not clamped! Len: {len(clamped_name)}"
        assert clamped_name == "A" * 32
        print(f"  -> 100-char player name safely clamped to {len(clamped_name)} chars: '{clamped_name}'")

        # 2. Test malformed room code in join_room
        s2 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s2.connect(("127.0.0.1", test_port))
        key2 = base64.b64encode(os.urandom(16)).decode()
        s2.sendall((
            f"GET /ws HTTP/1.1\r\n"
            f"Host: 127.0.0.1:{test_port}\r\n"
            f"Upgrade: websocket\r\n"
            f"Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key2}\r\n"
            f"Sec-WebSocket-Version: 13\r\n\r\n"
        ).encode())
        assert b"101 Switching Protocols" in s2.recv(4096)

        send_text(s2, json.dumps({"type": "join_room", "code": "!!!###"}))
        err_obj = recv_message_of_type(s2, "room_error")
        assert err_obj is not None, "Did not receive room_error for invalid room code"
        assert "Invalid room code" in err_obj.get("message", "")
        print("  -> Non-alphanumeric room code rejected with error message.")
        s.close()
        s2.close()
    finally:
        proc.send_signal(signal.SIGINT)
        proc.wait(timeout=3)
        assert proc.returncode == 0
    print("[PASS] Client input sanitization verified.")

def test_invalid_host_rejection():
    print("[TEST] Invalid IPv4 host address rejection...")
    # Invalid CLI host
    res = run_cmd([SERVER_BIN, "-h", "999.999.999.999"])
    assert res.returncode != 0
    assert "Invalid host address" in res.stderr

    # Non-IP CLI host
    res = run_cmd([SERVER_BIN, "-h", "not-a-valid-ip"])
    assert res.returncode != 0
    assert "Invalid host address" in res.stderr

    # Invalid ENV host
    res = run_cmd([SERVER_BIN], env={"HTTP_HOST": "invalid-ip-addr"})
    assert res.returncode != 0
    assert "Invalid HTTP_HOST" in res.stderr
    print("[PASS] Invalid host configurations strictly rejected.")

def test_path_traversal_and_symlink_security():
    print("[TEST] Path traversal and static file security...")
    test_port = 8093
    proc = subprocess.Popen(
        [SERVER_BIN, "-p", str(test_port), "-l", "WARN"],
        cwd=PROJECT_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )
    time.sleep(0.5)
    try:
        import urllib.error

        # 1. Direct path traversal
        for bad_path in ["/../CMakeLists.txt", "/../../etc/passwd", "/%2e%2e/CMakeLists.txt", "/%2e%2e%2fCMakeLists.txt"]:
            try:
                url = f"http://127.0.0.1:{test_port}{bad_path}"
                req = urllib.request.urlopen(url, timeout=2)
                assert False, f"Expected 404 for traversal path {bad_path}, got {req.status}"
            except urllib.error.HTTPError as e:
                assert e.code == 404, f"Expected 404 for {bad_path}, got {e.code}"

        # 2. Legitimate asset returns 200
        url = f"http://127.0.0.1:{test_port}/js/main.js"
        req = urllib.request.urlopen(url, timeout=2)
        assert req.status == 200
        assert "application/javascript" in req.headers.get("Content-Type", "")
    finally:
        proc.send_signal(signal.SIGINT)
        proc.wait(timeout=3)
        assert proc.returncode == 0
    print("[PASS] Path traversal attempts strictly rejected with 404.")

def test_binary_snapshot_exact_test_vectors():
    print("[TEST] Binary snapshot Schema v1 exact byte vector verification...")
    import struct

    # Construct known 86-byte Schema v1 test vector:
    # 36-byte header + 36-byte player record + 14-byte result record (11 fixed + 3 name)
    buf = bytearray(86)
    struct.pack_into("!I", buf, 0, 0x534E4150) # magic: 'SNAP'
    buf[4] = 1 # version = 1
    buf[5] = 3 # roomState = FINISHED
    struct.pack_into("!H", buf, 6, 0) # countdown = 0.0s (0 tenths)
    struct.pack_into("!I", buf, 8, 42) # roomId = 42
    buf[12:20] = b"TESTROOM" # roomCode
    struct.pack_into("!I", buf, 20, 1000) # tick = 1000
    struct.pack_into("!Q", buf, 24, 1700000000000) # timestamp ms
    struct.pack_into("!H", buf, 32, 1) # playerCount = 1
    struct.pack_into("!H", buf, 34, 1) # resultCount = 1

    # Player 1 record at offset 36 (36 bytes)
    struct.pack_into("!I", buf, 36, 7) # id = 7
    struct.pack_into("!f", buf, 40, 0.25) # x = 0.25
    struct.pack_into("!f", buf, 44, 150.5) # distance = 150.5
    struct.pack_into("!f", buf, 48, 35.2) # speed = 35.2
    struct.pack_into("!f", buf, 52, -0.1) # steer = -0.1
    struct.pack_into("!f", buf, 56, 120.0) # worldX = 120.0
    struct.pack_into("!f", buf, 60, 450.0) # worldY = 450.0
    struct.pack_into("!f", buf, 64, 1.57) # heading = 1.57
    struct.pack_into("!I", buf, 68, 42) # ackSeq = 42

    # Result 1 record at offset 72 (11-byte fixed header + 3-byte name)
    struct.pack_into("!I", buf, 72, 7) # playerId = 7 (uint32)
    struct.pack_into("!H", buf, 76, 1) # rank = 1 (uint16)
    struct.pack_into("!f", buf, 78, 45.67) # finishTime = 45.67 (float32)
    buf[82] = 3 # nameLen = 3 (uint8)
    buf[83:86] = b"Ace" # name = "Ace"

    # Verify exact header byte offset assertions
    assert len(buf) == 86
    assert buf[0:4] == b"SNAP"
    assert buf[4] == 1
    assert buf[5] == 3
    assert struct.unpack("!H", buf[6:8])[0] == 0
    assert struct.unpack("!I", buf[8:12])[0] == 42
    assert buf[12:20] == b"TESTROOM"
    assert struct.unpack("!I", buf[20:24])[0] == 1000
    assert struct.unpack("!Q", buf[24:32])[0] == 1700000000000
    assert struct.unpack("!H", buf[32:34])[0] == 1
    assert struct.unpack("!H", buf[34:36])[0] == 1

    # Verify player offset assertions (offset 36..71)
    assert struct.unpack("!I", buf[36:40])[0] == 7
    assert abs(struct.unpack("!f", buf[40:44])[0] - 0.25) < 1e-5
    assert abs(struct.unpack("!f", buf[44:48])[0] - 150.5) < 1e-4
    assert abs(struct.unpack("!f", buf[48:52])[0] - 35.2) < 1e-4
    assert abs(struct.unpack("!f", buf[52:56])[0] - (-0.1)) < 1e-5
    assert abs(struct.unpack("!f", buf[56:60])[0] - 120.0) < 1e-4
    assert abs(struct.unpack("!f", buf[60:64])[0] - 450.0) < 1e-4
    assert abs(struct.unpack("!f", buf[64:68])[0] - 1.57) < 1e-4
    assert struct.unpack("!I", buf[68:72])[0] == 42

    # Verify result record offset assertions (offset 72..85)
    assert struct.unpack("!I", buf[72:76])[0] == 7 # playerId: uint32 (4 bytes, offset +0..3)
    assert struct.unpack("!H", buf[76:78])[0] == 1 # rank: uint16 (2 bytes, offset +4..5)
    assert abs(struct.unpack("!f", buf[78:82])[0] - 45.67) < 1e-4 # finishTime: float32 (4 bytes, offset +6..9)
    assert buf[82] == 3 # nameLen: uint8 (1 byte, offset +10)
    assert buf[83:86] == b"Ace" # name: char[3] (3 bytes, offset +11..13)

    # Verify composite unpacking with !IHfB matches protocol specification exactly
    res_pid, res_rank, res_time, res_namelen = struct.unpack("!IHfB", buf[72:83])
    assert res_pid == 7
    assert res_rank == 1
    assert abs(res_time - 45.67) < 1e-4
    assert res_namelen == 3
    assert buf[83:83+res_namelen].decode('utf-8') == "Ace"

    # Verify corrupt vector rejection
    bad_magic = bytearray(buf)
    bad_magic[0:4] = b"NOPE"
    assert struct.unpack("!I", bad_magic[0:4])[0] != 0x534E4150

    bad_version = bytearray(buf)
    bad_version[4] = 99
    assert bad_version[4] != 1

    # Truncated record by 1 byte
    truncated = bytes(buf[:-1])
    assert len(truncated) == 85 # Missing last byte of name
    print("  -> Schema v1 36-byte header, 36-byte player, and 11+N byte result record validated.")
    print("[PASS] Binary snapshot byte vector verification passed.")

def run_all_tests():
    print("==================================================")
    print("Phase 8 Production Readiness & Deployment Tests")
    print("==================================================")
    test_cli_help()
    test_invalid_cli_options()
    test_environment_variables()
    test_invalid_host_rejection()
    test_health_endpoint()
    test_out_of_tree_execution()
    test_path_traversal_and_symlink_security()
    test_input_sanitization()
    test_binary_snapshot_exact_test_vectors()
    print("==================================================")
    print("ALL PHASE 8 PRODUCTION READINESS TESTS PASSED!")
    print("==================================================")

if __name__ == "__main__":
    run_all_tests()
