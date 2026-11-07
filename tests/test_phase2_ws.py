import socket
import struct
import time
import base64
import hashlib

HOST = "127.0.0.1"
PORT = 8080
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

def compute_accept(key):
    h = hashlib.sha1((key + WS_GUID).encode("utf-8")).digest()
    return base64.b64encode(h).decode("utf-8")

def make_frame(opcode, payload=b"", fin=True, mask=True, rsv1=False, rsv2=False, rsv3=False, masking_key=b"\x12\x34\x56\x78"):
    b0 = (0x80 if fin else 0) | (0x40 if rsv1 else 0) | (0x20 if rsv2 else 0) | (0x10 if rsv3 else 0) | (opcode & 0x0F)
    mask_bit = 0x80 if mask else 0
    length = len(payload)
    
    if length <= 125:
        header = bytes([b0, mask_bit | length])
    elif length <= 65535:
        header = bytes([b0, mask_bit | 126]) + struct.pack("!H", length)
    else:
        header = bytes([b0, mask_bit | 127]) + struct.pack("!Q", length)
        
    if mask:
        header += masking_key
        masked = bytes([b ^ masking_key[i % 4] for i, b in enumerate(payload)])
        return header + masked
    else:
        return header + payload

def read_frame(s):
    b0_b1 = s.recv(2)
    if len(b0_b1) < 2:
        return None
    b0, b1 = b0_b1[0], b0_b1[1]
    fin = (b0 & 0x80) != 0
    opcode = b0 & 0x0F
    masked = (b1 & 0x80) != 0
    len7 = b1 & 0x7F
    
    if len7 <= 125:
        payload_len = len7
    elif len7 == 126:
        ext = s.recv(2)
        payload_len = struct.unpack("!H", ext)[0]
    else:
        ext = s.recv(8)
        payload_len = struct.unpack("!Q", ext)[0]
        
    payload = b""
    while len(payload) < payload_len:
        chunk = s.recv(payload_len - len(payload))
        if not chunk:
            break
        payload += chunk
        
    return {"fin": fin, "opcode": opcode, "masked": masked, "payload": payload}

def test_handshake_valid():
    print("[TEST] WebSocket Handshake - Valid request...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, PORT))
    
    key = "dGhlIHNhbXBsZSBub25jZQ=="
    expected_accept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
    req = (
        f"GET /ws HTTP/1.1\r\n"
        f"Host: {HOST}:{PORT}\r\n"
        f"Upgrade: websocket\r\n"
        f"Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n\r\n"
    )
    s.sendall(req.encode())
    resp = s.recv(4096).decode("utf-8", errors="replace")
    
    assert "101 Switching Protocols" in resp, f"Expected 101, got: {resp}"
    assert "Upgrade: websocket" in resp or "upgrade: websocket" in resp.lower()
    assert f"Sec-WebSocket-Accept: {expected_accept}" in resp
    s.close()
    print("[PASS] Valid handshake accepted with RFC 6455 101 response.")

def test_handshake_missing_headers():
    print("[TEST] WebSocket Handshake - Missing headers / wrong method / wrong version...")
    # Missing Upgrade header
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, PORT))
    s.sendall(f"GET /ws HTTP/1.1\r\nHost: {HOST}\r\nSec-WebSocket-Version: 13\r\n\r\n".encode())
    resp = s.recv(4096).decode(errors="replace")
    s.close()
    assert "400 Bad Request" in resp or "404 Not Found" in resp

    # POST method instead of GET
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, PORT))
    s.sendall(f"POST /ws HTTP/1.1\r\nHost: {HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n".encode())
    resp = s.recv(4096).decode(errors="replace")
    s.close()
    assert "405 Method Not Allowed" in resp

    # Unsupported version (e.g. 12 instead of 13)
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, PORT))
    s.sendall(f"GET /ws HTTP/1.1\r\nHost: {HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 12\r\n\r\n".encode())
    resp = s.recv(4096).decode(errors="replace")
    s.close()
    assert "426 Upgrade Required" in resp
    assert "Sec-WebSocket-Version: 13" in resp
    print("[PASS] Malformed/unsupported handshakes rejected with correct HTTP status codes.")

