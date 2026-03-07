// WebSocket connection
let ws;
let hasControl = false;
let myPosition = 0;
let queueSize = 0;

// Motor lock state
let motorsLocked = false;

// Joystick state
let joyX = 0;
let joyY = 0;
let kickState = 0;

// Elements
const joystick = document.getElementById('joystick');
const stick = document.getElementById('stick');
const kickButton = document.getElementById('kickButton');
const coords = document.getElementById('coords');
const statusMessage = document.getElementById('statusMessage');
const queueInfo = document.getElementById('queueInfo');
const nicknamePrompt = document.getElementById('nicknamePrompt');
const nicknameInput = document.getElementById('nicknameInput');

// Connect to WebSocket
function connectWebSocket() {
    ws = new WebSocket(`ws://${window.location.hostname}/ws`);
    
    ws.onopen = () => {
        console.log('Connected');
        updateStatus('connecting', 'Connected to bot...');
    };
    
    ws.onmessage = (event) => {
        try {
            const data = JSON.parse(event.data);
            handleMessage(data);
        } catch (e) {
            console.error('Parse error:', e);
        }
    };
    
    ws.onerror = (error) => {
        console.error('WebSocket error:', error);
        updateStatus('denied', 'Connection error');
    };
    
    ws.onclose = () => {
        console.log('Disconnected');
        updateStatus('denied', 'Disconnected from bot');
        hasControl = false;
        kickButton.disabled = true;
        setTimeout(connectWebSocket, 3000);  // Reconnect after 3 seconds
    };
}

// Handle incoming messages
function handleMessage(data) {
    if (data.type === 'requestNickname') {
        nicknamePrompt.style.display = 'flex';
        nicknameInput.focus();
    }
    else if (data.type === 'controlStatus') {
        hasControl = data.hasControl;
        myPosition = data.position;
        queueSize = data.queueSize;
        
        // Always update control status display
        if (hasControl) {
            updateStatus('active', '✓ You have control');
            kickButton.disabled = false;
        } else {
            updateStatus('waiting', `⏳ Waiting... (Position ${myPosition}/${queueSize})`);
            kickButton.disabled = true;
        }
        
        updateQueueInfo();
    }
    else if (data.type === 'adminMessage') {
        // Motor lock/unlock - silent (LED indicator is enough)
        if (data.message.includes('locked')) {
            motorsLocked = true;
            kickButton.disabled = false;  // Kick still works
            // No message display - LED shows lock status
        } else if (data.message.includes('unlocked')) {
            motorsLocked = false;
            // No message display - LED shows unlock status
            if (hasControl) {
                kickButton.disabled = false;
            } else {
                kickButton.disabled = true;
            }
        } else {
            // Other admin messages - show temporarily
            const currentStatus = statusMessage.textContent;
            const currentClass = statusMessage.className;
            updateStatus('denied', data.message);
            setTimeout(() => {
                statusMessage.textContent = currentStatus;
                statusMessage.className = currentClass;
            }, 3000);
        }
    }
    else if (data.type === 'kicked') {
        // Player was kicked by admin
        updateStatus('denied', data.message);
        kickButton.disabled = true;
        alert(data.message);
    }
    else if (data.status === 'denied') {
        updateStatus('denied', `⛔ ${data.message}`);
        kickButton.disabled = true;
    }
}

// Submit nickname
function submitNickname() {
    const nickname = nicknameInput.value.trim();
    if (nickname.length > 0) {
        ws.send(JSON.stringify({ nickname: nickname }));
        nicknamePrompt.style.display = 'none';
    }
}

// Allow Enter key to submit nickname
nicknameInput.addEventListener('keypress', (e) => {
    if (e.key === 'Enter') {
        submitNickname();
    }
});

// Update status display
function updateStatus(type, message) {
    statusMessage.className = `status ${type}`;
    statusMessage.textContent = message;
}

// Update queue info
function updateQueueInfo() {
    if (myPosition > 0) {
        queueInfo.textContent = `Position: ${myPosition} of ${queueSize} players`;
    } else {
        queueInfo.textContent = '';
    }
}

// Joystick handling
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

// Initialize on load
window.addEventListener('load', updateJoystickDimensions);
window.addEventListener('resize', updateJoystickDimensions);

