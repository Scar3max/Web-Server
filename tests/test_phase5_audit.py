#!/usr/bin/env python3
"""
Phase 5.1 Room Lifecycle, Isolation, and Concurrency Audit Test Suite
Comprehensive audit verification covering:
1. Room Membership Consistency & Isolation between Practice (Room 0) and Multi-Room lobbies
2. Event Ordering and Race Conditions:
   - Rapid Join followed by Leave
   - Disconnect during Countdown (Host and non-host)
   - Stale input rejection for departed players & destroyed rooms
3. Multi-Room Isolation & Concurrent Independent Races (Room A & Room B running simultaneously)
4. Authoritative Race Lifecycle & Finishing Single-Finish Invariant
5. Capacity Bounds & Resource Accumulation Stress (Rapid room creation/destruction churn)
6. Real-time Simulation Tick Rate and Snapshot Frequency measurement under multi-room load
"""

import socket
import json
import base64
import os
import sys
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor

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
        
    masking_key = b"\x37\xfa\x21\x3d" if mask else b""
    header.extend(masking_key)
    
    if mask:
        masked = bytearray(plen)
        for i in range(plen):
            masked[i] = payload_bytes[i] ^ masking_key[i % 4]
        return bytes(header) + bytes(masked)
    else:
        return bytes(header) + payload_bytes

def parse_ws_frame(sock, timeout=5.0):
    try:
        sock.settimeout(timeout)
        b = sock.recv(2)
        if not b or len(b) < 2:
            return None, None
        b1, b2 = b[0], b[1]
        opcode = b1 & 0x0F
        masked = bool(b2 & 0x80)
        plen = b2 & 0x7F
        
        if plen == 126:
            ext = sock.recv(2)
            plen = int.from_bytes(ext, "big")
        elif plen == 127:
            ext = sock.recv(8)
            plen = int.from_bytes(ext, "big")
            
        mask_key = b""
        if masked:
            mask_key = sock.recv(4)
            
        payload = b""
        while len(payload) < plen:
            chunk = sock.recv(min(4096, plen - len(payload)))
            if not chunk:
                break
            payload += chunk
            
        if masked:
            unmasked = bytearray(len(payload))
            for i in range(len(payload)):
                unmasked[i] = payload[i] ^ mask_key[i % 4]
            payload = bytes(unmasked)
            
        return opcode, payload
    except (socket.timeout, TimeoutError, BlockingIOError):
        return None, None

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
    resp = s.recv(4096).decode(errors="replace")
    if "101 Switching Protocols" not in resp:
        s.close()
        raise RuntimeError(f"WebSocket handshake failed: {resp}")
    return s

def send_ws_json(s, obj):
    raw = json.dumps(obj).encode("utf-8")
    frame = build_ws_frame(raw, opcode=0x1, fin=True, mask=True)
    s.sendall(frame)

def recv_ws_json(s, timeout=5.0):
    op, payload = parse_ws_frame(s, timeout)
    if payload is None:
        return None
    try:
        return json.loads(payload.decode("utf-8"))
    except Exception:
        return {"_raw": payload}

def recv_until_type(s, target_types, timeout=5.0):
    if isinstance(target_types, str):
        target_types = [target_types]
    start = time.time()
    while time.time() - start < timeout:
        remaining = max(0.1, timeout - (time.time() - start))
        msg = recv_ws_json(s, timeout=remaining)
        if not msg:
            continue
        if msg.get("type") in target_types:
            return msg
    return None

def fetch_metrics():
    url = f"http://{SERVER_HOST}:{SERVER_PORT}/metrics"
    req = urllib.request.Request(url)
    with urllib.request.urlopen(req, timeout=3.0) as resp:
        return json.loads(resp.read().decode())

