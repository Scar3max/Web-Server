import socket
import time
import sys
import subprocess

HOST = "127.0.0.1"
PORT = 8080

def test_route(path, expected_substr):
    print(f"[TEST] Testing route {path}...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, PORT))
    req = f"GET {path} HTTP/1.1\r\nHost: {HOST}:{PORT}\r\nConnection: close\r\n\r\n"
    s.sendall(req.encode())
    
    response = b""
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        response += chunk
    s.close()

    resp_str = response.decode(errors="replace")
    assert "200 OK" in resp_str, f"Expected 200 OK, got: {resp_str[:100]}"
    assert expected_substr in resp_str, f"Expected substring '{expected_substr}' not found in response"
    print(f"[PASS] Route {path} returned 200 OK with expected content.")

def test_fragmented_request():
    print("[TEST] Testing fragmented HTTP request (byte-by-byte transmission)...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, PORT))
    
    full_req = f"GET /about HTTP/1.1\r\nHost: {HOST}:{PORT}\r\nConnection: close\r\n\r\n"
    # Send request in small 3-byte fragments with pauses
    for i in range(0, len(full_req), 3):
        chunk = full_req[i:i+3]
        s.sendall(chunk.encode())
        time.sleep(0.01) # 10ms pause between fragments
    
    response = b""
    while True:
        data = s.recv(4096)
        if not data:
            break
        response += data
    s.close()
    
    resp_str = response.decode(errors="replace")
    assert "200 OK" in resp_str, f"Fragmented request failed: {resp_str[:100]}"
    assert "Under The Hood" in resp_str or "Architecture" in resp_str, "Fragmented request returned wrong body"
    print("[PASS] Fragmented request correctly buffered and processed.")

def test_large_response_and_partial_writes():
    print("[TEST] Testing large response & partial writes with slow reader...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    # Set small receive buffer to induce TCP window throttling on the server side
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)
    s.connect((HOST, PORT))
    
    req = f"GET /large HTTP/1.1\r\nHost: {HOST}:{PORT}\r\nConnection: close\r\n\r\n"
    s.sendall(req.encode())
    
    response = b""
    while True:
        # Read slowly in small chunks to force server into EAGAIN and EPOLLOUT registration
        chunk = s.recv(1024)
        if not chunk:
            break
        response += chunk
        time.sleep(0.002) # 2ms pause
    s.close()
    
    resp_str = response.decode(errors="replace")
    assert "200 OK" in resp_str, f"Large response failed header check: {resp_str[:100]}"
    # Verify Content-Length matches body length
    header_end = resp_str.find("\r\n\r\n")
    headers = resp_str[:header_end]
    body = resp_str[header_end + 4:]
    
    cl_header = [h for h in headers.split("\r\n") if h.lower().startswith("content-length:")]
    assert len(cl_header) == 1, "Missing Content-Length header"
    expected_len = int(cl_header[0].split(":")[1].strip())
    assert len(body) == expected_len, f"Body length mismatch: expected {expected_len}, got {len(body)}"
    assert "Chunk 255:" in body, "Large body was truncated"
    print(f"[PASS] Large response fully transmitted ({len(body)} bytes) under slow-read partial writes.")

def test_client_abrupt_disconnect():
    print("[TEST] Testing client abrupt disconnect during request...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, PORT))
    s.sendall(b"GET /abo")
    # Close socket abruptly (RST/FIN)
    s.close()
    time.sleep(0.05)
    
    # Verify server is still completely healthy and responsive
    test_route("/", "Optimized C++ Server")
    print("[PASS] Abrupt client disconnect handled cleanly without server crash or hang.")

def test_header_too_large():
    print("[TEST] Testing oversized header rejection...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, PORT))
    # Send > 8KB of header data without \r\n\r\n
    junk_headers = "X-Junk: " + ("A" * 9000) + "\r\n"
    req = f"GET / HTTP/1.1\r\n{junk_headers}\r\n"
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
    
    resp_str = response.decode(errors="replace")
    assert ("431" in resp_str or "400" in resp_str or len(response) == 0), f"Expected 431/400 or close, got: {resp_str[:100]}"
    print("[PASS] Oversized header correctly rejected / connection closed.")

def run_all():
    test_route("/", "Optimized C++ Server")
    test_route("/about", "Under The Hood")
    test_fragmented_request()
    test_large_response_and_partial_writes()
    test_client_abrupt_disconnect()
    test_header_too_large()
    print("\nALL PHASE 1 FUNCTIONAL TESTS PASSED!")

if __name__ == "__main__":
    run_all()
