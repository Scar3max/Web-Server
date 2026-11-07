#!/usr/bin/env python3
"""
Phase 3 Client & Static Asset Integration Tests
Validates:
1. Static asset serving for HTML, CSS, JS with correct MIME types.
2. Directory traversal prevention (safe path resolution).
3. 404 Not Found handling for non-existent assets.
4. Browser client HTML structure (canvas, HUD, ES module imports).
5. WebSocket client telemetry loop on /ws.
"""

import socket
import json
import base64
import os
import sys
import time

SERVER_HOST = "127.0.0.1"
SERVER_PORT = 8080

def send_http_request(method, path, headers=None):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5.0)
    s.connect((SERVER_HOST, SERVER_PORT))
    
    req = f"{method} {path} HTTP/1.1\r\nHost: {SERVER_HOST}:{SERVER_PORT}\r\n"
    if headers:
        for k, v in headers.items():
            req += f"{k}: {v}\r\n"
    req += "Connection: close\r\n\r\n"
    
    s.sendall(req.encode())
    
    response = b""
    while True:
        try:
            chunk = s.recv(4096)
            if not chunk:
                break
            response += chunk
        except Exception:
            break
    s.close()
    
    header_end = response.find(b"\r\n\r\n")
    if header_end == -1:
        return 0, {}, response
    
    header_part = response[:header_end].decode(errors="replace")
    body_part = response[header_end + 4:]
    
    lines = header_part.split("\r\n")
    status_line = lines[0]
    parts = status_line.split(" ")
    status_code = int(parts[1]) if len(parts) > 1 else 0
    
    resp_headers = {}
    for line in lines[1:]:
        if ":" in line:
            hk, hv = line.split(":", 1)
            resp_headers[hk.strip().lower()] = hv.strip()
            
    return status_code, resp_headers, body_part

def test_static_assets_and_mime():
    print("[TEST] Static Asset Serving & MIME Types...")
    assets = [
        ("/", 200, "text/html"),
        ("/about", 200, "text/html"),
        ("/css/game.css", 200, "text/css"),
        ("/js/main.js", 200, "application/javascript"),
        ("/js/game.js", 200, "application/javascript"),
        ("/js/renderer.js", 200, "application/javascript"),
        ("/js/input.js", 200, "application/javascript"),
        ("/js/network.js", 200, "application/javascript")
    ]
    
    for path, exp_status, exp_mime in assets:
        status, headers, body = send_http_request("GET", path)
        assert status == exp_status, f"Asset {path}: expected {exp_status}, got {status}"
        ctype = headers.get("content-type", "")
        assert exp_mime in ctype, f"Asset {path}: expected MIME '{exp_mime}', got '{ctype}'"
        assert len(body) > 0, f"Asset {path}: body was unexpectedly empty"
        
    print("[PASS] All static assets served with 200 OK and accurate Content-Type.")

def test_security_directory_traversal():
    print("[TEST] Security - Directory Traversal Prevention...")
    attack_paths = [
        "/../CMakeLists.txt",
        "/../../etc/passwd",
        "/....//CMakeLists.txt",
        "/./../../src/main.cpp",
        "/static/../../include/Router.h"
    ]
    
    for attack in attack_paths:
        status, headers, body = send_http_request("GET", attack)
        assert status == 404, f"Path traversal attempt '{attack}' was not rejected with 404 (got {status})"
        
    print("[PASS] Directory traversal attempts cleanly rejected with 404 Not Found.")

def test_404_not_found():
    print("[TEST] 404 Not Found Handling...")
    status, headers, body = send_http_request("GET", "/non_existent_asset.js")
    assert status == 404, f"Expected 404, got {status}"
    assert b"404 Not Found" in body, "Body missing 404 Not Found indication"
    print("[PASS] Non-existent resources return 404 Not Found.")

def test_browser_racing_client_structure():
    print("[TEST] Browser Racing Client HTML Document Structure...")
    status, headers, body = send_http_request("GET", "/")
    assert status == 200
    html = body.decode(errors="replace")
    
    required_elements = [
        'id="gameCanvas"',
        'id="speedValue"',
        'id="distanceValue"',
        'id="lapValue"',
        'id="statusPill"',
        'id="telemetryBox"',
        'href="/css/game.css"',
        'type="module"',
        'src="/js/main.js"'
    ]
    
    for el in required_elements:
        assert el in html, f"Missing required element/attribute in index.html: '{el}'"
        
    print("[PASS] Racing client HTML correctly contains HUD, Canvas, and ES module linkages.")

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
    if mask:
        header.extend(masking_key)
        masked = bytearray(plen)
        for i in range(plen):
            masked[i] = payload_bytes[i] ^ masking_key[i % 4]
        return bytes(header) + bytes(masked)
    else:
        return bytes(header) + payload_bytes

def parse_ws_frame(sock):
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

def test_websocket_telemetry_loop():
    print("[TEST] Client WebSocket Telemetry Loop on /ws...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5.0)
    s.connect((SERVER_HOST, SERVER_PORT))
    
    nonce = os.urandom(16)
    sec_key = base64.b64encode(nonce).decode()
    upgrade_req = (
        f"GET /ws HTTP/1.1\r\n"
        f"Host: {SERVER_HOST}:{SERVER_PORT}\r\n"
        f"Upgrade: websocket\r\n"
        f"Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {sec_key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n\r\n"
    )
    s.sendall(upgrade_req.encode())
    
    resp = b""
    while b"\r\n\r\n" not in resp:
        chunk = s.recv(1024)
        if not chunk:
            break
        resp += chunk
        
    assert b"101 Switching Protocols" in resp, "WebSocket handshake failed"
    
    # Send a series of telemetry frames like the browser racing client would
    for seq in range(1, 6):
        telemetry = {
            "type": "telemetry",
            "seq": seq,
            "timestamp": int(time.time() * 1000),
            "controls": {"up": True, "down": False, "left": False, "right": (seq % 2 == 0)},
            "player": {"x": 480.0 + seq * 2.5, "speed": 120.0 + seq * 5}
        }
        json_payload = json.dumps(telemetry).encode("utf-8")
        s.sendall(build_ws_frame(json_payload, opcode=0x1, fin=True, mask=True))
        
        fin, opcode, echo_payload = parse_ws_frame(s)
        assert fin is True, f"Frame {seq}: fin bit was false"
        assert opcode == 0x1, f"Frame {seq}: expected text opcode 1, got {opcode}"
        echo_json = json.loads(echo_payload.decode("utf-8"))
        assert echo_json["type"] == "telemetry", "Mismatch in echo payload"
        assert echo_json["seq"] == seq, f"Expected seq {seq}, got {echo_json['seq']}"
        assert abs(echo_json["player"]["speed"] - (120.0 + seq * 5)) < 1e-4
        
    # Send Close frame
    close_payload = (1000).to_bytes(2, "big") + b"Normal Closure"
    s.sendall(build_ws_frame(close_payload, opcode=0x8, fin=True, mask=True))
    fin, opcode, resp_close = parse_ws_frame(s)
    assert opcode == 0x8, f"Expected close opcode 8, got {opcode}"
    s.close()
    
    print("[PASS] Full WebSocket telemetry loop exchanged 5 state packets with clean closure.")

def run_all():
    test_static_assets_and_mime()
    test_security_directory_traversal()
    test_404_not_found()
    test_browser_racing_client_structure()
    test_websocket_telemetry_loop()
    print("\nALL PHASE 3 CLIENT INTEGRATION TESTS PASSED!")

if __name__ == "__main__":
    run_all()
