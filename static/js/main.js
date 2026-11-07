import { InputManager } from './input.js';
import { Renderer } from './renderer.js';
import { NetworkManager } from './network.js';
import { Game } from './game.js';

document.addEventListener('DOMContentLoaded', () => {
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

    const inputManager = new InputManager();
    const renderer = new Renderer(canvas);
    const networkManager = new NetworkManager();

    // Canvas sizing
    function handleResize() {
        const rect = canvas.getBoundingClientRect();
        renderer.resize(rect.width, rect.height);
    }
    window.addEventListener('resize', handleResize);
    handleResize();

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
    };

    networkManager.onTelemetry = (data, rtt) => {
        if (latencyBadge && rtt !== null) {
            latencyBadge.textContent = `${rtt}ms RTT`;
        }
        if (telemetryBox) {
            telemetryBox.textContent = JSON.stringify(data, null, 2);
        }
    };

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
