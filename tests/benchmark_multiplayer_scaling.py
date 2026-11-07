"""
Phase 7 Multiplayer Scaling Benchmark Suite.
Evaluates simulation loop, collision solver, and network backpressure
under 4 representative multi-room scaling scenarios:
  1. Scenario A: 1 Room x 2 Players (Baseline)
  2. Scenario B: 1 Room x 8 Players (Dense single room)
  3. Scenario C: 4 Rooms x 8 Players (32 total players)
  4. Scenario D: 8 Rooms x 8 Players (64 total players)
"""

import socket
import struct
import json
import time
import threading
import urllib.request
import sys

HOST = "127.0.0.1"
PORT = 8080

def get_server_metrics():
    try:
        req = urllib.request.Request(
            f"http://{HOST}:{PORT}/metrics?format=json",
            headers={"Accept": "application/json"}
        )
        with urllib.request.urlopen(req, timeout=3) as resp:
            return json.loads(resp.read().decode('utf-8'))
    except Exception as e:
        return {}

def ws_handshake(sock):
    key = "dGhlIHNhbXBsZSBub25jZQ=="
    req = (
        f"GET /ws HTTP/1.1\r\n"
        f"Host: {HOST}:{PORT}\r\n"
        f"Upgrade: websocket\r\n"
        f"Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n\r\n"
    )
    sock.sendall(req.encode('utf-8'))
    resp = b""
    while b"\r\n\r\n" not in resp:
        chunk = sock.recv(1024)
        if not chunk:
            break
        resp += chunk
    return b"101 Switching Protocols" in resp

def send_ws_frame(sock, text):
    data = text.encode('utf-8')
    length = len(data)
    header = bytearray([0x81])
    mask_key = b"\x13\x37\x42\x24"
    if length <= 125:
        header.append(0x80 | length)
    elif length <= 65535:
        header.append(0x80 | 126)
        header.extend(struct.pack("!H", length))
    else:
        header.append(0x80 | 127)
        header.extend(struct.pack("!Q", length))
    header.extend(mask_key)
    masked = bytearray(length)
    for i in range(length):
        masked[i] = data[i] ^ mask_key[i % 4]
    sock.sendall(header + masked)

def recv_ws_frame(sock):
    hdr = sock.recv(2)
    if len(hdr) < 2:
        return None, None
    b1, b2 = hdr[0], hdr[1]
    opcode = b1 & 0x0F
    is_masked = (b2 & 0x80) != 0
    payload_len = b2 & 0x7F
    if payload_len == 126:
        payload_len = struct.unpack("!H", sock.recv(2))[0]
    elif payload_len == 127:
        payload_len = struct.unpack("!Q", sock.recv(8))[0]
    mask = sock.recv(4) if is_masked else None
    payload = bytearray()
    while len(payload) < payload_len:
        chunk = sock.recv(payload_len - len(payload))
        if not chunk:
            break
        payload.extend(chunk)
    if is_masked:
        for i in range(len(payload)):
            payload[i] ^= mask[i % 4]
    return opcode, payload

def simulated_player(room_code, player_num, is_creator, created_code_container, duration_sec, stop_event, stats):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5.0)
    try:
        sock.connect((HOST, PORT))
        if not ws_handshake(sock):
            return

        sock.settimeout(2.0)
        actual_code = room_code
        if is_creator:
            cmd = json.dumps({"type": "create_room", "name": f"P_{player_num}"})
            send_ws_frame(sock, cmd)
            # Receive room_created
            for _ in range(10):
                op, data = recv_ws_frame(sock)
                if data:
                    try:
                        msg = json.loads(data.decode('utf-8'))
                        if msg.get("type") == "room_created":
                            actual_code = msg.get("roomCode", room_code)
                            created_code_container.append(actual_code)
                            break
                    except Exception:
                        pass
        else:
            # Wait briefly for creator to establish roomCode
            t_wait = time.perf_counter() + 2.0
            while not created_code_container and time.perf_counter() < t_wait:
                time.sleep(0.02)
            actual_code = created_code_container[0] if created_code_container else room_code
            cmd = json.dumps({"type": "join_room", "code": actual_code, "name": f"P_{player_num}"})
            send_ws_frame(sock, cmd)

        # Mark ready
        time.sleep(0.05)
        send_ws_frame(sock, json.dumps({"type": "set_ready", "ready": True}))

        # If host, trigger start_race after players join
        if is_creator:
            time.sleep(0.2)
            send_ws_frame(sock, json.dumps({"type": "start_race"}))

        sock.settimeout(0.05)
        seq = 0
        snaps = 0
        t_end = time.perf_counter() + duration_sec
        last_input = time.perf_counter()

        while time.perf_counter() < t_end and not stop_event.is_set():
            now = time.perf_counter()
            # 30 Hz input sending
            if now - last_input >= 0.033:
                seq += 1
                cmd = json.dumps({
                    "type": "input",
                    "seq": seq,
                    "controls": {
                        "up": True,
                        "down": False,
                        "left": (player_num % 2 == 0),
                        "right": (player_num % 2 != 0)
                    }
                })
                send_ws_frame(sock, cmd)
                last_input = now

            try:
                op, data = recv_ws_frame(sock)
                if data:
                    msg = json.loads(data.decode('utf-8'))
                    if msg.get("type") == "snapshot":
                        snaps += 1
            except (socket.timeout, BlockingIOError):
                pass
            except Exception:
                break

        stats["snapshots"] += snaps
        stats["players_completed"] += 1
    except Exception as e:
        stats["errors"] += 1
    finally:
        try:
            sock.close()
        except:
            pass