def open_websocket():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, PORT))
    key = "dGhlIHNhbXBsZSBub25jZQ=="
    req = (
        f"GET /ws HTTP/1.1\r\n"
        f"Host: {HOST}:{PORT}\r\n"
        f"Upgrade: websocket\r\n"
        f"Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n\r\n"
    )
    s.sendall(req.encode())
    resp = s.recv(4096).decode(errors="replace")
    assert "101 Switching Protocols" in resp
    return s

def test_masked_text_frame_and_echo():
    print("[TEST] Frame Parsing - Masked text frame echo...")
    s = open_websocket()
    msg = "Hello, High Performance WebSocket Server!"
    frame = make_frame(opcode=1, payload=msg.encode("utf-8"))
    s.sendall(frame)
    
    res = read_frame(s)
    assert res is not None, "No response frame received"
    assert res["opcode"] == 1, f"Expected text opcode 1, got {res['opcode']}"
    assert res["masked"] is False, "Server-to-client frame MUST NOT be masked"
    assert res["payload"].decode("utf-8") == msg
    s.close()
    print("[PASS] Text frame received, unmasked, and echoed correctly.")

def test_multiple_messages_persistent_connection():
    print("[TEST] Connection Lifecycle - Multiple sequential messages...")
    s = open_websocket()
    for i in range(5):
        msg = f"Message batch index #{i}"
        s.sendall(make_frame(opcode=1, payload=msg.encode("utf-8")))
        res = read_frame(s)
        assert res is not None and res["payload"].decode("utf-8") == msg
    s.close()
    print("[PASS] Persistent connection handled multiple sequential frames cleanly.")

def test_empty_payload():
    print("[TEST] Frame Parsing - Empty payload text frame...")
    s = open_websocket()
    s.sendall(make_frame(opcode=1, payload=b""))
    res = read_frame(s)
    assert res is not None
    assert res["opcode"] == 1
    assert len(res["payload"]) == 0
    s.close()
    print("[PASS] Empty payload handled cleanly.")

def test_extended_payload():
    print("[TEST] Frame Parsing - Extended payload lengths (16-bit length > 125 bytes)...")
    s = open_websocket()
    large_text = "ABCDEFGHIJ" * 200 # 2000 bytes, triggers 16-bit extended length
    s.sendall(make_frame(opcode=1, payload=large_text.encode("utf-8")))
    res = read_frame(s)
    assert res is not None
    assert res["payload"].decode("utf-8") == large_text
    s.close()
    print("[PASS] 16-bit extended payload length parsed and echoed correctly.")

def test_fragmented_tcp_reads():
    print("[TEST] Frame Parsing - Single WebSocket frame fragmented across TCP reads...")
    s = open_websocket()
    msg = "This frame will be delivered in small 2-byte fragments over TCP."
    frame = make_frame(opcode=1, payload=msg.encode("utf-8"))
    
    for i in range(0, len(frame), 2):
        s.sendall(frame[i:i+2])
        time.sleep(0.005) # 5ms delay between fragments
        
    res = read_frame(s)
    assert res is not None
    assert res["payload"].decode("utf-8") == msg
    s.close()
    print("[PASS] Incrementally fragmented frame correctly assembled.")

def test_continuation_frames_fragmented_message():
    print("[TEST] Frame Parsing - Fragmented message via continuation frames...")
    s = open_websocket()
    part1 = "Part 1 of message; "
    part2 = "Part 2 of message; "
    part3 = "Part 3 final."
    
    # Frame 1: Text, FIN=0
    f1 = make_frame(opcode=1, payload=part1.encode("utf-8"), fin=False)
    # Frame 2: Continuation (opcode 0), FIN=0
    f2 = make_frame(opcode=0, payload=part2.encode("utf-8"), fin=False)
    # Frame 3: Continuation (opcode 0), FIN=1
    f3 = make_frame(opcode=0, payload=part3.encode("utf-8"), fin=True)
    
    s.sendall(f1)
    time.sleep(0.01)
    s.sendall(f2)
    time.sleep(0.01)
    s.sendall(f3)
    
    res = read_frame(s)
    assert res is not None
    assert res["payload"].decode("utf-8") == (part1 + part2 + part3)
    s.close()
    print("[PASS] Multi-frame continuation message assembled and validated.")

