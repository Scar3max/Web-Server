#!/bin/bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$DIR"

echo "Starting server..."
./build/server &
SERVER_PID=$!
sleep 1

cleanup() {
    echo "Stopping server (PID $SERVER_PID)..."
    kill -SIGINT $SERVER_PID 2>/dev/null || true
    wait $SERVER_PID 2>/dev/null || true
}
trap cleanup EXIT

echo "=================================================="
echo "1. Running Phase 1 HTTP Functional Test Suite"
echo "=================================================="
python3 tests/test_phase1.py

echo ""
echo "=================================================="
echo "2. Running Phase 2 WebSocket Integration Test Suite"
echo "=================================================="
python3 tests/test_phase2_ws.py

echo ""
echo "=================================================="
echo "3. Running Phase 2.1 Security & Correctness Audit Tests"
echo "=================================================="
python3 tests/test_phase2_audit.py

echo ""
echo "=================================================="
echo "4. Running Phase 3 Client & Static Asset Tests"
echo "=================================================="
python3 tests/test_phase3_client.py

echo ""
echo "=================================================="
echo "5. Running ApacheBench Concurrency Regression Test"
echo "=================================================="
ab -n 5000 -c 50 http://127.0.0.1:8080/

echo ""
echo "All test suites and benchmarks completed successfully!"
