// =============================================================================
// Robo Soccer Bot Controller - Waitlist Version with Nicknames
// =============================================================================

// =============================================================================
// WebSocket Setup
// =============================================================================

let socket;
let isAdmin = false;
let connectionStatus = 'disconnected';
let playerList = [];
let myNickname = '';

// WebSocket connection
socket = new WebSocket("ws://" + window.location.hostname + "/ws");

socket.onopen = () => {
    console.log("✓ Connected to ESP32");
    updateStatus('connected', 'Connected');
};

socket.onerror = (error) => {
    console.error("WebSocket error:", error);
    updateStatus('error', 'Connection error');
};

socket.onclose = () => {
    console.log("Disconnected from ESP32");
    updateStatus('disconnected', 'Disconnected');
    setTimeout(() => {
        location.reload();  // Auto-reconnect
    }, 3000);
};

socket.onmessage = (event) => {
    try {
        const data = JSON.parse(event.data);
        handleServerMessage(data);
    } catch (e) {
        console.error("Failed to parse message:", e);
    }
};

// =============================================================================
// Control State
// =============================================================================

const controlState = {
    x: 0,      // Joystick X axis (-100 to +100)
    y: 0,      // Joystick Y axis (-100 to +100)
    f: 0       // Flapper state (0 or 1)
};

// =============================================================================
// Nickname Management
// =============================================================================

function submitNickname() {
    const input = document.getElementById('nicknameInput');
    let nickname = input.value.trim();
    
    if (!nickname) {
        alert('Please enter a nickname');
        return;
    }
    
    // Validate: max 10 chars, ASCII only
    nickname = nickname.substring(0, 10);
    nickname = nickname.replace(/[^\x20-\x7E]/g, ''); // ASCII only
    
    if (!nickname) {
        alert('Please use ASCII characters only');
        return;
    }
    
    myNickname = nickname;
    
    // Send to server
    socket.send(JSON.stringify({ nickname: nickname }));
    
    // Hide modal
    document.getElementById('nicknameModal').classList.remove('visible');
    console.log('Nickname set:', nickname);
}

// Allow Enter key to submit
document.addEventListener('DOMContentLoaded', () => {
    const input = document.getElementById('nicknameInput');
    if (input) {
        input.addEventListener('keypress', (e) => {
            if (e.key === 'Enter') {
                submitNickname();
            }
        });
    }
});

// =============================================================================
// Server Message Handler
// =============================================================================

function handleServerMessage(data) {
    console.log('RX ←', data);
    
    if (data.type === 'requestNickname') {
        // Server wants us to enter nickname
        document.getElementById('nicknameModal').classList.add('visible');
        document.getElementById('nicknameInput').focus();
    }
    
    if (data.type === 'controlStatus') {
        // Update player's control status
        updateControlBanner(data);
    }
    
    if (data.type === 'playerList') {
        // Update player list (for admin)
        playerList = data.players || [];
        updatePlayerList();
    }
    
    if (data.status === 'admin') {
        // Auto-detected as admin device
        isAdmin = true;
        updateStatus('admin', data.message + ' (Admin Device)');
        console.log('%c🔐 ADMIN DEVICE DETECTED', 'color: #f5576c; font-weight: bold; font-size: 16px;');
        
        // Show admin controls and player list
        document.getElementById('adminControls').classList.add('visible');
        document.getElementById('playerList').classList.add('visible');
    }
    
    if (data.status === 'connected') {
        isAdmin = false;
        updateStatus('connected', data.message);
    }
    
    if (data.status === 'denied') {
        updateStatus('denied', data.message);
        alert('⛔ ' + data.message);
    }
    
    if (data.status === 'kicked') {
        alert('❌ ' + data.message);
        updateStatus('disconnected', 'Kicked by admin');
    }
}

function updateControlBanner(data) {
    const banner = document.getElementById('controlBanner');
    banner.classList.add('visible');
    
    if (data.adminControlling) {
        banner.className = 'admin-control visible';
        banner.textContent = '⚠️ ADMIN IS CONTROLLING - Your input is ignored';
    } else if (data.hasControl) {
        banner.className = 'active visible';
        banner.textContent = '✅ YOU HAVE CONTROL';
    } else {
        banner.className = 'waiting visible';
        banner.textContent = `⏳ WAITING - Position ${data.position} of ${data.queueSize}`;
    }
}

