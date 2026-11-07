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

def make_frame(opcode, payload=b"", fin=True, mask=True, rsv1=False, rsv2=False, rsv3=False, 
               masking_key=b"\x12\x34\x56\x78", force_len_encoding=None, msb_set=False):
    """
    Constructs a WebSocket frame.
    force_len_encoding: 
       None: auto (7-bit, 16-bit, 64-bit minimal)
       16: force 16-bit extended length (even if len <= 125, testing non-minimal)
       64: force 64-bit extended length (even if len < 65536, testing non-minimal)
    msb_set: set bit 63 in 64-bit length
    """
    b0 = (0x80 if fin else 0) | (0x40 if rsv1 else 0) | (0x20 if rsv2 else 0) | (0x10 if rsv3 else 0) | (opcode & 0x0F)
    mask_bit = 0x80 if mask else 0
    length = len(payload)
    
    if force_len_encoding == 16:
        header = bytes([b0, mask_bit | 126]) + struct.pack("!H", length)
    elif force_len_encoding == 64:
        val = (1 << 63) | length if msb_set else length
        header = bytes([b0, mask_bit | 127]) + struct.pack("!Q", val)
    else:
        if length <= 125:
            header = bytes([b0, mask_bit | length])
        elif length <= 65535:
            header = bytes([b0, mask_bit | 126]) + struct.pack("!H", length)
        else:
            val = (1 << 63) | length if msb_set else length
            header = bytes([b0, mask_bit | 127]) + struct.pack("!Q", val)
        
    if mask:
        header += masking_key
        masked = bytes([b ^ masking_key[i % 4] for i, b in enumerate(payload)])
        return header + masked
    else:
        return header + payload

def read_frame(s, timeout=2.0):
    s.settimeout(timeout)
    try:
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
    except socket.timeout:
        return None

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
    resp = s.recv(4096).decode("utf-8", errors="replace")
    assert "101 Switching Protocols" in resp
    return s

# =========================================================================
# 1. HANDSHAKE TESTS
# =========================================================================

def test_handshake_valid():
    print("[TEST] Handshake - Valid RFC 6455 test vector...")
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
    assert "101 Switching Protocols" in resp
    assert f"Sec-WebSocket-Accept: {expected_accept}" in resp
    s.close()
    print("[PASS] Handshake valid RFC vector accepted.")