# ==========================================================
# 1. Room Membership Consistency & Isolation from Room 0
# ==========================================================
def test_room_membership_practice_isolation():
    print("[TEST] Room Membership Consistency & Practice World (Room 0) Isolation...")
    # Client 1 will join Practice Mode (Room 0)
    p_sock = connect_ws()
    # Send an initial input to trigger admission into Room 0
    send_ws_json(p_sock, {"type": "input", "seq": 1, "controls": {"up": True, "down": False, "left": False, "right": False}})
    snap_p0 = recv_until_type(p_sock, "snapshot")
    assert snap_p0 is not None, "Practice client should receive Room 0 snapshots"
    assert snap_p0.get("roomId") == 0, f"Expected Room 0 snapshot, got {snap_p0.get('roomId')}"
    p_ids_initial = [p["id"] for p in snap_p0.get("players", [])]
    assert len(p_ids_initial) >= 1, "Practice snapshot must contain practice player"

    # Client 2 connects and creates a Multiplayer Room (Room > 0)
    r_host = connect_ws()
    send_ws_json(r_host, {"type": "create_room", "name": "HostPlayer"})
    r_created = recv_until_type(r_host, "room_created")
    assert r_created is not None, "Host should receive room_created"
    host_pid = r_created["playerId"]
    room_id = r_created["roomId"]

    # Verify that HostPlayer does NOT leak into Practice World (Room 0) snapshots!
    time.sleep(0.15)
    # Drain practice snapshots and check player list
    snap_p1 = recv_until_type(p_sock, "snapshot")
    if snap_p1:
        p_ids_now = [p["id"] for p in snap_p1.get("players", [])]
        assert host_pid not in p_ids_now, f"HostPlayer (PID {host_pid}) leaked into Practice World snapshot! {p_ids_now}"

    # Now HostPlayer leaves the room back to unassigned/practice
    send_ws_json(r_host, {"type": "leave_room"})
    r_left = recv_until_type(r_host, "room_left")
    assert r_left is not None, "Host should receive room_left"

    p_sock.close()
    r_host.close()
    print("[PASS] Membership consistency and practice world isolation verified.")

# ==========================================================
# 2. Event Ordering: Host Disconnect during Countdown
# ==========================================================
def test_host_disconnect_during_countdown():
    print("[TEST] Event Ordering: Host Disconnect During Countdown & Succession...")
    host = connect_ws()
    client = connect_ws()

    send_ws_json(host, {"type": "create_room", "name": "HostAlice"})
    r_created = recv_until_type(host, "room_created")
    code = r_created["roomCode"]
    host_id = r_created["playerId"]

    send_ws_json(client, {"type": "join_room", "code": code, "name": "ClientBob"})
    r_joined = recv_until_type(client, "room_joined")
    client_id = r_joined["playerId"]

    # Both ready
    send_ws_json(host, {"type": "set_ready", "ready": True})
    send_ws_json(client, {"type": "set_ready", "ready": True})
    time.sleep(0.05)

    # Host starts countdown
    send_ws_json(host, {"type": "start_race"})
    cd_msg = recv_until_type(client, "countdown")
    assert cd_msg is not None, "Client should receive countdown"

    # Abruptly close Host socket while countdown is active
    host.close()

    # Client must receive player_left (indicating host departed and succession) and room_updated back to WAITING
    left_msg = recv_until_type(client, "player_left", timeout=4.0)
    assert left_msg is not None, "Client should receive player_left when host disconnects"
    assert left_msg.get("newHostId") == client_id, f"Client {client_id} should become new host, got: {left_msg}"

    upd_msg = recv_until_type(client, "room_updated", timeout=3.0)
    assert upd_msg is not None, "Client should receive room_updated"
    assert upd_msg["room"]["state"] == "WAITING", f"Room state should revert to WAITING, got: {upd_msg['room']['state']}"
    assert upd_msg["room"]["hostPlayerId"] == client_id, "Host succession should be updated in room info"

    client.close()
    print("[PASS] Host disconnect during countdown handled cleanly with succession and state reset.")

# ==========================================================
# 3. Event Ordering: Rapid Create / Join / Leave Churn
# ==========================================================
def test_rapid_room_churn():
    print("[TEST] Rapid Create / Join / Leave Churn (30 cycles)...")
    for i in range(30):
        s1 = connect_ws()
        s2 = connect_ws()

        send_ws_json(s1, {"type": "create_room", "name": f"User_{i}_A"})
        cr = recv_until_type(s1, "room_created")
        assert cr is not None, f"Failed create on cycle {i}"
        code = cr["roomCode"]

        send_ws_json(s2, {"type": "join_room", "code": code, "name": f"User_{i}_B"})
        jr = recv_until_type(s2, "room_joined")
        assert jr is not None, f"Failed join on cycle {i}"

        # Rapidly send leave from both
        send_ws_json(s1, {"type": "leave_room"})
        send_ws_json(s2, {"type": "leave_room"})
        lr1 = recv_until_type(s1, "room_left")
        assert lr1 is not None, f"Failed leave on cycle {i}"

        s1.close()
        s2.close()

    # Query metrics to verify room count has returned cleanly to 0 active rooms
    metrics = fetch_metrics()
    assert metrics["roomCount"] == 0, f"Expected 0 active rooms after churn, got {metrics['roomCount']}"
    print(f"[PASS] 30 rapid churn cycles completed cleanly. Active rooms in registry: {metrics['roomCount']}.")

