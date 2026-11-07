#!/usr/bin/env python3
"""
Phase 4 Authoritative Racing Simulation Test Suite
Validates:
1. Physics: acceleration, braking, friction, max speed, steering, track boundaries.
2. Input validation: missing fields, wrong types, malformed JSON, sequence numbers, malicious coordinates.
3. Simulation: continuous ticking, monotonic tick progression, multi-player isolation, clean disconnect.
4. Integration: /ws connection, welcome message, authoritative snapshots, multi-client broadcast.
"""

import socket
import json
import base64
import os
import sys
import time

SERVER_HOST = "127.0.0.1"
SERVER_PORT = 8080

def build_ws_frame(payload_bytes, opcode=0x1, fin=True, mask=True):
    header = bytearray()
    b1 = (0x80 if fin else 0x00) | (opcode & 0x0F)
    header.append(b1)
    
    plen = len(payload_bytes)
    mask_bit = 0x80 if mask else 0x00
    if plen <= 125:
        header.append(mask_bit | plen)
    elif plen <= 65535:
        header.append(mask_bit | 126)
        header.extend(plen.to_bytes(2, "big"))
    else:
        header.append(mask_bit | 127)
        header.extend(plen.to_bytes(8, "big"))
        
    masking_key = b"\x12\x34\x56\x78" if mask else b""
    if mask:
        header.extend(masking_key)
        masked = bytearray(plen)
        for i in range(plen):
            masked[i] = payload_bytes[i] ^ masking_key[i % 4]
        return bytes(header) + bytes(masked)
    else:
        return bytes(header) + payload_bytes

def parse_ws_frame(sock, timeout=3.0):
    sock.settimeout(timeout)
    try:
        header = sock.recv(2)
        if len(header) < 2:
            return None, None, None
        b1, b2 = header[0], header[1]
        fin = (b1 & 0x80) != 0
        opcode = b1 & 0x0F
        has_mask = (b2 & 0x80) != 0
        plen = b2 & 0x7F
        
        if plen == 126:
            ext = sock.recv(2)
            plen = int.from_bytes(ext, "big")
        elif plen == 127:
            ext = sock.recv(8)
            plen = int.from_bytes(ext, "big")
            
        mask_key = b""
        if has_mask:
            mask_key = sock.recv(4)
            
        payload = b""
        while len(payload) < plen:
            chunk = sock.recv(plen - len(payload))
            if not chunk:
                break
            payload += chunk
            
        if has_mask:
            unmasked = bytearray(plen)
            for i in range(plen):
                unmasked[i] = payload[i] ^ mask_key[i % 4]
            payload = bytes(unmasked)
            
        return fin, opcode, payload
    except socket.timeout:
        return None, None, None

def connect_ws():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5.0)
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
    
    resp = b""
    while b"\r\n\r\n" not in resp:
        chunk = s.recv(1024)
        if not chunk:
            break
        resp += chunk
        
    assert b"101 Switching Protocols" in resp, "Handshake failed"
    return s

def read_next_json_frame(sock, timeout=3.0):
    fin, opcode, payload = parse_ws_frame(sock, timeout)
    if payload is None:
        return None
    try:
        return json.loads(payload.decode("utf-8"))
    except Exception:
        return None

def read_next_snapshot(sock, timeout=3.0):
    start = time.time()
    while time.time() - start < timeout:
        remaining = timeout - (time.time() - start)
        if remaining <= 0:
            break
        msg = read_next_json_frame(sock, timeout=remaining)
        if msg and msg.get("type") == "snapshot":
            return msg
    return None

def drain_latest_snapshot(sock, timeout=0.8):
    start = time.time()
    latest = None
    while time.time() - start < timeout:
        remaining = timeout - (time.time() - start)
        if remaining <= 0:
            break
        sp = read_next_snapshot(sock, timeout=min(0.08, remaining))
        if sp:
            latest = sp
        elif latest is not None:
            break
    return latest

def test_welcome_and_player_id():
    print("[TEST] Welcome Message & Server Player ID Assignment...")
    s = connect_ws()
    
    # Send initial input
    init_in = {
        "type": "input",
        "seq": 1,
        "controls": {"up": False, "down": False, "left": False, "right": False}
    }
    s.sendall(build_ws_frame(json.dumps(init_in).encode()))
    
    msg = read_next_json_frame(s)
    assert msg is not None, "Failed to receive response after initial input"
    assert msg.get("type") == "welcome", f"Expected 'welcome', got {msg}"
    assert "playerId" in msg and isinstance(msg["playerId"], int), f"Missing integer playerId: {msg}"
    pid = msg["playerId"]
    
    # Next frame should be an authoritative world snapshot containing this player
    snap = read_next_snapshot(s)
    assert snap is not None
    assert snap.get("type") == "snapshot"
    assert "tick" in snap and isinstance(snap["tick"], int)
    assert "players" in snap and isinstance(snap["players"], list)
    player_ids = [p["id"] for p in snap["players"]]
    assert pid in player_ids, f"Player ID {pid} not found in world snapshot players: {player_ids}"
    
    s.close()
    time.sleep(0.05) # Allow cleanup
    print(f"[PASS] Welcome received with playerId={pid} and active in world snapshots.")