def run_scenario(name, num_rooms, players_per_room, duration_sec=6.0):
    total_players = num_rooms * players_per_room
    print(f"\nRunning {name}: {num_rooms} Rooms x {players_per_room} Players ({total_players} total clients)...")

    # Metrics before test
    m_before = get_server_metrics()

    stats = {"snapshots": 0, "players_completed": 0, "errors": 0}
    stop_event = threading.Event()
    threads = []

    t_start = time.perf_counter()

    for r in range(num_rooms):
        room_container = []
        # First player in room creates the room
        for p in range(players_per_room):
            is_creator = (p == 0)
            t = threading.Thread(
                target=simulated_player,
                args=(f"R_{r}", p, is_creator, room_container, duration_sec, stop_event, stats)
            )
            threads.append(t)
            t.start()
            time.sleep(0.02) # Stagger client joins

    # Wait for test duration
    time.sleep(duration_sec)
    stop_event.set()

    for t in threads:
        t.join(timeout=2.0)

    elapsed = time.perf_counter() - t_start

    # Metrics after test
    time.sleep(0.5)
    m_after = get_server_metrics()

    sim_m = m_after.get("simulation", {})
    mp_m = m_after.get("multiplayer", {})

    tick_rate = m_after.get("tickRate", 60.0)
    snap_rate = m_after.get("snapshotRate", 20.0)
    tick_dur_us = sim_m.get("tickDurationAvgSeconds", 0.0) * 1000000.0
    col_dur_us = mp_m.get("collisionSolverDurationAvgSeconds", 0.0) * 1000000.0
    col_count = mp_m.get("collisionEventsTotal", 0)

    avg_client_snap_rate = (stats["snapshots"] / total_players / duration_sec) if total_players > 0 else 0.0

    result = {
        "scenario": name,
        "rooms": num_rooms,
        "players_per_room": players_per_room,
        "total_players": total_players,
        "tick_rate_hz": tick_rate,
        "snap_rate_hz": snap_rate,
        "avg_tick_us": tick_dur_us,
        "col_dur_us": col_dur_us,
        "client_snap_rate": avg_client_snap_rate,
        "collisions_total": col_count,
        "errors": stats["errors"]
    }
    return result

def main():
    print("=========================================================================================================")
    print("                              PHASE 7 MULTIPLAYER SCALING BENCHMARK                                      ")
    print("=========================================================================================================")

    scenarios = [
        ("Scenario A (1x2)", 1, 2),
        ("Scenario B (1x8)", 1, 8),
        ("Scenario C (4x8)", 4, 8),
        ("Scenario D (8x8)", 8, 8),
    ]

    results = []
    for name, rooms, ppr in scenarios:
        res = run_scenario(name, rooms, ppr, duration_sec=6.0)
        results.append(res)
        time.sleep(1.0)

    print("\n")
    print("=========================================================================================================")
    print("                                      SCALING BENCHMARK SUMMARY TABLE                                    ")
    print("=========================================================================================================")
    print(f"{'Scenario':<18} | {'Rooms':<6} | {'Players':<8} | {'Tick Rate':<10} | {'Tick Time(µs)':<14} | {'Col Time(µs)':<13} | {'Snap/s/cli':<11} | {'Errors':<7}")
    print("---------------------------------------------------------------------------------------------------------")
    for r in results:
        print(f"{r['scenario']:<18} | {r['rooms']:<6} | {r['total_players']:<8} | {r['tick_rate_hz']:<10.1f} | {r['avg_tick_us']:<14.2f} | {r['col_dur_us']:<13.2f} | {r['client_snap_rate']:<11.1f} | {r['errors']:<7}")
    print("=========================================================================================================")
    return results

if __name__ == "__main__":
    main()
