"""
Phase 7 Server Observability & Metrics Test Suite.
Validates Prometheus text format, JSON backward compatibility,
subsystem instrumentation, and concurrent scraping resilience.
"""

import sys
import time
import socket
import json
import urllib.request
import threading
import struct

HOST = "127.0.0.1"
PORT = 8080

def get_metrics_prometheus():
    req = urllib.request.Request(
        f"http://{HOST}:{PORT}/metrics?format=prometheus",
        headers={"Accept": "text/plain", "User-Agent": "curl/8.0.0"}
    )
    with urllib.request.urlopen(req, timeout=5) as resp:
        assert resp.status == 200
        content_type = resp.headers.get("Content-Type", "")
        assert "text/plain" in content_type
        return resp.read().decode("utf-8")

def get_metrics_json():
    req = urllib.request.Request(
        f"http://{HOST}:{PORT}/metrics?format=json",
        headers={"Accept": "application/json"}
    )
    with urllib.request.urlopen(req, timeout=5) as resp:
        assert resp.status == 200
        content_type = resp.headers.get("Content-Type", "")
        assert "application/json" in content_type
        return json.loads(resp.read().decode("utf-8"))

def test_prometheus_exposition_format():
    print("Testing Prometheus text exposition format...")
    text = get_metrics_prometheus()
    lines = [line.strip() for line in text.strip().split("\n") if line.strip()]
    
    # Must have # HELP and # TYPE headers
    help_lines = [l for l in lines if l.startswith("# HELP ")]
    type_lines = [l for l in lines if l.startswith("# TYPE ")]
    metric_lines = [l for l in lines if not l.startswith("#")]
    
    assert len(help_lines) >= 10, f"Expected >= 10 HELP headers, got {len(help_lines)}"
    assert len(type_lines) >= 10, f"Expected >= 10 TYPE headers, got {len(type_lines)}"
    assert len(metric_lines) >= 10, f"Expected >= 10 metric data lines, got {len(metric_lines)}"
    
    # Check key Prometheus metrics
    required_metrics = [
        "http_requests_total",
        "tcp_connections_active",
        "tcp_connections_accepted_total",
        "tcp_connections_closed_total",
        "websocket_connections_active",
        "websocket_frames_received_total",
        "websocket_frames_sent_total",
        "websocket_protocol_errors_total",
        "network_bytes_received_total",
        "multiplayer_rooms_active",
        "multiplayer_players_active",
        "multiplayer_collision_events_total",
        "simulation_tick_frequency_hz",
        "simulation_tick_duration_seconds",
        "simulation_tick_deadline_misses_total",
        "simulation_snapshot_frequency_hz",
        "simulation_snapshot_queue_depth",
        "simulation_input_queue_depth",
        "backpressure_output_buffer_bytes_bucket",
        "backpressure_slow_client_disconnects_total",
        "backpressure_snapshot_drops_total",
    ]
    
    found_metrics = set()
    for l in metric_lines:
        metric_name = l.split("{")[0].split()[0]
        found_metrics.add(metric_name)
        
    for rm in required_metrics:
        assert rm in found_metrics, f"Missing required Prometheus metric: {rm}"
        
    # Check TYPE declarations valid (counter or gauge or histogram)
    valid_types = {"counter", "gauge", "histogram"}
    for tl in type_lines:
        parts = tl.split()
        assert len(parts) == 4, f"Invalid TYPE line format: {tl}"
        assert parts[3] in valid_types, f"Invalid metric type '{parts[3]}' in line: {tl}"

    print("  ✓ Prometheus exposition format syntax valid and complete")