function updatePlayerList() {
    const content = document.getElementById('playerListContent');
    if (!content) return;
    
    if (playerList.length === 0) {
        content.innerHTML = '<div style="text-align:center;color:#666;">No players connected</div>';
        return;
    }
    
    content.innerHTML = '';
    
    playerList.forEach((player, index) => {
        const item = document.createElement('div');
        item.className = 'player-item';
        
        const badge = player.active ? 
            '<span class="player-badge active">ACTIVE</span>' : 
            '<span class="player-badge waiting">WAITING</span>';
        
        item.innerHTML = `
            <div class="player-info">
                <div class="player-name">${escapeHtml(player.nickname)} ${badge}</div>
                <div class="player-ip">${player.ip}</div>
            </div>
            <div class="player-actions">
                ${!player.active ? `<button class="player-btn switch" onclick="switchPlayer(${index})">SWITCH</button>` : ''}
                <button class="player-btn kick" onclick="kickPlayerAtIndex(${index})">KICK</button>
            </div>
        `;
        
        content.appendChild(item);
    });
}

function escapeHtml(text) {
    const div = document.createElement('div');
    div.textContent = text;
    return div.innerHTML;
}

function switchPlayer(index) {
    if (!isAdmin) return;
    socket.send(JSON.stringify({ switchPlayer: index }));
    console.log('TX → Switch to player', index);
}

function kickPlayerAtIndex(index) {
    if (!isAdmin) return;
    
    const player = playerList[index];
    if (confirm(`Kick player "${player.nickname}"?`)) {
        socket.send(JSON.stringify({ kickPlayer: index }));
        console.log('TX → Kick player', index);
    }
}

function kickPlayer() {
    // Legacy function - kick active player
    if (!isAdmin || playerList.length === 0) return;
    
    const activeIndex = playerList.findIndex(p => p.active);
    if (activeIndex !== -1) {
        kickPlayerAtIndex(activeIndex);
    }
}

function updateStatus(status, message) {
    connectionStatus = status;
    const statusMsg = document.getElementById('statusMessage');
    
    if (statusMsg) {
        statusMsg.textContent = message;
        
        // Color based on status
        if (status === 'admin') {
            statusMsg.style.color = '#f5576c';
            statusMsg.style.fontWeight = 'bold';
        } else if (status === 'connected') {
            statusMsg.style.color = '#4caf50';
        } else if (status === 'denied') {
            statusMsg.style.color = '#ff6b6b';
        } else {
            statusMsg.style.color = '#666';
        }
    }
}

// =============================================================================
// Joystick Control
// =============================================================================

const joystick = document.getElementById('joystick');
const stick = document.getElementById('stick');
const xValueDisplay = document.getElementById('xValue');
const yValueDisplay = document.getElementById('yValue');

let isDragging = false;
let joystickRect = null;
let centerX = 0;
let centerY = 0;
let maxRadius = 0;

// Initialize joystick dimensions
function updateJoystickDimensions() {
    joystickRect = joystick.getBoundingClientRect();
    centerX = joystickRect.width / 2;
    centerY = joystickRect.height / 2;
    maxRadius = (joystickRect.width / 2) - 40; // Leave space for stick
    resetStick();
}

// Reset stick to center
function resetStick() {
    stick.style.left = '50%';
    stick.style.top = '50%';
    controlState.x = 0;
    controlState.y = 0;
    updateDisplay();
}

// Update stick position based on pointer
function updateStickPosition(clientX, clientY) {
    const rect = joystick.getBoundingClientRect();
    
    // Calculate relative position
    let x = clientX - rect.left - centerX;
    let y = clientY - rect.top - centerY;
    
    // Calculate distance from center
    const distance = Math.sqrt(x * x + y * y);
    
    // Limit to maxRadius
    if (distance > maxRadius) {
        const angle = Math.atan2(y, x);
        x = Math.cos(angle) * maxRadius;
        y = Math.sin(angle) * maxRadius;
    }
    
    // Update stick position
    stick.style.left = (centerX + x) + 'px';
    stick.style.top = (centerY + y) + 'px';
    
    // Update control state (-100 to +100 range)
    controlState.x = Math.round((x / maxRadius) * 100);
    controlState.y = Math.round((-y / maxRadius) * 100); // Invert Y for intuitive up/down
    
    updateDisplay();
}

