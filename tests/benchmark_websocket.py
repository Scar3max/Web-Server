"""
Phase 7 WebSocket Performance Benchmark.
Tests concurrency levels of 10, 50, and 100 simultaneous WebSocket connections.
Measures handshake latency, snapshot broadcast rates, input-to-ack round-trip latency,
and throughput (msg/s and KB/s).
"""

import socket
import struct
import json
import time
import threading
import sys
import statistics

HOST = "127.0.0.1"
PORT = 8080

def decode_binary_snapshot(buf):
    if len(buf) < 36:
        return None
    magic = struct.unpack("!I", buf[0:4])[0]
    if magic != 0x534E4150:
        return None
    version = buf[4]
    if version != 1:
        return None
    room_state_idx = buf[5]
    state_names = ["WAITING", "COUNTDOWN", "RACING", "FINISHED"]
    room_state = state_names[room_state_idx] if room_state_idx < len(state_names) else "UNKNOWN"
    countdown_tenths = struct.unpack("!H", buf[6:8])[0]
    countdown = countdown_tenths / 10.0
    room_id = struct.unpack("!I", buf[8:12])[0]
    room_code = buf[12:20].decode('utf-8', errors='ignore').rstrip('\x00')
    tick = struct.unpack("!I", buf[20:24])[0]
    timestamp = struct.unpack("!Q", buf[24:32])[0]
    player_count = struct.unpack("!H", buf[32:34])[0]
    result_count = struct.unpack("!H", buf[34:36])[0]

    offset = 36
    players = []
    for _ in range(player_count):
        if offset + 36 > len(buf):
            return None
        pid, x, dist, spd, steer, wx, wy, hdg, ack = struct.unpack("!IfffffffI", buf[offset:offset+36])
        offset += 36
        players.append({
            "id": pid,
            "x": x,
            "distance": dist,
            "speed": spd,
            "steer": steer,
            "worldX": wx,
            "worldY": wy,
            "heading": hdg,
            "ackSeq": ack
        })

    return {
        "type": "snapshot",
        "roomId": room_id,
        "roomCode": room_code,
        "state": room_state,
        "countdown": countdown,
        "tick": tick,
        "timestamp": timestamp,
        "players": players
    }

