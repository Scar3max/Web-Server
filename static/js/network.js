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

        // Diagnostics & Optimization Stats (Phase 7.1)
        this.snapshotFormat = 'json';
        this.lastSnapshotBytes = 0;
        this.snapshotBytesTotal = 0;
    }

    connect() {
        if (this.socket && (this.socket.readyState === WebSocket.OPEN || this.socket.readyState === WebSocket.CONNECTING)) {
            return;
        }

        this.isExplicitlyClosed = false;
        this.updateStatus('CONNECTING', 'Connecting to /ws...');

        const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        const host = window.location.host || 'localhost:8080';
        const wsUrl = `${protocol}//${host}/ws?format=binary`;

        try {
            this.socket = new WebSocket(wsUrl);
            this.socket.binaryType = 'arraybuffer';

            this.socket.onopen = (e) => this.handleOpen(e);
            this.socket.onmessage = (e) => this.handleMessage(e);
            this.socket.onerror = (e) => this.handleError(e);
            this.socket.onclose = (e) => this.handleClose(e);
        } catch (err) {
            this.updateStatus('ERROR', 'WebSocket init failed');
            this.scheduleReconnect();
        }
    }

    decodeBinarySnapshot(buffer) {
        if (!buffer || buffer.byteLength < 36) return null;
        const view = new DataView(buffer);

        // Magic 0x534E4150 ('SNAP')
        const magic = view.getUint32(0, false);
        if (magic !== 0x534E4150) return null;

        const version = view.getUint8(4);
        if (version !== 1) return null;

        const roomStateNum = view.getUint8(5);
        const stateNames = ['WAITING', 'COUNTDOWN', 'RACING', 'FINISHED'];
        const state = stateNames[roomStateNum] || 'WAITING';

        const countdownRaw = view.getUint16(6, false);
        const countdown = countdownRaw / 10.0;

        const roomId = view.getUint32(8, false);

        // RoomCode (8 bytes null-terminated ASCII)
        const decoder = new TextDecoder('utf-8');
        const codeBytes = new Uint8Array(buffer, 12, 8);
        const roomCode = decoder.decode(codeBytes).replace(/\0/g, '');

        const tick = view.getUint32(20, false);
        const timestampHigh = view.getUint32(24, false);
        const timestampLow = view.getUint32(28, false);
        const timestamp = (timestampHigh * 4294967296) + timestampLow;

        const playerCount = view.getUint16(32, false);
        const resultCount = view.getUint16(34, false);

        let offset = 36;
        const players = [];
        for (let i = 0; i < playerCount; i++) {
            if (offset + 36 > buffer.byteLength) break;
            const id = view.getUint32(offset, false);
            const x = view.getFloat32(offset + 4, false);
            const distance = view.getFloat32(offset + 8, false);
            const speed = view.getFloat32(offset + 12, false);
            const steer = view.getFloat32(offset + 16, false);
            const worldX = view.getFloat32(offset + 20, false);
            const worldY = view.getFloat32(offset + 24, false);
            const heading = view.getFloat32(offset + 28, false);
            const ackSeq = view.getUint32(offset + 32, false);
            offset += 36;

            players.push({ id, x, distance, speed, steer, worldX, worldY, heading, ackSeq });
        }

        const results = [];
        for (let i = 0; i < resultCount; i++) {
            if (offset + 11 > buffer.byteLength) break;
            const playerId = view.getUint32(offset, false);
            const rank = view.getUint16(offset + 4, false);
            const finishTime = view.getFloat32(offset + 6, false);
            const nameLen = view.getUint8(offset + 10);
            offset += 11;

            let name = '';
            if (nameLen > 0 && offset + nameLen <= buffer.byteLength) {
                name = decoder.decode(new Uint8Array(buffer, offset, nameLen));
                offset += nameLen;
            }
            results.push({ playerId, name, rank, finishTime });
        }

        return {
            type: 'snapshot',
            roomId,
            roomCode,
            state,
            countdown,
            tick,
            timestamp,
            players,
            results
        };
    }

    handleOpen(e) {
        this.reconnectAttempts = 0;
        this.updateStatus('CONNECTED', 'Connected to /ws');
    }

    handleMessage(e) {
        try {
            let data = null;
            if (e.data instanceof ArrayBuffer) {
                data = this.decodeBinarySnapshot(e.data);
                this.snapshotFormat = 'binary';
                this.lastSnapshotBytes = e.data.byteLength;
                this.snapshotBytesTotal += e.data.byteLength;
            } else if (typeof e.data === 'string') {
                data = JSON.parse(e.data);
                if (data.type === 'snapshot') {
                    this.snapshotFormat = 'json';
                    this.lastSnapshotBytes = e.data.length;
                    this.snapshotBytesTotal += e.data.length;
                }
            }

            if (!data) return;

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
