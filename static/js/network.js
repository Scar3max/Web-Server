/**
 * NetworkManager — Manages WebSocket connection, exponential reconnection, and telemetry exchange.
 */
export class NetworkManager {
    constructor() {
        this.socket = null;
        this.status = 'DISCONNECTED'; // DISCONNECTED, CONNECTING, CONNECTED, ERROR
        this.reconnectAttempts = 0;
        this.maxReconnectAttempts = 10;
        this.baseReconnectDelay = 1000;
        this.maxReconnectDelay = 10000;
        this.reconnectTimer = null;
        this.isExplicitlyClosed = false;

        this.onStatusChange = null;
        this.onTelemetry = null;

        // Telemetry throttle
        this.lastSendTime = 0;
        this.sendInterval = 50; // Send telemetry at ~20 Hz
    }

    connect() {
        if (this.socket && (this.socket.readyState === WebSocket.OPEN || this.socket.readyState === WebSocket.CONNECTING)) {
            return; // Avoid multiple simultaneous sockets
        }

        this.isExplicitlyClosed = false;
        this.updateStatus('CONNECTING', 'Connecting to /ws...');

        const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        const host = window.location.host || 'localhost:8080';
        const wsUrl = `${protocol}//${host}/ws`;

        try {
            this.socket = new WebSocket(wsUrl);

            this.socket.onopen = (e) => this.handleOpen(e);
            this.socket.onmessage = (e) => this.handleMessage(e);
            this.socket.onerror = (e) => this.handleError(e);
            this.socket.onclose = (e) => this.handleClose(e);
        } catch (err) {
            this.updateStatus('ERROR', 'WebSocket init failed');
            this.scheduleReconnect();
        }
    }

    handleOpen(e) {
        this.reconnectAttempts = 0;
        this.updateStatus('CONNECTED', 'Connected to /ws');
    }

    handleMessage(e) {
        try {
            const data = JSON.parse(e.data);
            const now = Date.now();
            const rtt = data.timestamp ? (now - data.timestamp) : null;

            if (typeof this.onTelemetry === 'function') {
                this.onTelemetry(data, rtt);
            }
        } catch (err) {
            // Non-JSON or raw text echoed back
            if (typeof this.onTelemetry === 'function') {
                this.onTelemetry({ raw: e.data }, null);
            }
        }
    }

    handleError(e) {
        this.updateStatus('ERROR', 'Socket error encountered');
    }

    handleClose(e) {
        this.socket = null;
        if (!this.isExplicitlyClosed) {
            this.updateStatus('DISCONNECTED', `Disconnected (code ${e.code || 1006})`);
            this.scheduleReconnect();
        } else {
            this.updateStatus('DISCONNECTED', 'Connection closed');
        }
    }

    scheduleReconnect() {
        if (this.reconnectTimer) return;
        if (this.reconnectAttempts >= this.maxReconnectAttempts) {
            this.updateStatus('DISCONNECTED', 'Max reconnect retries reached');
            return;
        }

        this.reconnectAttempts++;
        const delay = Math.min(
            this.baseReconnectDelay * Math.pow(1.5, this.reconnectAttempts - 1),
            this.maxReconnectDelay
        );

        this.updateStatus('CONNECTING', `Reconnecting in ${(delay / 1000).toFixed(1)}s (try ${this.reconnectAttempts}/${this.maxReconnectAttempts})...`);

        this.reconnectTimer = setTimeout(() => {
            this.reconnectTimer = null;
            this.connect();
        }, delay);
    }

    sendTelemetry(controls, speed, distance) {
        if (!this.socket || this.socket.readyState !== WebSocket.OPEN) {
            return;
        }

        const now = Date.now();
        if (now - this.lastSendTime < this.sendInterval) {
            return;
        }
        this.lastSendTime = now;

        const packet = {
            type: 'telemetry',
            timestamp: now,
            controls: controls,
            speed: Math.round(speed),
            distance: Math.round(distance)
        };

        this.socket.send(JSON.stringify(packet));
    }

    updateStatus(state, label) {
        this.status = state;
        if (typeof this.onStatusChange === 'function') {
            this.onStatusChange(state, label);
        }
    }

    disconnect() {
        this.isExplicitlyClosed = true;
        if (this.reconnectTimer) {
            clearTimeout(this.reconnectTimer);
            this.reconnectTimer = null;
        }
        if (this.socket) {
            this.socket.close();
            this.socket = null;
        }
    }
}
