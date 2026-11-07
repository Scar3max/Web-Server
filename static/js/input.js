/**
 * InputManager — Handles keyboard controls smoothly and prevents page scrolling.
 */
export class InputManager {
    constructor() {
        this.keys = {
            up: false,
            down: false,
            left: false,
            right: false
        };
        this.onPauseToggle = null;
        this.initListeners();
    }

    initListeners() {
        window.addEventListener('keydown', (e) => this.handleKeyDown(e));
        window.addEventListener('keyup', (e) => this.handleKeyUp(e));
        window.addEventListener('blur', () => this.resetAllKeys());
    }

    handleKeyDown(e) {
        if (e.repeat) return;

        // Prevent browser scrolling on arrow keys and space
        if (['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Space'].includes(e.code)) {
            e.preventDefault();
        }

        switch (e.code) {
            case 'ArrowUp':
            case 'KeyW':
                this.keys.up = true;
                break;
            case 'ArrowDown':
            case 'KeyS':
                this.keys.down = true;
                break;
            case 'ArrowLeft':
            case 'KeyA':
                this.keys.left = true;
                break;
            case 'ArrowRight':
            case 'KeyD':
                this.keys.right = true;
                break;
            case 'Escape':
                if (typeof this.onPauseToggle === 'function') {
                    this.onPauseToggle();
                }
                break;
        }
    }

    handleKeyUp(e) {
        switch (e.code) {
            case 'ArrowUp':
            case 'KeyW':
                this.keys.up = false;
                break;
            case 'ArrowDown':
            case 'KeyS':
                this.keys.down = false;
                break;
            case 'ArrowLeft':
            case 'KeyA':
                this.keys.left = false;
                break;
            case 'ArrowRight':
            case 'KeyD':
                this.keys.right = false;
                break;
        }
    }

    resetAllKeys() {
        this.keys.up = false;
        this.keys.down = false;
        this.keys.left = false;
        this.keys.right = false;
    }

    getState() {
        return {
            throttle: this.keys.up,
            brake: this.keys.down,
            left: this.keys.left,
            right: this.keys.right
        };
    }
}