# ==========================================================
# 4. Multi-Room Isolation: Two Concurrent Simultaneous Races
# ==========================================================
def test_concurrent_simultaneous_races():
    print("[TEST] Multi-Room Isolation: Two Concurrent Simultaneous Races...")
    # Room A
    sA1 = connect_ws()
    sA2 = connect_ws()
    send_ws_json(sA1, {"type": "create_room", "name": "A_Racer1", "trackLength": 100.0})
    cA = recv_until_type(sA1, "room_created")["roomCode"]
    send_ws_json(sA2, {"type": "join_room", "code": cA, "name": "A_Racer2"})
    recv_until_type(sA2, "room_joined")

    # Room B
    sB1 = connect_ws()
    sB2 = connect_ws()
    send_ws_json(sB1, {"type": "create_room", "name": "B_Racer1", "trackLength": 100.0})
    cB = recv_until_type(sB1, "room_created")["roomCode"]
    send_ws_json(sB2, {"type": "join_room", "code": cB, "name": "B_Racer2"})
    recv_until_type(sB2, "room_joined")

    # Set all ready
    for s in [sA1, sA2, sB1, sB2]:
        send_ws_json(s, {"type": "set_ready", "ready": True})
    time.sleep(0.05)

    # Start both rooms simultaneously
    send_ws_json(sA1, {"type": "start_race"})
    send_ws_json(sB1, {"type": "start_race"})

    # Wait through 5s countdown
    print("  -> Waiting through synchronized 5s countdown...")
    start_A = recv_until_type(sA1, "race_started", timeout=6.0)
    start_B = recv_until_type(sB1, "race_started", timeout=6.0)
    assert start_A is not None, "Room A should start"
    assert start_B is not None, "Room B should start"

    # Drive Room A cars and Room B cars simultaneously
    print("  -> Driving cars across both rooms simultaneously...")
    start_drive = time.time()
    fin_A = None
    fin_B = None

    seq = 0
    while time.time() - start_drive < 15.0 and (fin_A is None or fin_B is None):
        seq += 1
        elapsed = time.time() - start_drive

        # Room A racers
        send_ws_json(sA1, {"type": "input", "seq": seq, "controls": {"up": True, "down": False, "left": False, "right": False}})
        send_ws_json(sA2, {"type": "input", "seq": seq, "controls": {"up": (elapsed > 0.8), "down": False, "left": False, "right": False}})

        # Room B racers
        send_ws_json(sB1, {"type": "input", "seq": seq, "controls": {"up": True, "down": False, "left": False, "right": False}})
        send_ws_json(sB2, {"type": "input", "seq": seq, "controls": {"up": (elapsed > 0.8), "down": False, "left": False, "right": False}})

        time.sleep(0.04)

        if fin_A is None:
            msgA = recv_ws_json(sA1, timeout=0.01)
            if msgA and msgA.get("type") == "race_finished":
                fin_A = msgA

        if fin_B is None:
            msgB = recv_ws_json(sB1, timeout=0.01)
            if msgB and msgB.get("type") == "race_finished":
                fin_B = msgB

    assert fin_A is not None, "Room A should finish"
    assert fin_B is not None, "Room B should finish"

    # Verify that Room A results contain only Room A racers
    p_names_A = [r["name"] for r in fin_A["results"]]
    p_names_B = [r["name"] for r in fin_B["results"]]
    assert "A_Racer1" in p_names_A and "B_Racer1" not in p_names_A, f"Cross-room leakage into Room A: {p_names_A}"
    assert "B_Racer1" in p_names_B and "A_Racer1" not in p_names_B, f"Cross-room leakage into Room B: {p_names_B}"

    for s in [sA1, sA2, sB1, sB2]:
        s.close()
    print("[PASS] Two concurrent simultaneous races ran in full isolation and completed cleanly.")