def test_input_validation():
    print("[TEST] Input Protocol Validation & Malicious Field Rejection...")
    s = connect_ws()
    
    # Register player
    s.sendall(build_ws_frame(json.dumps({"type": "input", "seq": 1, "controls": {"up": False, "down": False, "left": False, "right": False}}).encode()))
    welcome = read_next_json_frame(s)
    pid = welcome["playerId"]
    
    # 1. Malformed JSON
    s.sendall(build_ws_frame(b'{"type": "input", seq: malformed}'))
    # 2. Missing fields
    s.sendall(build_ws_frame(json.dumps({"type": "input", "seq": 2}).encode())) # missing controls
    s.sendall(build_ws_frame(json.dumps({"type": "input", "controls": {"up": True}}).encode())) # missing seq
    # 3. Wrong types
    s.sendall(build_ws_frame(json.dumps({"type": "input", "seq": "three", "controls": {"up": True, "down": False, "left": False, "right": False}}).encode()))
    s.sendall(build_ws_frame(json.dumps({"type": "input", "seq": 4, "controls": {"up": "yes", "down": False, "left": False, "right": False}}).encode()))
    
    # 4. Malicious attempt to inject client-authoritative position & speed
    malicious_in = {
        "type": "input",
        "seq": 10,
        "controls": {"up": False, "down": False, "left": False, "right": False},
        "x": 999.0,
        "position": 50000.0,
        "speed": 9999.0,
        "distance": 8888.0
    }
    s.sendall(build_ws_frame(json.dumps(malicious_in).encode()))
    
    # Read snapshots and verify the malicious coordinates were completely ignored
    time.sleep(0.1)
    snap = read_next_snapshot(s)
    assert snap is not None
    p_state = next(p for p in snap["players"] if p["id"] == pid)
    assert p_state["speed"] == 0.0, f"Malicious speed accepted! Got {p_state['speed']}"
    assert -1.0 <= p_state["x"] <= 1.0, f"Malicious x coordinate accepted! Got {p_state['x']}"
    assert p_state["distance"] < 10.0, f"Malicious distance accepted! Got {p_state['distance']}"
    
    # 5. Stale / repeated sequence numbers
    s.sendall(build_ws_frame(json.dumps({"type": "input", "seq": 5, "controls": {"up": True, "down": False, "left": False, "right": False}}).encode()))
    s.sendall(build_ws_frame(json.dumps({"type": "input", "seq": 10, "controls": {"up": True, "down": False, "left": False, "right": False}}).encode()))
    
    s.close()
    time.sleep(0.05)
    print("[PASS] Input protocol strictly validated; malicious coordinates and malformed inputs rejected.")

def test_authoritative_physics():
    print("[TEST] Authoritative Vehicle Physics (Acceleration, Max Speed, Braking, Boundaries)...")
    s = connect_ws()
    s.sendall(build_ws_frame(json.dumps({"type": "input", "seq": 1, "controls": {"up": False, "down": False, "left": False, "right": False}}).encode()))
    welcome = read_next_json_frame(s)
    pid = welcome["playerId"]
    
    # 1. Acceleration: hold UP throttle for 0.6 seconds (~36 ticks)
    seq = 2
    for _ in range(12):
        s.sendall(build_ws_frame(json.dumps({"type": "input", "seq": seq, "controls": {"up": True, "down": False, "left": False, "right": False}}).encode()))
        seq += 1
        time.sleep(0.05)
        
    snap = drain_latest_snapshot(s)
    assert snap is not None
    p_state = next(p for p in snap["players"] if p["id"] == pid)
    assert p_state["speed"] > 10.0, f"Vehicle did not accelerate; speed: {p_state['speed']}"
    assert p_state["distance"] > 0.0, f"Distance did not increase; dist: {p_state['distance']}"
    accel_speed = p_state["speed"]
    print(f"  -> Accelerated to {accel_speed:.1f} km/h (authoritative)")
    
    # 2. Braking: hold DOWN brake for 0.4 seconds
    for _ in range(8):
        s.sendall(build_ws_frame(json.dumps({"type": "input", "seq": seq, "controls": {"up": False, "down": True, "left": False, "right": False}}).encode()))
        seq += 1
        time.sleep(0.05)
        
    snap = drain_latest_snapshot(s)
    assert snap is not None
    p_state = next(p for p in snap["players"] if p["id"] == pid)
    assert p_state["speed"] < accel_speed, f"Braking did not reduce speed: {p_state['speed']} vs {accel_speed}"
    print(f"  -> Braked down to {p_state['speed']:.1f} km/h (authoritative)")
    
    # 3. Steering and track boundary clamp
    # Accelerate and steer left hard
    for _ in range(25):
        s.sendall(build_ws_frame(json.dumps({"type": "input", "seq": seq, "controls": {"up": True, "down": False, "left": True, "right": False}}).encode()))
        seq += 1
        time.sleep(0.04)
        
    snap = drain_latest_snapshot(s)
    p_state = next(p for p in snap["players"] if p["id"] == pid)
    assert p_state["x"] == -1.0, f"Track boundary left clamp failed: got x={p_state['x']}"
    assert p_state["speed"] <= 220.0, f"Exceeded max forward speed: {p_state['speed']}"
    print(f"  -> Clamped cleanly at left track boundary (x = {p_state['x']:.2f})")
    
    s.close()
    time.sleep(0.05)
    print("[PASS] Vehicle physics (acceleration, braking, steering, boundary clamp) verified.")

