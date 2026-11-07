#!/usr/bin/env python3
"""
Phase 6.2 Client-Side Prediction & Server Reconciliation Test Suite

Verifies:
1. Authoritative snapshot carries ackSeq field for all players.
2. ackSeq reflects the latest input sequence actually applied by the 60 Hz simulation thread.
3. Rapid input burst handling and acknowledgement convergence.
4. Reconnection lifecycle: clean sequence reset and reconnection acceptance.
5. Multi-client ackSeq isolation: Player A inputs do not leak into Player B ackSeq.
6. Automated client prediction engine tests (node tests/test_prediction_reconciliation.mjs).
"""

import socket
import json
import base64
import os
import sys
import time
import shutil
import subprocess

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
        remaining = max(0.05, timeout - (time.time() - start))
        msg = recv_ws_json(s, timeout=remaining)
        if not msg:
            continue
        if msg.get("type") in target_types:
            return msg
    return None

def recv_next_snapshot(s, timeout=2.0):
    start = time.time()
    while time.time() - start < timeout:
        msg = recv_ws_json(s, timeout=max(0.05, timeout - (time.time() - start)))
        if msg and "tick" in msg and "players" in msg:
            return msg
    return None

def connect_and_admit():
    s = connect_ws()
    send_ws_json(s, {
        "type": "input",
        "seq": 1,
        "controls": {"up": False, "down": False, "left": False, "right": False}
    })
    msg = recv_until_type(s, "welcome", timeout=2.0)
    assert msg is not None and "playerId" in msg, f"Failed to receive welcome message: {msg}"
    pid = msg["playerId"]
    return s, pid

def get_player_from_snapshot(snap, pid):
    if not snap or "players" not in snap:
        return None
    for p in snap["players"]:
        if p["id"] == pid:
            return p
    return None

# ==========================================================
# 1. Authoritative Snapshot ackSeq Protocol Verification
# ==========================================================

def test_snapshot_ack_seq_protocol():
    print("[TEST] 1. Snapshot Protocol ackSeq Field Verification...")
    s, pid = connect_and_admit()

    # Receive practice snapshot
    snap = recv_next_snapshot(s, timeout=2.0)
    assert snap is not None, "Failed to receive initial snapshot"
    
    p0 = get_player_from_snapshot(snap, pid)
    assert p0 is not None, f"Player {pid} not found in snapshot: {snap}"
    assert "ackSeq" in p0, f"PlayerSnapshot missing ackSeq field: {p0}"
    assert isinstance(p0["ackSeq"], int), f"ackSeq must be an integer, got {type(p0['ackSeq'])}"
    assert p0["ackSeq"] >= 0, f"ackSeq must be non-negative: {p0['ackSeq']}"

    print(f"  -> Initial snapshot verified: playerId={p0['id']}, ackSeq={p0['ackSeq']}, tick={snap['tick']}")
    s.close()
    time.sleep(0.05)

# ==========================================================
# 2. Input Streaming and Authoritative Acknowledgement
# ==========================================================

def test_input_stream_acknowledgement():
    print("[TEST] 2. Input Streaming & Authoritative Acknowledgement Convergence...")
    s, pid = connect_and_admit()

    # Stream inputs with seq 2..16
    latest_sent_seq = 1
    acknowledged_seqs = []

    for seq in range(2, 17):
        send_ws_json(s, {
            "type": "input",
            "seq": seq,
            "controls": {"up": True, "down": False, "left": False, "right": False}
        })
        latest_sent_seq = seq
        time.sleep(0.016) # ~60 Hz input rate

        # Check for snapshot
        snap = recv_next_snapshot(s, timeout=0.01)
        if snap:
            p = get_player_from_snapshot(snap, pid)
            if p:
                acknowledged_seqs.append(p["ackSeq"])

    # Drain remaining snapshots to observe convergence
    start = time.time()
    final_ack = 0
    while time.time() - start < 1.0:
        snap = recv_next_snapshot(s, timeout=0.1)
        if snap:
            p = get_player_from_snapshot(snap, pid)
            if p:
                final_ack = p["ackSeq"]
                acknowledged_seqs.append(final_ack)
                if final_ack == latest_sent_seq:
                    break

    s.close()
    time.sleep(0.05)

    assert final_ack == latest_sent_seq, (
        f"Server failed to acknowledge latest sent seq: final_ack={final_ack}, expected={latest_sent_seq}"
    )

    # Verify monotonicity of received ackSeqs
    for i in range(1, len(acknowledged_seqs)):
        assert acknowledged_seqs[i] >= acknowledged_seqs[i - 1], (
            f"ackSeq decreased: {acknowledged_seqs[i]} < {acknowledged_seqs[i - 1]}"
        )

    print(f"  -> Successfully converged: latest_sent={latest_sent_seq}, final_ack={final_ack}, acks={acknowledged_seqs[:8]}...")

# ==========================================================
# 3. Burst Inputs & Coalescing Acknowledgement
# ==========================================================