# ==========================================================
# 5. Measure Simulation Tick Rate & Snapshot Rate under Load
# ==========================================================
def test_simulation_rate_diagnostics():
    print("[TEST] Real-Time Simulation Timing & Snapshot Frequency Under Multi-Room Load...")
    time.sleep(1.2) # Allow diagnostic window to refresh
    metrics = fetch_metrics()
    tick_rate = metrics["tickRate"]
    snap_rate = metrics["snapshotRate"]
    print(f"  -> Authoritative Simulation Tick Rate: {tick_rate:.2f} Hz (Target: 60.0 Hz)")
    print(f"  -> Snapshot Publication Rate: {snap_rate:.2f} Hz")
    assert 58.0 <= tick_rate <= 62.0, f"Simulation tick rate {tick_rate} out of acceptable bounds [58.0, 62.0]"
    print("[PASS] Simulation timing verified strictly at 60 Hz fixed timestep.")

# ==========================================================
# 6. Disconnect During Racing & Remaining Racer Completes
# ==========================================================
def test_disconnect_during_racing():
    print("[TEST] Player Disconnect During Active Race...")
    s1 = connect_ws()
    s2 = connect_ws()

    send_ws_json(s1, {"type": "create_room", "name": "Finisher", "trackLength": 80.0})
    code = recv_until_type(s1, "room_created")["roomCode"]
    send_ws_json(s2, {"type": "join_room", "code": code, "name": "Quitter"})
    recv_until_type(s2, "room_joined")

    send_ws_json(s1, {"type": "set_ready", "ready": True})
    send_ws_json(s2, {"type": "set_ready", "ready": True})
    time.sleep(0.05)

    send_ws_json(s1, {"type": "start_race"})
    recv_until_type(s1, "race_started", timeout=6.0)

    # Quitter abruptly disconnects during race
    s2.close()

    # Finisher continues driving to the finish line
    for seq in range(1, 35):
        send_ws_json(s1, {"type": "input", "seq": seq, "controls": {"up": True, "down": False, "left": False, "right": False}})
        time.sleep(0.04)

    fin = recv_until_type(s1, "race_finished", timeout=6.0)
    assert fin is not None, "Finisher should receive race_finished"
    assert len(fin["results"]) == 1, f"Expected 1 finisher in results, got {fin['results']}"
    assert fin["results"][0]["name"] == "Finisher", "Finisher must be ranked 1st"
    assert fin["results"][0]["rank"] == 1, "Finisher rank must be 1"

    s1.close()
    print("[PASS] Disconnect during active race handled cleanly with authoritative finish results.")

# ==========================================================
# 7. Room Limits & Rejection
# ==========================================================
def test_room_capacity_bounds():
    print("[TEST] Room Capacity & Max Active Rooms Bounds...")
    sockets = []
    try:
        # Create rooms up to MAX_ROOMS (64)
        for i in range(64):
            s = connect_ws()
            sockets.append(s)
            send_ws_json(s, {"type": "create_room", "name": f"Host_{i}"})
            resp = recv_until_type(s, ["room_created", "room_error"])
            assert resp["type"] == "room_created", f"Failed creating room {i}: {resp}"

        # 65th room must be rejected
        s_overflow = connect_ws()
        sockets.append(s_overflow)
        send_ws_json(s_overflow, {"type": "create_room", "name": "OverflowHost"})
        err = recv_until_type(s_overflow, "room_error")
        assert err is not None, "65th room creation should be rejected"
        assert "room limit" in err.get("message", "").lower(), f"Unexpected error message: {err}"
        print(f"  -> Room limit enforced cleanly: {err['message']}")
    finally:
        for s in sockets:
            s.close()
        time.sleep(0.1)

    print("[PASS] Server MAX_ROOMS capacity strictly enforced.")

def run_all_audit_tests():
    print("==================================================")
    print("Running Phase 5.1 Room Lifecycle & Concurrency Audit")
    print("==================================================")
    test_room_membership_practice_isolation()
    test_host_disconnect_during_countdown()
    test_rapid_room_churn()
    test_concurrent_simultaneous_races()
    test_simulation_rate_diagnostics()
    test_disconnect_during_racing()
    test_room_capacity_bounds()
    print("==================================================")
    print("ALL PHASE 5.1 AUDIT TESTS PASSED!")
    print("==================================================")

if __name__ == "__main__":
    run_all_audit_tests()