// Update display values
function updateDisplay() {
    xValueDisplay.textContent = controlState.x;
    yValueDisplay.textContent = controlState.y;
}

// Pointer event handlers for joystick
joystick.addEventListener('pointerdown', (e) => {
    isDragging = true;
    joystick.setPointerCapture(e.pointerId);
    updateStickPosition(e.clientX, e.clientY);
});

joystick.addEventListener('pointermove', (e) => {
    if (isDragging) {
        updateStickPosition(e.clientX, e.clientY);
    }
});

joystick.addEventListener('pointerup', (e) => {
    isDragging = false;
    joystick.releasePointerCapture(e.pointerId);
    resetStick();
});

joystick.addEventListener('pointercancel', (e) => {
    isDragging = false;
    resetStick();
});

// =============================================================================
// Flapper Button Control
// =============================================================================

const flapperBtn = document.getElementById('flapperBtn');
const flapperValueDisplay = document.getElementById('flapperValue');

// Pointer event handlers for flapper button
flapperBtn.addEventListener('pointerdown', (e) => {
    e.preventDefault();
    e.stopPropagation();
    flapperBtn.setPointerCapture(e.pointerId);
    controlState.f = 1;
    flapperValueDisplay.textContent = '1';
    console.log('[FLAPPER] Pressed (pointer)');
    sendControlPacket();  // Send immediately
});

flapperBtn.addEventListener('pointerup', (e) => {
    e.preventDefault();
    e.stopPropagation();
    flapperBtn.releasePointerCapture(e.pointerId);
    controlState.f = 0;
    flapperValueDisplay.textContent = '0';
    console.log('[FLAPPER] Released (pointer)');
    sendControlPacket();  // Send immediately
});

flapperBtn.addEventListener('pointercancel', (e) => {
    controlState.f = 0;
    flapperValueDisplay.textContent = '0';
    console.log('[FLAPPER] Cancelled');
    sendControlPacket();  // Send immediately
});

// Add explicit touch event handlers for better mobile support
flapperBtn.addEventListener('touchstart', (e) => {
    e.preventDefault();
    e.stopPropagation();
    controlState.f = 1;
    flapperValueDisplay.textContent = '1';
    console.log('[FLAPPER] Pressed (touch)');
    sendControlPacket();  // Send immediately
}, { passive: false });

flapperBtn.addEventListener('touchend', (e) => {
    e.preventDefault();
    e.stopPropagation();
    controlState.f = 0;
    flapperValueDisplay.textContent = '0';
    console.log('[FLAPPER] Released (touch)');
    sendControlPacket();  // Send immediately
}, { passive: false });

flapperBtn.addEventListener('touchcancel', (e) => {
    controlState.f = 0;
    flapperValueDisplay.textContent = '0';
    console.log('[FLAPPER] Touch cancelled');
    sendControlPacket();  // Send immediately
});

// Prevent context menu on long press
flapperBtn.addEventListener('contextmenu', (e) => {
    e.preventDefault();
});

// =============================================================================
// Packet Transmission (Timer-Driven at 50 Hz)
// =============================================================================

function sendControlPacket() {
    const packet = {
        x: controlState.x,
        y: controlState.y,
        f: controlState.f
    };
    
    socket.send(JSON.stringify(packet));
}

// Send packets at 50 Hz (20ms interval)
setInterval(sendControlPacket, 20);

// =============================================================================
// Initialize on Load
// =============================================================================

window.addEventListener('load', () => {
    updateJoystickDimensions();
    console.log("%c[INIT] Controller initialized - 50 Hz packet rate", "color: #667eea; font-weight: bold;");
});

// Update dimensions on window resize
window.addEventListener('resize', updateJoystickDimensions);