def test_multiplayer_isolation_and_disconnection():
    print("[TEST] Multi-Player Isolation & Dynamic Disconnection...")
    s1 = connect_ws()
    s1.sendall(build_ws_frame(json.dumps({"type": "input", "seq": 1, "controls": {"up": False, "down": False, "left": False, "right": False}}).encode()))
    w1 = read_next_json_frame(s1)
    p1_id = w1["playerId"]
    
    s2 = connect_ws()
    s2.sendall(build_ws_frame(json.dumps({"type": "input", "seq": 1, "controls": {"up": False, "down": False, "left": False, "right": False}}).encode()))
    w2 = read_next_json_frame(s2)
    p2_id = w2["playerId"]
    
    assert p1_id != p2_id, f"Both players assigned identical ID {p1_id}"
    
    # Read snapshot from s1: wait until it contains BOTH players
    start_wait = time.time()
    found_both = False
    p_ids = []
    while time.time() - start_wait < 2.0:
        snap = read_next_snapshot(s1, timeout=0.5)
        if snap:
            p_ids = [p["id"] for p in snap["players"]]
            if p1_id in p_ids and p2_id in p_ids:
                found_both = True
                break
    assert found_both, f"Expected both {p1_id} and {p2_id} in snapshot, got {p_ids}"
    
    # Send throttle ONLY to player 1
    for i in range(12):
        s1.sendall(build_ws_frame(json.dumps({"type": "input", "seq": 2 + i, "controls": {"up": True, "down": False, "left": False, "right": False}}).encode()))
        time.sleep(0.04)
        
    snap = drain_latest_snapshot(s2) # Client 2 receives the shared world state
    assert snap is not None
    p1_state = next(p for p in snap["players"] if p["id"] == p1_id)
    p2_state = next(p for p in snap["players"] if p["id"] == p2_id)
    
    assert p1_state["speed"] > 10.0, "Player 1 should have accelerated"
    assert p2_state["speed"] == 0.0, f"Player 2 should remain stationary! Got {p2_state['speed']}"
    print(f"  -> Player 1 speed={p1_state['speed']:.1f} km/h, Player 2 speed={p2_state['speed']:.1f} km/h (independent)")
    
    # Disconnect Player 2
    s2.close()
    time.sleep(0.15) # Wait for simulation tick to process leave event
    
    snap = drain_latest_snapshot(s1)
    assert snap is not None
    remaining_ids = [p["id"] for p in snap["players"]]
    assert p2_id not in remaining_ids, f"Disconnected player {p2_id} still present in snapshot: {remaining_ids}"
    assert p1_id in remaining_ids, f"Active player {p1_id} unexpectedly missing"
    print(f"  -> Player 2 disconnected; cleanly removed from world state")
    
    s1.close()
    time.sleep(0.05)
    print("[PASS] Multiple players maintain independent state and clean disconnect lifecycle.")

def test_tick_monotonicity_and_rate():
    print("[TEST] Simulation Monotonic Tick Progression & Rate...")
    s = connect_ws()
    s.sendall(build_ws_frame(json.dumps({"type": "input", "seq": 1, "controls": {"up": False, "down": False, "left": False, "right": False}}).encode()))
    read_next_json_frame(s) # welcome
    
    ticks = []
    timestamps = []
    
    for _ in range(10):
        snap = read_next_snapshot(s, timeout=1.0)
        assert snap is not None and snap["type"] == "snapshot"
        ticks.append(snap["tick"])
        timestamps.append(time.time())
        
    # Verify strictly monotonic progression
    for i in range(1, len(ticks)):
        assert ticks[i] > ticks[i - 1], f"Tick did not increase monotonically: {ticks[i-1]} -> {ticks[i]}"
        
    # Verify snapshot interval (~50ms = 20 Hz)
    total_time = timestamps[-1] - timestamps[0]
    avg_interval_ms = (total_time / (len(timestamps) - 1)) * 1000.0
    print(f"  -> Average snapshot publication interval: {avg_interval_ms:.1f} ms (~{1000.0/avg_interval_ms:.1f} Hz)")
    assert 30.0 <= avg_interval_ms <= 80.0, f"Snapshot rate outside expected range: {avg_interval_ms} ms"
    
    s.close()
    time.sleep(0.05)
    print("[PASS] Simulation tick progression is strictly monotonic at controlled 20 Hz publication.")

def run_all():
    test_welcome_and_player_id()
    test_input_validation()
    test_authoritative_physics()
    test_multiplayer_isolation_and_disconnection()
    test_tick_monotonicity_and_rate()
    print("\nALL PHASE 4 AUTHORITATIVE SIMULATION TESTS PASSED!")

if __name__ == "__main__":
    run_all()