def test_handshake_duplicate_headers():
    print("[TEST] Handshake - Multiple duplicate Connection headers...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, PORT))
    key = "dGhlIHNhbXBsZSBub25jZQ=="
    req = (
        f"GET /ws HTTP/1.1\r\n"
        f"Host: {HOST}:{PORT}\r\n"
        f"Upgrade: websocket\r\n"
        f"Connection: keep-alive\r\n"
        f"Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n\r\n"
    )
    s.sendall(req.encode())
    resp = s.recv(4096).decode("utf-8", errors="replace")
    assert "101 Switching Protocols" in resp
    s.close()
    print("[PASS] Duplicate headers merged and accepted per RFC 7230.")

def test_handshake_invalid_keys():
    print("[TEST] Handshake - Invalid Sec-WebSocket-Key formats...")
    bad_keys = [
        "short_key==",                  # Wrong length (< 24)
        "dGhlIHNhbXBsZSBub25jZQ===extra", # Too long (> 24)
        "dGhlIHNhbXBsZSBub25jZQ!=",     # Invalid base64 char
        "dGhlIHNhbXBsZSBub25jZ1==",     # Invalid padding bits (index 21 is '1', lower 4 bits != 0)
        ""                              # Empty
    ]
    for bkey in bad_keys:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((HOST, PORT))
        req = (
            f"GET /ws HTTP/1.1\r\n"
            f"Host: {HOST}:{PORT}\r\n"
            f"Upgrade: websocket\r\n"
            f"Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {bkey}\r\n"
            f"Sec-WebSocket-Version: 13\r\n\r\n"
        )
        s.sendall(req.encode())
        resp = s.recv(4096).decode("utf-8", errors="replace")
        s.close()
        assert "400 Bad Request" in resp, f"Key '{bkey}' was not rejected! Got: {resp[:100]}"
    print("[PASS] All malformed and invalid-nonce keys rejected with 400 Bad Request.")

def test_handshake_wrong_http_version():
    print("[TEST] Handshake - HTTP version != HTTP/1.1...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, PORT))
    req = (
        f"GET /ws HTTP/1.0\r\n"
        f"Host: {HOST}:{PORT}\r\n"
        f"Upgrade: websocket\r\n"
        f"Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        f"Sec-WebSocket-Version: 13\r\n\r\n"
    )
    s.sendall(req.encode())
    resp = s.recv(4096).decode("utf-8", errors="replace")
    s.close()
    assert "400 Bad Request" in resp
    print("[PASS] Non-HTTP/1.1 handshake rejected.")

# =========================================================================
# 2. FRAME ENCODING & LENGTH BOUNDARY TESTS
# =========================================================================

def test_non_minimal_length_16():
    print("[TEST] Frame Parsing - Non-minimal 16-bit length rejection...")
    s = open_websocket()
    # Payload of 10 bytes forced into 16-bit extended length (violates RFC 6455 5.2)
    frame = make_frame(opcode=1, payload=b"1234567890", force_len_encoding=16)
    s.sendall(frame)
    res = read_frame(s)
    assert res is not None and res["opcode"] == 8
    code = struct.unpack("!H", res["payload"][:2])[0]
    assert code == 1002
    s.close()
    print("[PASS] Non-minimal 16-bit encoding rejected with 1002.")

def test_non_minimal_length_64():
    print("[TEST] Frame Parsing - Non-minimal 64-bit length rejection...")
    s = open_websocket()
    # Payload of 500 bytes forced into 64-bit extended length (< 65536)
    frame = make_frame(opcode=1, payload=b"A" * 500, force_len_encoding=64)
    s.sendall(frame)
    res = read_frame(s)
    assert res is not None and res["opcode"] == 8
    code = struct.unpack("!H", res["payload"][:2])[0]
    assert code == 1002
    s.close()
    print("[PASS] Non-minimal 64-bit encoding rejected with 1002.")

def test_64bit_msb_set():
    print("[TEST] Frame Parsing - 64-bit length with MSB set rejection...")
    s = open_websocket()
    # 64-bit header with bit 63 set
    frame = make_frame(opcode=1, payload=b"test", force_len_encoding=64, msb_set=True)
    s.sendall(frame)
    res = read_frame(s)
    assert res is not None and res["opcode"] == 8
    code = struct.unpack("!H", res["payload"][:2])[0]
    assert code == 1002
    s.close()
    print("[PASS] 64-bit length with MSB set rejected with 1002.")

def test_valid_64bit_frame():
    print("[TEST] Frame Parsing - Valid 64-bit extended length frame (> 65535 bytes)...")
    s = open_websocket()
    # Send 70,000 bytes (triggers valid 64-bit extended length)
    payload = b"X" * 70000
    frame = make_frame(opcode=2, payload=payload) # Binary frame
    s.sendall(frame)
    res = read_frame(s, timeout=5.0)
    assert res is not None
    assert res["opcode"] == 2
    assert len(res["payload"]) == 70000
    s.close()
    print("[PASS] Valid 64-bit extended payload (70,000 bytes) echoed cleanly.")

# =========================================================================
# 3. CONTROL FRAMES & FRAGMENTATION INTERLEAVING
# =========================================================================

def test_ping_during_fragmented_message():
    print("[TEST] Fragmentation - Ping interleaved inside fragmented message...")
    s = open_websocket()
    part1 = "First slice of text, "
    part2 = "and second slice of text."
    
    # 1. Start fragmented text message (FIN=0)
    f1 = make_frame(opcode=1, payload=part1.encode("utf-8"), fin=False)
    s.sendall(f1)
    time.sleep(0.01)
    
    # 2. Inject Ping frame while message is in flight
    ping_data = b"interleaved_ping"
    s.sendall(make_frame(opcode=9, payload=ping_data))
    
    # Verify server answers Ping with Pong immediately without waiting for message completion
    pong = read_frame(s)
    assert pong is not None
    assert pong["opcode"] == 10, f"Expected Pong opcode 10, got {pong['opcode']}"
    assert pong["payload"] == ping_data
    
    # 3. Send final continuation frame (FIN=1)
    f2 = make_frame(opcode=0, payload=part2.encode("utf-8"), fin=True)
    s.sendall(f2)
    
    # Verify server completes and echoes the assembled message
    msg = read_frame(s)
    assert msg is not None
    assert msg["opcode"] == 1
    assert msg["payload"].decode("utf-8") == (part1 + part2)
    s.close()
    print("[PASS] Ping answered with Pong during fragmented message without breaking stream.")

def test_unexpected_binary_in_fragmented_text():
    print("[TEST] Fragmentation - Unexpected data frame during fragmented message...")
    s = open_websocket()
    # Start text fragment
    s.sendall(make_frame(opcode=1, payload=b"Part 1", fin=False))
    # Send unexpected binary frame
    s.sendall(make_frame(opcode=2, payload=b"Unexpected Binary", fin=True))
    res = read_frame(s)
    assert res is not None and res["opcode"] == 8
    code = struct.unpack("!H", res["payload"][:2])[0]
    assert code == 1002
    s.close()
    print("[PASS] Unexpected data frame during fragmented sequence rejected with 1002.")

def test_utf8_split_across_continuation_frames():
    print("[TEST] UTF-8 - Multi-byte UTF-8 character cleanly split across frames...")
    s = open_websocket()
    # 4-byte rocket emoji: 🚀 is b'\xf0\x9f\x9a\x80'
    rocket = "🚀".encode("utf-8")
    assert len(rocket) == 4
    
    # Frame 1 sends first 2 bytes (FIN=0)
    f1 = make_frame(opcode=1, payload=rocket[:2], fin=False)
    # Frame 2 sends last 2 bytes (FIN=1)
    f2 = make_frame(opcode=0, payload=rocket[2:], fin=True)
    
    s.sendall(f1)
    time.sleep(0.01)
    s.sendall(f2)
    
    res = read_frame(s)
    assert res is not None
    assert res["opcode"] == 1
    assert res["payload"].decode("utf-8") == "🚀"
    s.close()
    print("[PASS] Multi-byte UTF-8 split across continuation frames assembled and validated.")

def test_invalid_utf8_in_continuation():
    print("[TEST] UTF-8 - Invalid UTF-8 sequence in continuation frame...")
    s = open_websocket()
    # Frame 1: Valid text (FIN=0)
    s.sendall(make_frame(opcode=1, payload=b"Valid prefix", fin=False))
    # Frame 2: Invalid byte \xFF (FIN=1)
    s.sendall(make_frame(opcode=0, payload=b" invalid \xFF suffix", fin=True))
    
    res = read_frame(s)
    assert res is not None and res["opcode"] == 8
    code = struct.unpack("!H", res["payload"][:2])[0]
    assert code == 1007, f"Expected Invalid Payload Data (1007), got {code}"
    s.close()
    print("[PASS] Invalid UTF-8 across continuation rejected with 1007.")

# =========================================================================
# 4. CLOSE CODE AND CLOSE HANDSHAKE TESTS
# =========================================================================

def test_one_byte_close_payload():
    print("[TEST] Close Handshake - 1-byte close payload rejection...")
    s = open_websocket()
    # RFC 6455 5.5.1: Payload length of close frame cannot be 1
    s.sendall(make_frame(opcode=8, payload=b"\x03"))
    res = read_frame(s)
    assert res is not None and res["opcode"] == 8
    code = struct.unpack("!H", res["payload"][:2])[0]
    assert code == 1002
    s.close()
    print("[PASS] 1-byte close frame payload rejected with 1002.")

def test_invalid_close_codes():
    print("[TEST] Close Handshake - Reserved / out-of-range close codes...")
    bad_codes = [999, 1005, 1006, 1015, 2000, 5000, 60000]
    for bc in bad_codes:
        s = open_websocket()
        payload = struct.pack("!H", bc)
        s.sendall(make_frame(opcode=8, payload=payload))
        res = read_frame(s)
        assert res is not None and res["opcode"] == 8
        code = struct.unpack("!H", res["payload"][:2])[0]
        assert code == 1002, f"Close code {bc} was not rejected with 1002! Got {code}"
        s.close()
    print("[PASS] All invalid close codes rejected with 1002.")

def test_repeated_close_frames():
    print("[TEST] Close Handshake - Repeated close frames...")
    s = open_websocket()
    normal_close = struct.pack("!H", 1000)
    s.sendall(make_frame(opcode=8, payload=normal_close))
    # Immediately send second close frame
    try:
        s.sendall(make_frame(opcode=8, payload=normal_close))
    except Exception:
        pass
    res = read_frame(s)
    assert res is not None and res["opcode"] == 8
    s.close()
    print("[PASS] Repeated close frame handled cleanly without crash.")

def run_all_audit_tests():
    test_handshake_valid()
    test_handshake_duplicate_headers()
    test_handshake_invalid_keys()
    test_handshake_wrong_http_version()
    test_non_minimal_length_16()
    test_non_minimal_length_64()
    test_64bit_msb_set()
    test_valid_64bit_frame()
    test_ping_during_fragmented_message()
    test_unexpected_binary_in_fragmented_text()
    test_utf8_split_across_continuation_frames()
    test_invalid_utf8_in_continuation()
    test_one_byte_close_payload()
    test_invalid_close_codes()
    test_repeated_close_frames()
    print("\nALL PHASE 2.1 AUDIT TESTS PASSED!")

if __name__ == "__main__":
    run_all_audit_tests()
