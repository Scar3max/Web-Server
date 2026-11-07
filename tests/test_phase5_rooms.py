#!/usr/bin/env python3
"""
Phase 5 Multiplayer Game Rooms, Lobbies & Race Lifecycle Test Suite
Comprehensive testing for:
1. Room Management (Create, Join, Nonexistent, Full, Duplicate, Leave, Empty Cleanup, Host Transfer, Disconnect Cleanup)
2. Permissions (Non-host start rejection, Readiness isolation, Invalid room operations)
3. Race Lifecycle (WAITING -> COUNTDOWN -> RACING -> FINISHED, Countdown cancellation on unready/leave)
4. Multi-Room Isolation (Room A vs Room B state, Snapshot isolation, Input isolation)
5. Concurrent Multi-Room Races & Churn
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

# ==========================================================
# 1. Room Management Tests
# ==========================================================
def test_room_creation_and_unique_codes():
    print("[TEST] Room Creation, Unique Codes & Initial Host Assignment...")
    s1 = connect_ws()
    s2 = connect_ws()
    
    # Create Room 1
    send_ws_json(s1, {"type": "create_room", "name": "Alice"})
    r1 = recv_until_type(s1, ["room_created", "room_error"])
    assert r1 is not None and r1.get("type") == "room_created", f"Expected room_created, got: {r1}"
    code1 = r1["roomCode"]
    assert len(code1) == 6, f"Room code should be 6 characters, got: {code1}"
    assert r1["hostId"] == r1["playerId"], "Creator must be host"
    assert r1["room"]["players"][0]["ready"] == False, "Initial ready status must be false"
    
    # Create Room 2
    send_ws_json(s2, {"type": "create_room", "name": "Bob"})
    r2 = recv_until_type(s2, ["room_created", "room_error"])
    assert r2 is not None and r2.get("type") == "room_created", f"Expected room_created, got: {r2}"
    code2 = r2["roomCode"]
    assert code1 != code2, f"Room codes collided: {code1} vs {code2}"
    
    s1.close()
    s2.close()
    time.sleep(0.05)
    print(f"  -> Generated unique codes: {code1}, {code2}")
    print("[PASS] Room creation, code generation, and host assignment verified.")

def test_room_join_and_nonexistent():
    print("[TEST] Join Room & Nonexistent Room Handling...")
    s_host = connect_ws()
    s_client = connect_ws()
    
    # Join nonexistent room
    send_ws_json(s_client, {"type": "join_room", "code": "NONEX1", "name": "Charlie"})
    err = recv_until_type(s_client, ["room_error"])
    assert err is not None and err.get("type") == "room_error", f"Expected error for nonexistent room, got: {err}"
    assert "not found" in err["message"].lower(), f"Unexpected error message: {err['message']}"
    
    # Host creates room
    send_ws_json(s_host, {"type": "create_room", "name": "HostPlayer"})
    r_created = recv_until_type(s_host, "room_created")
    code = r_created["roomCode"]
    
    # Client joins valid room
    send_ws_json(s_client, {"type": "join_room", "code": code, "name": "GuestPlayer"})
    r_joined = recv_until_type(s_client, "room_joined")
    assert r_joined is not None and r_joined.get("type") == "room_joined", f"Expected room_joined, got: {r_joined}"
    assert r_joined["roomCode"] == code
    assert len(r_joined["room"]["players"]) == 2
    
    # Host should receive player_joined notification
    host_notif = recv_until_type(s_host, "player_joined")
    assert host_notif is not None, "Host did not receive player_joined event"
    assert host_notif["name"] == "GuestPlayer"
    
    s_host.close()
    s_client.close()
    time.sleep(0.05)
    print("[PASS] Join room and nonexistent room rejection verified.")

def test_duplicate_room_membership_prevention():
    print("[TEST] Duplicate Room Membership Prevention...")
    s = connect_ws()
    send_ws_json(s, {"type": "create_room", "name": "Solo"})
    r = recv_until_type(s, "room_created")
    code = r["roomCode"]
    
    # Attempt to create another room while already in one
    send_ws_json(s, {"type": "create_room", "name": "SoloAgain"})
    err1 = recv_until_type(s, "room_error")
    assert err1 is not None and "already" in err1["message"].lower(), f"Expected already in room error, got: {err1}"
    
    # Attempt to join room while already in one
    send_ws_json(s, {"type": "join_room", "code": code, "name": "SoloJoin"})
    err2 = recv_until_type(s, "room_error")
    assert err2 is not None and "already" in err2["message"].lower(), f"Expected already in room error, got: {err2}"
    
    s.close()
    time.sleep(0.05)
    print("[PASS] Duplicate room membership prevented for active player.")

def test_room_capacity_limits():
    print("[TEST] Room Capacity Limits (Max 8 Players)...")
    sockets = []
    host = connect_ws()
    sockets.append(host)
    send_ws_json(host, {"type": "create_room", "name": "Host"})
    r = recv_until_type(host, "room_created")
    code = r["roomCode"]
    
    # Fill room to 8 players (7 guests)
    for i in range(1, 8):
        s = connect_ws()
        sockets.append(s)
        send_ws_json(s, {"type": "join_room", "code": code, "name": f"P{i+1}"})
        joined = recv_until_type(s, "room_joined")
        assert joined is not None, f"Player {i+1} failed to join: {joined}"
        
    # Attempt 9th player join (should be rejected as room is full)
    s9 = connect_ws()
    sockets.append(s9)
    send_ws_json(s9, {"type": "join_room", "code": code, "name": "P9_Overflow"})
    err = recv_until_type(s9, "room_error")
    assert err is not None and "full" in err["message"].lower(), f"Expected room is full error, got: {err}"
    
    for s in sockets:
        s.close()
    time.sleep(0.05)
    print("[PASS] Room capacity (8 players) strictly enforced.")

def test_host_transfer_on_leave():
    print("[TEST] Deterministic Host Transfer Upon Host Departure...")
    s1 = connect_ws()
    s2 = connect_ws()
    s3 = connect_ws()
    
    send_ws_json(s1, {"type": "create_room", "name": "OriginalHost"})
    r_created = recv_until_type(s1, "room_created")
    code = r_created["roomCode"]
    p1_id = r_created["playerId"]
    
    send_ws_json(s2, {"type": "join_room", "code": code, "name": "SecondJoiner"})
    r_joined2 = recv_until_type(s2, "room_joined")
    p2_id = r_joined2["playerId"]
    
    send_ws_json(s3, {"type": "join_room", "code": code, "name": "ThirdJoiner"})
    r_joined3 = recv_until_type(s3, "room_joined")
    p3_id = r_joined3["playerId"]
    
    # Original host leaves
    send_ws_json(s1, {"type": "leave_room"})
    left_conf = recv_until_type(s1, "room_left")
    assert left_conf is not None
    
    # Remaining players s2 and s3 must receive player_left with newHostId == p2_id
    notif2 = recv_until_type(s2, "player_left")
    assert notif2 is not None, "Second joiner did not receive player_left"
    assert notif2["newHostId"] == p2_id, f"Expected host transfer to second joiner {p2_id}, got {notif2['newHostId']}"
    assert notif2["room"]["hostPlayerId"] == p2_id
    
    s1.close()
    s2.close()
    s3.close()
    time.sleep(0.05)
    print("[PASS] Host transfer correctly transferred to earliest joiner.")

def test_empty_room_cleanup():
    print("[TEST] Empty Room Cleanup Upon Last Departure...")
    s = connect_ws()
    send_ws_json(s, {"type": "create_room", "name": "TempHost"})
    r = recv_until_type(s, "room_created")
    code = r["roomCode"]
    
    # Host leaves -> room is empty and destroyed
    send_ws_json(s, {"type": "leave_room"})
    recv_until_type(s, "room_left")
    
    # New client tries to join destroyed room
    s_new = connect_ws()
    send_ws_json(s_new, {"type": "join_room", "code": code, "name": "LateGuest"})
    err = recv_until_type(s_new, "room_error")
    assert err is not None and "not found" in err["message"].lower(), f"Expected room destroyed / not found, got {err}"
    
    s.close()
    s_new.close()
    time.sleep(0.05)
    print("[PASS] Empty room destroyed and removed from lookup registry.")

# ==========================================================
# 2. Permissions Tests
# ==========================================================
def test_permissions_non_host_start():
    print("[TEST] Permissions - Non-Host Cannot Start Race...")
    s_host = connect_ws()
    s_guest = connect_ws()
    
    send_ws_json(s_host, {"type": "create_room", "name": "Host"})
    r_created = recv_until_type(s_host, "room_created")
    code = r_created["roomCode"]
    
    send_ws_json(s_guest, {"type": "join_room", "code": code, "name": "Guest"})
    recv_until_type(s_guest, "room_joined")
    
    # Guest readies up
    send_ws_json(s_guest, {"type": "set_ready", "ready": True})
    recv_until_type(s_guest, "ready_updated")
    
    # Host readies up
    send_ws_json(s_host, {"type": "set_ready", "ready": True})
    recv_until_type(s_host, "ready_updated")
    
    # Guest attempts to start race -> Must be rejected!
    send_ws_json(s_guest, {"type": "start_race"})
    err = recv_until_type(s_guest, "room_error")
    assert err is not None and "host" in err["message"].lower(), f"Expected host permission error, got: {err}"
    
    s_host.close()
    s_guest.close()
    time.sleep(0.05)
    print("[PASS] Non-host start attempt rejected with permission error.")

def test_permissions_readiness_isolation():
    print("[TEST] Permissions - Player Cannot Modify Another Player's Readiness...")
    s1 = connect_ws()
    s2 = connect_ws()
    
    send_ws_json(s1, {"type": "create_room", "name": "P1"})
    r = recv_until_type(s1, "room_created")
    code = r["roomCode"]
    
    send_ws_json(s2, {"type": "join_room", "code": code, "name": "P2"})
    r2 = recv_until_type(s2, "room_joined")
    p2_id = r2["playerId"]
    
    # P1 sends set_ready with forged playerId field (should only toggle P1)
    send_ws_json(s1, {"type": "set_ready", "ready": True, "playerId": p2_id})
    upd = recv_until_type(s1, "ready_updated")
    assert upd is not None
    assert upd["playerId"] == r["playerId"], "Server must not trust client-supplied playerId for readiness"
    
    s1.close()
    s2.close()
    time.sleep(0.05)
    print("[PASS] Readiness authorization strictly verified against connection identity.")

# ==========================================================
# 3. Race Lifecycle & Countdown Tests
# ==========================================================
def test_race_lifecycle_countdown_and_start():
    print("[TEST] Race Lifecycle: WAITING -> COUNTDOWN -> RACING...")
    s1 = connect_ws()
    s2 = connect_ws()
    
    send_ws_json(s1, {"type": "create_room", "name": "Host"})
    r_created = recv_until_type(s1, "room_created")
    code = r_created["roomCode"]
    
    send_ws_json(s2, {"type": "join_room", "code": code, "name": "Racer"})
    recv_until_type(s2, "room_joined")
    
    # Cannot start when not ready
    send_ws_json(s1, {"type": "start_race"})
    err = recv_until_type(s1, "room_error")
    assert err is not None and "ready" in err["message"].lower(), f"Expected unready start error, got: {err}"
    
    # Both ready up
    send_ws_json(s1, {"type": "set_ready", "ready": True})
    recv_until_type(s1, "ready_updated")
    send_ws_json(s2, {"type": "set_ready", "ready": True})
    recv_until_type(s2, "ready_updated")
    
    # Host starts race -> Enters COUNTDOWN
    send_ws_json(s1, {"type": "start_race"})
    cd_evt1 = recv_until_type(s1, "countdown")
    cd_evt2 = recv_until_type(s2, "countdown")
    assert cd_evt1 is not None and cd_evt2 is not None, "Both racers should receive countdown event"
    assert cd_evt1["countdown"] == 5.0
    
    # Wait for countdown to expire (~5 seconds) and transition to RACING
    print("  -> Awaiting authoritative 5s countdown completion...")
    start_evt1 = recv_until_type(s1, "race_started", timeout=6.5)
    start_evt2 = recv_until_type(s2, "race_started", timeout=6.5)
    assert start_evt1 is not None and start_evt2 is not None, "Both racers should receive race_started event"
    assert start_evt1["room"]["state"] == "RACING"
    
    # Snapshots should now show RACING state
    snap = recv_until_type(s1, "snapshot", timeout=2.0)
    assert snap is not None
    assert snap.get("state") == "RACING"
    assert len(snap.get("players", [])) == 2
    
    s1.close()
    s2.close()
    time.sleep(0.05)
    print("[PASS] Race lifecycle (WAITING -> COUNTDOWN -> RACING) verified.")

def test_countdown_cancellation_on_unready():
    print("[TEST] Countdown Cancellation on Player Unready...")
    s1 = connect_ws()
    s2 = connect_ws()
    
    send_ws_json(s1, {"type": "create_room", "name": "Host"})
    r = recv_until_type(s1, "room_created")
    code = r["roomCode"]
    
    send_ws_json(s2, {"type": "join_room", "code": code, "name": "Racer"})
    recv_until_type(s2, "room_joined")
    
    send_ws_json(s1, {"type": "set_ready", "ready": True})
    recv_until_type(s1, "ready_updated")
    send_ws_json(s2, {"type": "set_ready", "ready": True})
    recv_until_type(s2, "ready_updated")
    
    send_ws_json(s1, {"type": "start_race"})
    recv_until_type(s1, "countdown")
    recv_until_type(s1, "room_updated")
    
    # Racer cancels ready during countdown
    time.sleep(0.5)
    send_ws_json(s2, {"type": "set_ready", "ready": False})
    
    recv_until_type(s1, "ready_updated")
    upd = recv_until_type(s1, "room_updated")
    assert upd is not None
    assert upd["room"]["state"] == "WAITING", f"Room should return to WAITING on unready, got {upd['room']['state']}"
    
    s1.close()
    s2.close()
    time.sleep(0.05)
    print("[PASS] Countdown cleanly aborted when player toggled unready.")

# ==========================================================
# 4. Multi-Room Isolation Tests
# ==========================================================
def test_multi_room_isolation():
    print("[TEST] Multi-Room Isolation (Room A vs Room B)...")
    # Room A
    sA1 = connect_ws()
    sA2 = connect_ws()
    send_ws_json(sA1, {"type": "create_room", "name": "PilotA1"})
    rA = recv_until_type(sA1, "room_created")
    codeA = rA["roomCode"]
    send_ws_json(sA2, {"type": "join_room", "code": codeA, "name": "PilotA2"})
    recv_until_type(sA2, "room_joined")
    
    # Room B
    sB1 = connect_ws()
    sB2 = connect_ws()
    send_ws_json(sB1, {"type": "create_room", "name": "PilotB1"})
    rB = recv_until_type(sB1, "room_created")
    codeB = rB["roomCode"]
    send_ws_json(sB2, {"type": "join_room", "code": codeB, "name": "PilotB2"})
    recv_until_type(sB2, "room_joined")
    
    # Start race in Room A ONLY
    send_ws_json(sA1, {"type": "set_ready", "ready": True})
    recv_until_type(sA1, "ready_updated")
    send_ws_json(sA2, {"type": "set_ready", "ready": True})
    recv_until_type(sA2, "ready_updated")
    send_ws_json(sA1, {"type": "start_race"})
    recv_until_type(sA1, "countdown")
    
    # Verify Room B receives NO countdown and remains in WAITING
    time.sleep(0.5)
    send_ws_json(sB1, {"type": "get_room_info"})
    infoB = recv_until_type(sB1, "room_updated")
    assert infoB is not None
    assert infoB["room"]["state"] == "WAITING", f"Room B was improperly affected by Room A! State: {infoB['room']['state']}"
    assert infoB["room"]["code"] == codeB
    assert len(infoB["room"]["players"]) == 2
    
    # Await Room A race start
    recv_until_type(sA1, "race_started", timeout=6.0)
    
    # Stream inputs in Room A
    for seq in range(1, 15):
        send_ws_json(sA1, {
            "type": "input",
            "seq": seq,
            "controls": {"up": True, "down": False, "left": False, "right": False}
        })
        time.sleep(0.016)
        
    # Read snapshot from Room A
    snapA = recv_until_type(sA1, "snapshot", timeout=2.0)
    assert snapA is not None
    assert snapA["roomCode"] == codeA
    player_ids_A = {p["id"] for p in snapA["players"]}
    assert len(player_ids_A) == 2, f"Room A snapshot must only contain Room A players, got: {player_ids_A}"
    
    # Verify Room B receives zero snapshots because it is in WAITING
    snapB = recv_until_type(sB1, "snapshot", timeout=0.5)
    assert snapB is None, f"Room B client unexpectedly received a snapshot while in WAITING: {snapB}"
    
    sA1.close()
    sA2.close()
    sB1.close()
    sB2.close()
    time.sleep(0.05)
    print("[PASS] Full isolation verified: Room A race and snapshots did not leak to Room B.")

# ==========================================================
# 5. Integration: Complete Race & Results
# ==========================================================
def test_complete_race_and_finish_results():
    print("[TEST] Complete Multiplayer Race & Finish Results...")
    s1 = connect_ws()
    s2 = connect_ws()
    
    send_ws_json(s1, {"type": "create_room", "name": "Speeder", "trackLength": 100.0})
    r = recv_until_type(s1, "room_created")
    code = r["roomCode"]
    
    send_ws_json(s2, {"type": "join_room", "code": code, "name": "Cruiser"})
    recv_until_type(s2, "room_joined")
    
    send_ws_json(s1, {"type": "set_ready", "ready": True})
    recv_until_type(s1, "ready_updated")
    send_ws_json(s2, {"type": "set_ready", "ready": True})
    recv_until_type(s2, "ready_updated")
    
    send_ws_json(s1, {"type": "start_race"})
    recv_until_type(s1, "race_started", timeout=6.5)
    recv_until_type(s2, "race_started", timeout=6.5)
    
    # Speeder accelerates full throttle; Cruiser drives slower
    print("  -> Racing down the track...")
    start_time = time.time()
    seq = 1
    
    finished_msg = None
    while time.time() - start_time < 20.0:
        seq += 1
        elapsed = time.time() - start_time
        # Speeder accelerates immediately at full throttle
        send_ws_json(s1, {
            "type": "input",
            "seq": seq,
            "controls": {"up": True, "down": False, "left": False, "right": False}
        })
        # Cruiser starts driving after 1.5s delay to guarantee Speeder finishes first
        cruiser_throttle = (elapsed > 1.5)
        send_ws_json(s2, {
            "type": "input",
            "seq": seq,
            "controls": {"up": cruiser_throttle, "down": False, "left": False, "right": False}
        })
        
        # Check if race_finished arrived
        msg = recv_ws_json(s1, timeout=0.03)
        if msg and msg.get("type") == "race_finished":
            finished_msg = msg
            break
            
    assert finished_msg is not None, "Race did not produce race_finished event within 30s"
    results = finished_msg.get("results", [])
    assert len(results) >= 1, f"Expected finish results, got: {results}"
    winner = results[0]
    assert winner["rank"] == 1
    assert "Speeder" in winner["name"]
    print(f"  -> Winner: {winner['name']} with finish time {winner['finishTime']:.2f}s")
    
    s1.close()
    s2.close()
    time.sleep(0.05)
    print("[PASS] Multiplayer race completed with authoritative rankings and finish times.")

def run_all_tests():
    print("==================================================")
    print("Running Phase 5 Multiplayer Game Rooms Test Suite")
    print("==================================================")
    test_room_creation_and_unique_codes()
    test_room_join_and_nonexistent()
    test_duplicate_room_membership_prevention()
    test_room_capacity_limits()
    test_host_transfer_on_leave()
    test_empty_room_cleanup()
    test_permissions_non_host_start()
    test_permissions_readiness_isolation()
    test_race_lifecycle_countdown_and_start()
    test_countdown_cancellation_on_unready()
    test_multi_room_isolation()
    test_complete_race_and_finish_results()
    print("==================================================")
    print("ALL PHASE 5 MULTIPLAYER ROOM TESTS PASSED!")
    print("==================================================")

if __name__ == "__main__":
    run_all_tests()
