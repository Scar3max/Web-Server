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

        // Room lifecycle callbacks
        this.onRoomCreated = null;
        this.onRoomJoined = null;
        this.onRoomUpdated = null;
        this.onPlayerJoined = null;
        this.onPlayerLeft = null;
        this.onReadyUpdated = null;
        this.onCountdown = null;
        this.onRaceStarted = null;
        this.onRaceFinished = null;
        this.onRoomError = null;
        this.onRoomLeft = null;

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
            } else if (data.type === 'room_created') {
                if (typeof this.onRoomCreated === 'function') {
                    this.onRoomCreated(data);
                }
            } else if (data.type === 'room_joined') {
                if (typeof this.onRoomJoined === 'function') {
                    this.onRoomJoined(data);
                }
            } else if (data.type === 'room_updated') {
                if (typeof this.onRoomUpdated === 'function') {
                    this.onRoomUpdated(data.room);
                }
            } else if (data.type === 'player_joined') {
                if (typeof this.onPlayerJoined === 'function') {
                    this.onPlayerJoined(data);
                }
            } else if (data.type === 'player_left') {
                if (typeof this.onPlayerLeft === 'function') {
                    this.onPlayerLeft(data);
                }
            } else if (data.type === 'ready_updated') {
                if (typeof this.onReadyUpdated === 'function') {
                    this.onReadyUpdated(data);
                }
            } else if (data.type === 'countdown') {
                if (typeof this.onCountdown === 'function') {
                    this.onCountdown(data.countdown, data.room);
                }
            } else if (data.type === 'race_started') {
                if (typeof this.onRaceStarted === 'function') {
                    this.onRaceStarted(data.room);
                }
            } else if (data.type === 'race_finished') {
                if (typeof this.onRaceFinished === 'function') {
                    this.onRaceFinished(data.results, data.room);
                }
            } else if (data.type === 'room_error') {
                if (typeof this.onRoomError === 'function') {
                    this.onRoomError(data.message);
                }
            } else if (data.type === 'room_left') {
                if (typeof this.onRoomLeft === 'function') {
                    this.onRoomLeft(data.roomId);
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

    createRoom(name) {
        if (!this.socket || this.socket.readyState !== WebSocket.OPEN) return;
        this.socket.send(JSON.stringify({
            type: 'create_room',
            name: name || ''
        }));
    }

    joinRoom(code, name) {
        if (!this.socket || this.socket.readyState !== WebSocket.OPEN) return;
        this.socket.send(JSON.stringify({
            type: 'join_room',
            code: (code || '').toUpperCase().trim(),
            name: name || ''
        }));
    }

    leaveRoom() {
        if (!this.socket || this.socket.readyState !== WebSocket.OPEN) return;
        this.socket.send(JSON.stringify({
            type: 'leave_room'
        }));
    }

    setReady(ready) {
        if (!this.socket || this.socket.readyState !== WebSocket.OPEN) return;
        this.socket.send(JSON.stringify({
            type: 'set_ready',
            ready: !!ready
        }));
    }

    startRace() {
        if (!this.socket || this.socket.readyState !== WebSocket.OPEN) return;
        this.socket.send(JSON.stringify({
            type: 'start_race'
        }));
    }

    getRoomInfo() {
        if (!this.socket || this.socket.readyState !== WebSocket.OPEN) return;
        this.socket.send(JSON.stringify({
            type: 'get_room_info'
        }));
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
            return null;
        }

        const now = performance.now();
        if (!force && (now - this.lastSendTime < this.sendInterval)) {
            return null;
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
        return this.seq;
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