def test_burst_input_coalescing():
    print("[TEST] 3. Burst Inputs & Coalescing Acknowledgement...")
    s, pid = connect_and_admit()

    # Send a rapid burst of inputs with increasing seq
    for seq in range(100, 106):
        send_ws_json(s, {
            "type": "input",
            "seq": seq,
            "controls": {"up": True, "down": False, "left": False, "right": False}
        })
        time.sleep(0.007) # Rapid ~140 Hz burst (above tick rate)

    # Wait for snapshot acknowledging burst
    start = time.time()
    ack = 0
    while time.time() - start < 1.0:
        snap = recv_next_snapshot(s, timeout=0.1)
        if snap:
            p = get_player_from_snapshot(snap, pid)
            if p and p["ackSeq"] >= 100:
                ack = p["ackSeq"]
                break

    # Server must acknowledge the latest coalesced input applied
    assert ack >= 100, f"Expected ackSeq >= 100 for burst, got {ack}"
    print(f"  -> Burst seq 100..105 acknowledged safely at ackSeq={ack} without queue overflow.")
    s.close()
    time.sleep(0.05)

# ==========================================================
# 4. Reconnection Lifecycle & Sequence Reset
# ==========================================================

def test_reconnection_lifecycle():
    print("[TEST] 4. Reconnection Lifecycle & Sequence Counter Reset...")
    # First connection: send inputs up to seq=9
    s1, pid1 = connect_and_admit()
    for seq in range(2, 10):
        send_ws_json(s1, {
            "type": "input",
            "seq": seq,
            "controls": {"up": True, "down": False, "left": False, "right": False}
        })
        time.sleep(0.01)

    snap1 = recv_next_snapshot(s1, timeout=0.5)
    s1.close()
    time.sleep(0.1)

    # Reconnect: client starts fresh from seq=1
    s2, pid2 = connect_and_admit()
    assert pid2 != pid1, f"Expected new playerId on reconnect, got {pid2} vs {pid1}"

    time.sleep(0.02)
    for seq in range(2, 6):
        send_ws_json(s2, {
            "type": "input",
            "seq": seq,
            "controls": {"up": True, "down": False, "left": False, "right": False}
        })
        time.sleep(0.016)

    start = time.time()
    ack2 = 0
    while time.time() - start < 1.0:
        snap2 = recv_next_snapshot(s2, timeout=0.1)
        if snap2:
            p2 = get_player_from_snapshot(snap2, pid2)
            if p2 and p2["ackSeq"] >= 2:
                ack2 = p2["ackSeq"]
                break
        time.sleep(0.016)

    assert ack2 >= 2, f"Reconnected client seq >= 2 should be acknowledged, got {ack2}"

    s2.close()
    time.sleep(0.05)
    print(f"  -> Reconnected client {pid2} successfully accepted and acknowledged starting from fresh seq.")

# ==========================================================
# 5. Multi-Client ackSeq Isolation
# ==========================================================

def test_multi_client_ack_seq_isolation():
    print("[TEST] 5. Multi-Client ackSeq Isolation (Player A vs Player B)...")
    s_a, pid_a = connect_and_admit()
    s_b, pid_b = connect_and_admit()
    assert pid_a != pid_b, "Player A and Player B must have distinct playerIds"

    # Player A streams more inputs (seq 2..15) while Player B sends no new inputs
    for seq in range(2, 16):
        send_ws_json(s_a, {
            "type": "input",
            "seq": seq,
            "controls": {"up": True, "down": False, "left": False, "right": False}
        })
        time.sleep(0.016)

    # Read latest snapshot from s_a
    start = time.time()
    p_a_ack = 0
    p_b_ack = 0
    while time.time() - start < 1.0:
        snap = recv_next_snapshot(s_a, timeout=0.1)
        if snap:
            p_a = get_player_from_snapshot(snap, pid_a)
            p_b = get_player_from_snapshot(snap, pid_b)
            if p_a:
                p_a_ack = p_a["ackSeq"]
            if p_b:
                p_b_ack = p_b["ackSeq"]
            if p_a_ack >= 10:
                break
        time.sleep(0.016)

    assert p_a_ack >= 10, f"Player A ackSeq should have advanced: {p_a_ack}"
    assert p_b_ack <= 1, f"Player B ackSeq leaked Player A inputs: {p_b_ack}"

    s_a.close()
    s_b.close()
    time.sleep(0.05)
    print(f"  -> Multi-client isolation verified: Player A ackSeq={p_a_ack}, Player B ackSeq={p_b_ack}.")

# ==========================================================
# 6. Automated Client Prediction Unit & Reconciliation Suite
# ==========================================================

def test_client_prediction_js_suite():
    print("[TEST] 6. Running Node.js Client Prediction & Reconciliation Test Harness...")
    node_bin = "node" if shutil.which("node") else "node.exe"
    res = subprocess.run([node_bin, "tests/test_prediction_reconciliation.mjs"], capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[FAIL] Node.js prediction tests failed:\n{res.stdout}\n{res.stderr}", file=sys.stderr)
        sys.exit(1)
    print(res.stdout.strip())
    print("[PASS] Automated client-side prediction and server reconciliation tests passed.")

# ==========================================================
# Main Runner
# ==========================================================

if __name__ == "__main__":
    print("==================================================")
    print("Phase 6.2 Client-Side Prediction & Reconciliation")
    print("==================================================")

    test_snapshot_ack_seq_protocol()
    test_input_stream_acknowledgement()
    test_burst_input_coalescing()
    test_reconnection_lifecycle()
    test_multi_client_ack_seq_isolation()
    test_client_prediction_js_suite()

    print("==================================================")
    print("ALL PHASE 6.2 TESTS PASSED SUCCESSFULLY!")
    print("==================================================")