// Update stick position based on pointer (calculates joyX/joyY state)
function updateStickPosition(clientX, clientY) {
    const rect = joystick.getBoundingClientRect();
    
    // Recalculate center in case of resize
    const cx = rect.width / 2;
    const cy = rect.height / 2;
    const maxR = (rect.width / 2) - 40;
    
    // Calculate relative position
    let x = clientX - rect.left - cx;
    let y = clientY - rect.top - cy;
    
    // Calculate distance from center
    const distance = Math.sqrt(x * x + y * y);
    
    // Limit to maxRadius
    if (distance > maxR) {
        const angle = Math.atan2(y, x);
        x = Math.cos(angle) * maxR;
        y = Math.sin(angle) * maxR;
    }
    
    // Update control state (-100 to +100 range) - NO VISUAL UPDATE HERE
    joyX = Math.round((x / maxR) * 100);
    joyY = Math.round((-y / maxR) * 100); // Invert Y for intuitive up/down
}

// Update visual stick position based on joyX/joyY state (called by animation loop)
function updateStickVisual() {
    if (joyX === 0 && joyY === 0) {
        // Centered - use percentage
        stick.style.left = '50%';
        stick.style.top = '50%';
    } else {
        // Convert joyX/joyY (-100 to +100) back to pixel position
        const x = (joyX / 100) * maxRadius;
        const y = (-joyY / 100) * maxRadius; // Un-invert Y
        
        stick.style.left = (centerX + x) + 'px';
        stick.style.top = (centerY + y) + 'px';
    }
    
    coords.textContent = `X: ${joyX}, Y: ${joyY}`;
}

function resetStick() {
    // Use percentage for center reset (important!)
    stick.style.left = '50%';
    stick.style.top = '50%';
    coords.textContent = 'X: 0, Y: 0';
}

// Kick button
kickButton.addEventListener('touchstart', (e) => {
    if (!hasControl) return;
    e.preventDefault();
    kickState = 1;
});

kickButton.addEventListener('touchend', (e) => {
    if (!hasControl) return;
    e.preventDefault();
    kickState = 0;
});

kickButton.addEventListener('mousedown', (e) => {
    if (!hasControl) return;
    e.preventDefault();
    kickState = 1;
});

kickButton.addEventListener('mouseup', (e) => {
    if (!hasControl) return;
    e.preventDefault();
    kickState = 0;
});

// Attach joystick event listeners
// Use explicit touch and mouse events for better mobile compatibility
joystick.addEventListener('touchstart', (e) => {
    if (!hasControl) return;
    e.preventDefault();
    isDragging = true;
    const touch = e.touches[0];
    updateStickPosition(touch.clientX, touch.clientY);
}, { passive: false });

joystick.addEventListener('touchmove', (e) => {
    if (!isDragging || !hasControl) return;
    e.preventDefault();
    const touch = e.touches[0];
    updateStickPosition(touch.clientX, touch.clientY);
}, { passive: false });

joystick.addEventListener('touchend', (e) => {
    if (!hasControl) return;
    e.preventDefault();
    isDragging = false;
    joyX = 0;
    joyY = 0;
    // Visual update handled by animation loop
}, { passive: false });

joystick.addEventListener('touchcancel', (e) => {
    isDragging = false;
    joyX = 0;
    joyY = 0;
    // Visual update handled by animation loop
});

// Mouse events for desktop
joystick.addEventListener('mousedown', (e) => {
    if (!hasControl) return;
    e.preventDefault();
    isDragging = true;
    updateStickPosition(e.clientX, e.clientY);
});

joystick.addEventListener('mousemove', (e) => {
    if (!isDragging || !hasControl) return;
    e.preventDefault();
    updateStickPosition(e.clientX, e.clientY);
});

joystick.addEventListener('mouseup', (e) => {
    if (!hasControl) return;
    e.preventDefault();
    isDragging = false;
    joyX = 0;
    joyY = 0;
    // Visual update handled by animation loop
});

joystick.addEventListener('mouseleave', (e) => {
    if (isDragging) {
        isDragging = false;
        joyX = 0;
        joyY = 0;
        // Visual update handled by animation loop
    }
});

// Initialize stick position
resetStick();

// High-speed control loop - 50 Hz (20ms interval)
setInterval(() => {
    // Update visual stick position based on state
    updateStickVisual();
    
    // Send control packet
    if (hasControl && ws && ws.readyState === WebSocket.OPEN) {
        const data = {
            x: joyX,
            y: joyY,
            f: kickState
        };
        ws.send(JSON.stringify(data));
    }
}, 20);

// Connect on page load
connectWebSocket();