def test_json_backward_compatibility():
    print("Testing JSON metrics format backward compatibility...")
    data = get_metrics_json()
    
    # Legacy Phase 4/5 keys must exist and be numeric
    legacy_keys = [
        "tick", "tickRate", "snapshotRate", "activePlayers", "roomCount",
        "totalSnapshotsPublished", "inputQueueLifecycleSize", "inputQueueHighWaterEvents",
        "inputQueueHighWaterInputs", "totalJoinsAttempted", "totalJoinsAdmitted",
        "totalJoinsRejected", "totalLeavesPushed", "snapshotQueueHighWater",
        "snapshotQueueTotalDropped"
    ]
    for key in legacy_keys:
        assert key in data, f"Missing legacy metric key '{key}' in JSON response"
        assert isinstance(data[key], (int, float)), f"Legacy key '{key}' must be numeric"
        
    # Phase 7 structured keys
    assert "simulation" in data
    assert "network" in data
    assert "multiplayer" in data
    assert "backpressure" in data
    assert "httpRequests" in data
    
    sim = data["simulation"]
    assert "tickDurationAvgSeconds" in sim
    assert "tickDeadlineMisses" in sim
    
    print("  ✓ JSON backward compatibility and Phase 7 extensions verified")

def test_http_subsystem_instrumentation():
    print("Testing HTTP request and response code instrumentation...")
    # Record baseline
    m_before = get_metrics_json()
    http_200_before = m_before.get("httpRequests", {}).get("200", 0)
    http_404_before = m_before.get("httpRequests", {}).get("404", 0)
    
    # Request valid static file (200)
    req200 = urllib.request.Request(f"http://{HOST}:{PORT}/")
    with urllib.request.urlopen(req200) as resp:
        assert resp.status == 200
        
    # Request non-existent path (404)
    req404 = urllib.request.Request(f"http://{HOST}:{PORT}/non_existent_path_test_12345")
    try:
        urllib.request.urlopen(req404)
    except urllib.error.HTTPError as e:
        assert e.code == 404
        
    # Send malformed HTTP request with invalid Content-Length (400)
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, PORT))
    s.sendall(b"GET / HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nContent-Length: abc\r\n\r\n")
    s.recv(1024)
    s.close()
    
    time.sleep(0.1)
    m_after = get_metrics_json()
    http_200_after = m_after.get("httpRequests", {}).get("200", 0)
    http_404_after = m_after.get("httpRequests", {}).get("404", 0)
    http_400_after = m_after.get("httpRequests", {}).get("400", 0)
    
    assert http_200_after > http_200_before, "HTTP 200 counter did not increment"
    assert http_404_after > http_404_before, "HTTP 404 counter did not increment"
    assert http_400_after >= 1, "HTTP 400 counter did not record malformed request"
    print("  ✓ HTTP 200, 404, and 400 status codes correctly instrumented")

def make_ws_handshake(sock):
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
    assert b"101 Switching Protocols" in resp

def send_ws_text(sock, text):
    data = text.encode('utf-8')
    length = len(data)
    header = bytearray()
    header.append(0x81) # FIN + text opcode
    mask_key = b"\x12\x34\x56\x78"
    if length <= 125:
        header.append(0x80 | length)
    elif length <= 65535:
        header.append(0x80 | 126)
        header.extend(struct.pack("!H", length))
    else:
        header.append(0x80 | 127)
        header.extend(struct.pack("!Q", length))
    header.extend(mask_key)
    masked_data = bytearray(length)
    for i in range(length):
        masked_data[i] = data[i] ^ mask_key[i % 4]
    sock.sendall(header + masked_data)

def recv_ws_frame(sock):
    try:
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
    except Exception:
        return None, None

