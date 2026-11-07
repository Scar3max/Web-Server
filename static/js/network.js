/**
 * NetworkManager — Manages WebSocket connection, exponential backoff,
 * input transmission, and authoritative server snapshot reception.
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

        this.localPlayerId = null;
        this.seq = 0;

        // Callbacks
        this.onStatusChange = null;
        this.onSnapshot = null;
        this.onWelcome = null;
        this.onTelemetry = null;

        // Input throttle
        this.lastSendTime = 0;
        this.sendInterval = 16; // ~60 Hz input transmission
    }

    connect() {
        if (this.socket && (this.socket.readyState === WebSocket.OPEN || this.socket.readyState === WebSocket.CONNECTING)) {
            return;
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

        // Immediately send initial input to register with server simulation
        this.sendInput({ up: false, down: false, left: false, right: false }, true);
    }

    handleMessage(e) {
        try {
            const data = JSON.parse(e.data);

            if (data.type === 'welcome') {
                this.localPlayerId = data.playerId;
                if (typeof this.onWelcome === 'function') {
                    this.onWelcome(data.playerId);
                }
            } else if (data.type === 'snapshot') {
                const now = Date.now();
                const rtt = data.timestamp ? (now - data.timestamp) : null;
                if (typeof this.onSnapshot === 'function') {
                    this.onSnapshot(data, rtt);
                }
            } else if (data.type === 'telemetry') {
                const now = Date.now();
                const rtt = data.timestamp ? (now - data.timestamp) : null;
                if (typeof this.onTelemetry === 'function') {
                    this.onTelemetry(data, rtt);
                }
            }
        } catch (err) {
            // Non-JSON or raw text fallback
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

    sendInput(controls, force = false) {
        if (!this.socket || this.socket.readyState !== WebSocket.OPEN) {
            return;
        }

        const now = performance.now();
        if (!force && (now - this.lastSendTime < this.sendInterval)) {
            return;
        }
        this.lastSendTime = now;

        this.seq++;
        const packet = {
            type: 'input',
            seq: this.seq,
            controls: {
                up: !!controls.up,
                down: !!controls.down,
                left: !!controls.left,
                right: !!controls.right
            }
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
