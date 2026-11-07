# =============================================================================
# Multi-Stage Dockerfile for C++20 Multiplayer Racing Server
# =============================================================================

# -----------------------------------------------------------------------------
# Stage 1: Build Environment
# -----------------------------------------------------------------------------
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Copy project definition and source tree
COPY CMakeLists.txt ./
COPY include/ ./include/
COPY src/ ./src/

# Build optimized Release binary
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --target server -j$(nproc)

# -----------------------------------------------------------------------------
# Stage 2: Minimal Production Runtime
# -----------------------------------------------------------------------------
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    curl \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Security: run as non-root user
RUN groupadd -g 10001 appgroup \
    && useradd -u 10001 -g appgroup -s /sbin/nologin -M appuser

WORKDIR /app

# Copy binary and static assets
COPY --from=builder /src/build/server /app/server
COPY static/ /app/static/

RUN chown -R appuser:appgroup /app

USER appuser:appgroup

# Production environment defaults
ENV HTTP_HOST=0.0.0.0 \
    HTTP_PORT=8080 \
    HTTP_STATIC_DIR=/app/static \
    HTTP_WORKERS=4 \
    HTTP_LOG_LEVEL=INFO

EXPOSE 8080

# Forward SIGINT for clean simulation shutdown
STOPSIGNAL SIGINT

# Reliable container health check
HEALTHCHECK --interval=10s --timeout=3s --start-period=3s --retries=3 \
    CMD curl -f http://localhost:8080/health || exit 1

ENTRYPOINT ["/app/server"]