def test_websocket_and_room_lifecycle_metrics():
    print("Testing WebSocket and room lifecycle metrics...")
    m_start = get_metrics_json()
    ws_active_start = m_start["network"]["wsActive"]
    rooms_active_start = m_start["multiplayer"]["roomsActive"]
    players_active_start = m_start["multiplayer"]["playersActive"]
    
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(3.0)
    sock.connect((HOST, PORT))
    make_ws_handshake(sock)
    
    time.sleep(0.1)
    m_ws = get_metrics_json()
    assert m_ws["network"]["wsActive"] == ws_active_start + 1
    
    # Create room (triggers getOrAdmitPlayer -> welcome + room_created)
    create_req = json.dumps({"type": "create_room", "name": "MetricsTestPlayer"})
    send_ws_text(sock, create_req)
    
    room_resp = None
    for _ in range(20):
        op, payload = recv_ws_frame(sock)
        if payload:
            try:
                msg = json.loads(payload.decode('utf-8'))
                if msg.get("type") in ("room_created", "room_joined"):
                    room_resp = msg
                    break
            except Exception:
                pass
    assert room_resp is not None, "Did not receive room_created response"
    
    time.sleep(0.1)
    m_room = get_metrics_json()
    assert m_room["multiplayer"]["roomsActive"] == rooms_active_start + 1
    assert m_room["multiplayer"]["playersActive"] == players_active_start + 1
    
    # Disconnect
    sock.close()
    time.sleep(0.2)
    
    m_end = get_metrics_json()
    assert m_end["network"]["wsActive"] == ws_active_start
    assert m_end["multiplayer"]["roomsActive"] == rooms_active_start
    assert m_end["multiplayer"]["playersActive"] == players_active_start
    print("  ✓ WebSocket and Room create/join/leave lifecycle accurately reflected in gauges")

def test_simulation_timing_and_rates():
    print("Testing simulation loop timing metrics...")
    data = get_metrics_json()
    
    tick_rate = data["tickRate"]
    snap_rate = data["snapshotRate"]
    avg_tick_us = data["simulation"]["tickDurationAvgSeconds"] * 1000000.0
    missed_ticks = data["simulation"]["tickDeadlineMisses"]
    
    print(f"  Simulation measured tick rate: {tick_rate:.2f} Hz (nominal: 60.0 Hz)")
    print(f"  Simulation measured snapshot rate: {snap_rate:.2f} Hz (nominal: 20.0 Hz)")
    print(f"  Simulation avg tick duration: {avg_tick_us:.2f} µs (budget: 16,666 µs)")
    
    assert tick_rate >= 45.0, f"Tick rate too low: {tick_rate} Hz"
    assert tick_rate <= 75.0, f"Tick rate out of bounds: {tick_rate} Hz"
    assert snap_rate >= 15.0, f"Snapshot rate too low: {snap_rate} Hz"
    assert snap_rate <= 28.0, f"Snapshot rate out of bounds: {snap_rate} Hz"
    assert avg_tick_us < 16666.0, f"Simulation tick exceeds 60 Hz budget: {avg_tick_us} µs"
    print("  ✓ Simulation tick rate, snapshot rate, and execution duration within design limits")

def test_concurrent_scraping_resilience():
    print("Testing concurrent /metrics scraping resilience (20 threads x 10 requests)...")
    errors = []
    
    def worker():
        for _ in range(10):
            try:
                # Alternate between prometheus and json formats
                p_text = get_metrics_prometheus()
                assert len(p_text) > 100
                j_data = get_metrics_json()
                assert "simulation" in j_data
            except Exception as e:
                errors.append(str(e))
                
    threads = [threading.Thread(target=worker) for _ in range(20)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
        
    assert len(errors) == 0, f"Errors encountered during concurrent scraping: {errors[:5]}"
    print("  ✓ Zero errors under 200 concurrent metric scrapes across dual formats")

def run_all_tests():
    print("=== PHASE 7 METRICS & OBSERVABILITY TEST SUITE ===")
    test_prometheus_exposition_format()
    test_json_backward_compatibility()
    test_http_subsystem_instrumentation()
    test_websocket_and_room_lifecycle_metrics()
    test_simulation_timing_and_rates()
    test_concurrent_scraping_resilience()
    print("==================================================")
    print("ALL PHASE 7 METRICS TESTS PASSED!")

if __name__ == "__main__":
    run_all_tests()