def ws_handshake(sock, path="/ws"):
    key = "dGhlIHNhbXBsZSBub25jZQ=="
    req = (
        f"GET {path} HTTP/1.1\r\n"
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

def send_ws_frame(sock, payload_str):
    data = payload_str.encode('utf-8')
    length = len(data)
    header = bytearray()
    header.append(0x81)
    mask_key = b"\xaa\xbb\xcc\xdd"
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

def percentile(data, p):
    if not data:
        return 0.0
    sorted_data = sorted(data)
    k = (len(sorted_data) - 1) * (p / 100.0)
    f = int(k)
    c = min(f + 1, len(sorted_data) - 1)
    d = k - f
    return sorted_data[f] * (1.0 - d) + sorted_data[c] * d

def run_ws_client(client_id, duration_sec, results, stop_event, use_binary=False):
    t_start_connect = time.perf_counter()
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(3.0)
    try:
        sock.connect((HOST, PORT))
        path = "/ws?format=binary" if use_binary else "/ws"
        if not ws_handshake(sock, path):
            return
        t_connected = time.perf_counter()
        handshake_ms = (t_connected - t_start_connect) * 1000.0
        results["handshake_latencies"].append(handshake_ms)

        # Send initial input to register in practice mode (rid == 0)
        init_cmd = json.dumps({
            "type": "input",
            "seq": 0,
            "controls": {"up": False, "down": False, "left": False, "right": False}
        })
        send_ws_frame(sock, init_cmd)

        player_id = None
        seq = 0
        input_sent_times = {}
        rtts = []
        snapshots_received = 0
        bytes_received = 0

        last_input_time = time.perf_counter()
        input_interval = 1.0 / 30.0 # 30 Hz inputs

        t_end = time.perf_counter() + duration_sec
        sock.settimeout(0.05)

        while time.perf_counter() < t_end and not stop_event.is_set():
            now = time.perf_counter()
            # Send input if interval elapsed
            if now - last_input_time >= input_interval:
                seq += 1
                input_cmd = json.dumps({
                    "type": "input",
                    "seq": seq,
                    "controls": {"up": True, "down": False, "left": False, "right": False}
                })
                input_sent_times[seq] = now
                send_ws_frame(sock, input_cmd)
                last_input_time = now

            # Attempt to read frames
            try:
                op, data = recv_ws_frame(sock)
                if data:
                    bytes_received += len(data)
                    msg = None
                    if op == 0x1:
                        try:
                            msg = json.loads(data.decode('utf-8'))
                        except Exception:
                            pass
                    elif op == 0x2:
                        msg = decode_binary_snapshot(data)

                    if msg:
                        mtype = msg.get("type", "")
                        if mtype == "welcome":
                            player_id = msg.get("playerId", 0)
                        elif mtype == "snapshot":
                            snapshots_received += 1
                            for sp in msg.get("players", []):
                                if player_id is not None and sp.get("id") == player_id:
                                    ack = sp.get("ackSeq", 0)
                                    if ack in input_sent_times:
                                        rtt_ms = (time.perf_counter() - input_sent_times[ack]) * 1000.0
                                        rtts.append(rtt_ms)
                                        # Prune acknowledged seqs
                                        keys_to_del = [k for k in input_sent_times.keys() if k <= ack]
                                        for k in keys_to_del:
                                            del input_sent_times[k]
            except (socket.timeout, BlockingIOError):
                pass
            except Exception:
                break

        results["rtts"].extend(rtts)
        results["snapshots"].append(snapshots_received)
        results["bytes"].append(bytes_received)
    except Exception as e:
        results["errors"].append(str(e))
    finally:
        try:
            sock.close()
        except:
            pass

def benchmark_concurrency(num_clients, duration_sec=5.0, use_binary=False):
    results = {
        "handshake_latencies": [],
        "rtts": [],
        "snapshots": [],
        "bytes": [],
        "errors": []
    }
    stop_event = threading.Event()
    threads = []
    
    t0 = time.perf_counter()
    for i in range(num_clients):
        t = threading.Thread(target=run_ws_client, args=(i, duration_sec, results, stop_event, use_binary))
        threads.append(t)
        t.start()
        time.sleep(0.01) # Stagger connection initiation slightly to avoid local ephemeral port exhaustion

    for t in threads:
        t.join()
    elapsed = time.perf_counter() - t0

    hs_data = results["handshake_latencies"]
    rtt_data = results["rtts"]
    total_snaps = sum(results["snapshots"])
    total_bytes = sum(results["bytes"])
    successful_clients = len(results["snapshots"])

    avg_snap_rate_per_client = (total_snaps / successful_clients / duration_sec) if successful_clients > 0 else 0.0
    throughput_kb_s = (total_bytes / 1024.0 / duration_sec) if duration_sec > 0 else 0.0

    return {
        "clients": num_clients,
        "format": "BINARY" if use_binary else "JSON",
        "successful_clients": successful_clients,
        "hs_p50": percentile(hs_data, 50),
        "hs_p95": percentile(hs_data, 95),
        "rtt_p50": percentile(rtt_data, 50),
        "rtt_p95": percentile(rtt_data, 95),
        "rtt_p99": percentile(rtt_data, 99),
        "snap_rate_client": avg_snap_rate_per_client,
        "throughput_kb_s": throughput_kb_s,
        "errors": len(results["errors"])
    }

def main():
    levels = [10, 50, 100]
    run_mode = "all"
    if "--json" in sys.argv:
        run_mode = "json"
    elif "--binary" in sys.argv:
        run_mode = "binary"

    modes_to_run = []
    if run_mode in ["all", "json"]:
        modes_to_run.append(False) # JSON
    if run_mode in ["all", "binary"]:
        modes_to_run.append(True)  # Binary

    for use_bin in modes_to_run:
        fmt_label = "BINARY PROTOCOL (OPTIMIZED)" if use_bin else "STANDARD JSON"
        target_url = f"ws://{HOST}:{PORT}/ws?format=binary" if use_bin else f"ws://{HOST}:{PORT}/ws"
        print("=========================================================================================")
        print(f"            PHASE 7.1 WEBSOCKET PERFORMANCE BENCHMARK [{fmt_label}]")
        print("=========================================================================================")
        print(f"Target: {target_url} | Duration per level: 5.0 seconds")
        print("-----------------------------------------------------------------------------------------")
        print(f"{'Clients':<8} | {'HS p50 (ms)':<12} | {'HS p95 (ms)':<12} | {'RTT p50(ms)':<12} | {'RTT p95(ms)':<12} | {'Snap/s/cli':<11} | {'KB/sec':<10}")
        print("-----------------------------------------------------------------------------------------")

        for l in levels:
            res = benchmark_concurrency(l, duration_sec=5.0, use_binary=use_bin)
            print(f"{res['clients']:<8} | {res['hs_p50']:<12.2f} | {res['hs_p95']:<12.2f} | {res['rtt_p50']:<12.2f} | {res['rtt_p95']:<12.2f} | {res['snap_rate_client']:<11.1f} | {res['throughput_kb_s']:<10.1f}")
            time.sleep(1.0)

        print("=========================================================================================\n")

if __name__ == "__main__":
    main()