def test_ping_pong():
    print("[TEST] Control Frames - Ping and Pong...")
    s = open_websocket()
    ping_payload = b"HeartbeatData123"
    s.sendall(make_frame(opcode=9, payload=ping_payload)) # Opcode 9 = Ping
    
    res = read_frame(s)
    assert res is not None
    assert res["opcode"] == 10, f"Expected Pong (10), got {res['opcode']}" # Opcode 10 = Pong
    assert res["payload"] == ping_payload, "Pong payload must match Ping payload"
    s.close()
    print("[PASS] Ping answered with Pong containing identical payload.")

def test_close_frame():
    print("[TEST] Control Frames - Close frame handshake...")
    s = open_websocket()
    # Send close code 1000 Normal Closure
    close_payload = struct.pack("!H", 1000) + b"Normal close"
    s.sendall(make_frame(opcode=8, payload=close_payload))
    
    res = read_frame(s)
    assert res is not None
    assert res["opcode"] == 8, f"Expected Close opcode (8), got {res['opcode']}"
    code = struct.unpack("!H", res["payload"][:2])[0]
    assert code == 1000
    
    # After close handshake, socket should close
    time.sleep(0.05)
    extra = s.recv(1024)
    assert len(extra) == 0, "Socket should be closed by server after close handshake"
    s.close()
    print("[PASS] Clean close handshake completed.")

def test_unmasked_client_frame_rejection():
    print("[TEST] RFC 6455 Enforcement - Unmasked client frame rejection...")
    s = open_websocket()
    # Send frame with mask=False (violates RFC 6455 5.1)
    s.sendall(make_frame(opcode=1, payload=b"Should fail", mask=False))
    
    res = read_frame(s)
    assert res is not None and res["opcode"] == 8 # Server closes with protocol error
    code = struct.unpack("!H", res["payload"][:2])[0]
    assert code == 1002, f"Expected Protocol Error (1002), got {code}"
    s.close()
    print("[PASS] Unmasked frame rejected with close code 1002.")

def test_invalid_opcode_rejection():
    print("[TEST] RFC 6455 Enforcement - Invalid opcode rejection...")
    s = open_websocket()
    s.sendall(make_frame(opcode=3, payload=b"Reserved opcode")) # 3 is reserved
    res = read_frame(s)
    assert res is not None and res["opcode"] == 8
    code = struct.unpack("!H", res["payload"][:2])[0]
    assert code == 1002
    s.close()
    print("[PASS] Reserved opcode rejected with close code 1002.")

def test_invalid_utf8_rejection():
    print("[TEST] RFC 6455 Enforcement - Invalid UTF-8 text rejection...")
    s = open_websocket()
    # Invalid UTF-8 byte sequence \xFF\xFE
    s.sendall(make_frame(opcode=1, payload=b"Valid\xFF\xFEMalformed"))
    res = read_frame(s)
    assert res is not None and res["opcode"] == 8
    code = struct.unpack("!H", res["payload"][:2])[0]
    assert code == 1007, f"Expected Invalid Payload Data (1007), got {code}"
    s.close()
    print("[PASS] Invalid UTF-8 text rejected with close code 1007.")

def run_all_ws_tests():
    test_handshake_valid()
    test_handshake_missing_headers()
    test_masked_text_frame_and_echo()
    test_multiple_messages_persistent_connection()
    test_empty_payload()
    test_extended_payload()
    test_fragmented_tcp_reads()
    test_continuation_frames_fragmented_message()
    test_ping_pong()
    test_close_frame()
    test_unmasked_client_frame_rejection()
    test_invalid_opcode_rejection()
    test_invalid_utf8_rejection()
    print("\nALL PHASE 2 WEBSOCKET TESTS PASSED!")

if __name__ == "__main__":
    run_all_ws_tests()
