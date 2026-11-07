import { InputManager } from './input.js';
import { Renderer } from './renderer.js';
import { NetworkManager } from './network.js';
import { Game } from './game.js';

document.addEventListener('DOMContentLoaded', () => {
    // HUD & Common Elements
    const canvas = document.getElementById('gameCanvas');
    const speedVal = document.getElementById('speedValue');
    const distanceVal = document.getElementById('distanceValue');
    const lapVal = document.getElementById('lapValue');
    const tickVal = document.getElementById('tickValue');
    const statusPill = document.getElementById('statusPill');
    const statusText = document.getElementById('statusText');
    const latencyBadge = document.getElementById('latencyBadge');
    const telemetryBox = document.getElementById('telemetryBox');
    const pauseOverlay = document.getElementById('pauseOverlay');

    // Menu Screen Elements
    const menuOverlay = document.getElementById('menuOverlay');
    const playerNameInput = document.getElementById('playerNameInput');
    const btnCreateRoom = document.getElementById('btnCreateRoom');
    const roomCodeInput = document.getElementById('roomCodeInput');
    const btnJoinRoom = document.getElementById('btnJoinRoom');
    const menuError = document.getElementById('menuError');
    const btnSoloPractice = document.getElementById('btnSoloPractice');

    // Lobby Screen Elements
    const lobbyOverlay = document.getElementById('lobbyOverlay');
    const lobbyRoomCode = document.getElementById('lobbyRoomCode');
    const btnCopyCode = document.getElementById('btnCopyCode');
    const lobbyPlayerCount = document.getElementById('lobbyPlayerCount');
    const lobbyPlayerList = document.getElementById('lobbyPlayerList');
    const lobbyNotice = document.getElementById('lobbyNotice');
    const btnToggleReady = document.getElementById('btnToggleReady');
    const btnStartRace = document.getElementById('btnStartRace');
    const btnLeaveRoom = document.getElementById('btnLeaveRoom');

    // Countdown & Results Elements
    const countdownBanner = document.getElementById('countdownBanner');
    const resultsOverlay = document.getElementById('resultsOverlay');
    const resultsList = document.getElementById('resultsList');
    const btnReturnToLobby = document.getElementById('btnReturnToLobby');

    const inputManager = new InputManager();
    const renderer = new Renderer(canvas);
    const networkManager = new NetworkManager();

    let currentRoom = null;
    let isReady = false;
    let isHost = false;

    // Canvas sizing
    function handleResize() {
        const rect = canvas.getBoundingClientRect();
        renderer.resize(rect.width, rect.height);
    }
    window.addEventListener('resize', handleResize);
    handleResize();

    // UI State Helpers
    function hideAllScreens() {
        if (menuOverlay) menuOverlay.classList.remove('active');
        if (lobbyOverlay) lobbyOverlay.classList.remove('active');
        if (countdownBanner) countdownBanner.classList.remove('active');
        if (resultsOverlay) resultsOverlay.classList.remove('active');
    }

    function showMenu() {
        hideAllScreens();
        game.stopDriving();
        currentRoom = null;
        isReady = false;
        isHost = false;
        if (menuOverlay) menuOverlay.classList.add('active');
        if (menuError) menuError.textContent = '';
    }

    function showLobby(room) {
        hideAllScreens();
        game.stopDriving();
        currentRoom = room;
        if (lobbyOverlay) lobbyOverlay.classList.add('active');
        updateLobbyUI(room);
    }

    function updateLobbyUI(room) {
        if (!room) return;
        currentRoom = room;

        if (lobbyRoomCode) lobbyRoomCode.textContent = room.code || '------';
        if (lobbyPlayerCount) lobbyPlayerCount.textContent = (room.players ? room.players.length : 0);

        const myId = networkManager.localPlayerId;
        isHost = (room.hostPlayerId === myId);

        // Find my ready state
        if (room.players) {
            const me = room.players.find(p => p.playerId === myId);
            if (me) {
                isReady = me.ready;
            }
        }

        if (btnToggleReady) {
            btnToggleReady.textContent = isReady ? 'Cancel Ready' : 'Ready Up';
            btnToggleReady.className = isReady ? 'btn btn-secondary' : 'btn btn-primary';
        }

        // Render player list
        if (lobbyPlayerList && room.players) {
            lobbyPlayerList.innerHTML = '';
            let allReady = (room.players.length >= 2);

            room.players.forEach(p => {
                if (!p.ready) allReady = false;

                const item = document.createElement('div');
                item.className = 'player-item';

                const nameCol = document.createElement('div');
                nameCol.className = 'player-name-col';
                nameCol.textContent = p.name || `Player ${p.playerId}`;

                if (p.isHost) {
                    const hostBadge = document.createElement('span');
                    hostBadge.className = 'badge badge-host';
                    hostBadge.textContent = 'HOST';
                    nameCol.appendChild(hostBadge);
                }

                const statusBadge = document.createElement('span');
                statusBadge.className = `badge ${p.ready ? 'badge-ready' : 'badge-unready'}`;
                statusBadge.textContent = p.ready ? 'READY' : 'WAITING';

                item.appendChild(nameCol);
                item.appendChild(statusBadge);
                lobbyPlayerList.appendChild(item);
            });

            // Start Race button visibility and state
            if (btnStartRace) {
                btnStartRace.style.display = isHost ? 'flex' : 'none';
                btnStartRace.disabled = !allReady;
            }

            if (lobbyNotice) {
                if (room.players.length < 2) {
                    lobbyNotice.textContent = 'Waiting for at least 2 pilots to join...';
                } else if (!allReady) {
                    lobbyNotice.textContent = 'Waiting for all pilots to ready up...';
                } else if (isHost) {
                    lobbyNotice.textContent = 'All pilots ready! You can now start the race.';
                } else {
                    lobbyNotice.textContent = 'All pilots ready! Waiting for host to start.';
                }
            }
        }
    }

    function showCountdown(seconds) {
        if (!countdownBanner) return;
        countdownBanner.classList.add('active');
        if (seconds <= 0.5) {
            countdownBanner.textContent = 'GO!';
        } else {
            countdownBanner.textContent = Math.ceil(seconds);
        }
    }

    function showResults(results) {
        hideAllScreens();
        game.stopDriving();
        if (resultsOverlay) resultsOverlay.classList.add('active');

        if (resultsList && results) {
            resultsList.innerHTML = '';
            results.forEach(r => {
                const tr = document.createElement('tr');

                const tdRank = document.createElement('td');
                if (r.rank === 1) tdRank.className = 'rank-gold';
                else if (r.rank === 2) tdRank.className = 'rank-silver';
                else if (r.rank === 3) tdRank.className = 'rank-bronze';
                tdRank.textContent = `#${r.rank}`;

                const tdName = document.createElement('td');
                tdName.textContent = r.name || `Player ${r.playerId}`;

                const tdTime = document.createElement('td');
                tdTime.textContent = `${Number(r.finishTime).toFixed(2)}s`;

                tr.appendChild(tdRank);
                tr.appendChild(tdName);
                tr.appendChild(tdTime);
                resultsList.appendChild(tr);
            });
        }
    }

    // Network Status updates
    networkManager.onStatusChange = (state, label) => {
        if (!statusPill) return;

        statusPill.className = 'status-pill';
        if (state === 'CONNECTED') {
            statusPill.classList.add('status-connected');
        } else if (state === 'CONNECTING') {
            statusPill.classList.add('status-connecting');
        } else {
            statusPill.classList.add('status-disconnected');
            // If disconnected, return to menu
            if (currentRoom) {
                showMenu();
                if (menuError) menuError.textContent = 'Disconnected from server';
            }
        }

        if (statusText) {
            statusText.textContent = label;
        }
    };

    // Telemetry & Snapshot Display
    networkManager.onSnapshot = (snapshot, rtt) => {
        if (latencyBadge && rtt !== null) {
            latencyBadge.textContent = `${rtt}ms RTT`;
        }
        if (telemetryBox) {
            telemetryBox.textContent = JSON.stringify(snapshot, null, 2);
        }

        // Live countdown sync from snapshot if in COUNTDOWN state
        if (snapshot.state === 'COUNTDOWN') {
            showCountdown(snapshot.countdown);
        } else if (snapshot.state === 'RACING') {
            if (countdownBanner && countdownBanner.classList.contains('active')) {
                countdownBanner.classList.remove('active');
            }
            game.startDriving();
        } else if (snapshot.state === 'FINISHED') {
            if (snapshot.results && snapshot.results.length > 0) {
                showResults(snapshot.results);
            }
        }
    };

    networkManager.onTelemetry = (data, rtt) => {
        if (latencyBadge && rtt !== null) {
            latencyBadge.textContent = `${rtt}ms RTT`;
        }
        if (telemetryBox) {
            telemetryBox.textContent = JSON.stringify(data, null, 2);
        }
    };

    // Room Event Listeners
    networkManager.onRoomCreated = (data) => {
        showLobby(data.room);
    };

    networkManager.onRoomJoined = (data) => {
        showLobby(data.room);
    };

    networkManager.onRoomUpdated = (room) => {
        updateLobbyUI(room);
    };

    networkManager.onPlayerJoined = (data) => {
        updateLobbyUI(data.room);
    };

    networkManager.onPlayerLeft = (data) => {
        updateLobbyUI(data.room);
    };

    networkManager.onReadyUpdated = (data) => {
        updateLobbyUI(data.room);
    };

    networkManager.onCountdown = (countdown, room) => {
        showCountdown(countdown);
    };

    networkManager.onRaceStarted = (room) => {
        hideAllScreens();
        game.startDriving();
    };

    networkManager.onRaceFinished = (results, room) => {
        showResults(results);
    };

    networkManager.onRoomLeft = () => {
        showMenu();
    };

    networkManager.onRoomError = (msg) => {
        if (menuError && menuOverlay && menuOverlay.classList.contains('active')) {
            menuError.textContent = msg;
        } else if (lobbyNotice && lobbyOverlay && lobbyOverlay.classList.contains('active')) {
            lobbyNotice.textContent = msg;
            lobbyNotice.style.color = 'var(--danger)';
            setTimeout(() => {
                if (lobbyNotice) lobbyNotice.style.color = 'var(--text-muted)';
            }, 3000);
        }
    };

    // Button Bindings
    if (btnCreateRoom) {
        btnCreateRoom.onclick = () => {
            const name = playerNameInput ? playerNameInput.value.trim() : '';
            networkManager.createRoom(name);
        };
    }

    if (btnJoinRoom) {
        btnJoinRoom.onclick = () => {
            const code = roomCodeInput ? roomCodeInput.value.trim() : '';
            const name = playerNameInput ? playerNameInput.value.trim() : '';
            if (!code) {
                if (menuError) menuError.textContent = 'Please enter a 6-character room code.';
                return;
            }
            networkManager.joinRoom(code, name);
        };
    }

    if (btnSoloPractice) {
        btnSoloPractice.onclick = () => {
            hideAllScreens();
            game.startDriving();
        };
    }

    if (btnCopyCode) {
        btnCopyCode.onclick = () => {
            if (currentRoom && currentRoom.code) {
                navigator.clipboard.writeText(currentRoom.code).then(() => {
                    btnCopyCode.textContent = '✓ Copied';
                    setTimeout(() => { btnCopyCode.textContent = '📋 Copy'; }, 1500);
                });
            }
        };
    }

    if (btnToggleReady) {
        btnToggleReady.onclick = () => {
            networkManager.setReady(!isReady);
        };
    }

    if (btnStartRace) {
        btnStartRace.onclick = () => {
            networkManager.startRace();
        };
    }

    if (btnLeaveRoom) {
        btnLeaveRoom.onclick = () => {
            networkManager.leaveRoom();
        };
    }

    if (btnReturnToLobby) {
        btnReturnToLobby.onclick = () => {
            if (currentRoom) {
                showLobby(currentRoom);
                isReady = false;
                networkManager.setReady(false);
            } else {
                showMenu();
            }
        };
    }

    const uiElements = {
        speedVal,
        distanceVal,
        lapVal,
        tickVal,
        overlay: pauseOverlay
    };

    const game = new Game(canvas, inputManager, renderer, networkManager, uiElements);
    game.start();

    // Cleanup on window unload
    window.addEventListener('beforeunload', () => {
        game.stop();
    });
});
