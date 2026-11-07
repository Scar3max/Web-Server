#!/bin/bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$DIR"

echo "Starting server..."
./build/server &
SERVER_PID=$!
sleep 1

# Trap exit to ensure server is killed
cleanup() {
    echo "Stopping server (PID $SERVER_PID)..."
    kill -SIGINT $SERVER_PID 2>/dev/null || true
    wait $SERVER_PID 2>/dev/null || true
}
trap cleanup EXIT

echo "Running functional test suite..."
python3 tests/test_phase1.py

echo ""
echo "Running ApacheBench concurrency test (5000 requests, 50 concurrency)..."
ab -n 5000 -c 50 http://127.0.0.1:8080/

echo ""
echo "Running ApacheBench for /about (2000 requests, 25 concurrency)..."
ab -n 2000 -c 25 http://127.0.0.1:8080/about

echo ""
echo "All tests and benchmarks completed successfully!"
