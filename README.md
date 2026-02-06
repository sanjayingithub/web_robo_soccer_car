# Web-Based Robo Soccer Bot Controller

ESP32-based web controller for a 2-wheel differential drive robot with WebSocket control interface.

## Hardware Configuration

### Motors (DRV8833 Driver)
- **Left Motor:**
  - IN1: GPIO 32
  - IN2: GPIO 33
  - PWM: GPIO 25

- **Right Motor:**
  - IN1: GPIO 26
  - IN2: GPIO 27
  - PWM: GPIO 14

### Servo/Flapper
- **Servo Pin:** GPIO 18

## Features

- ✅ WiFi Access Point mode
- ✅ WebSocket-based real-time control
- ✅ Differential drive with tank turning
- ✅ Virtual joystick interface
- ✅ Flapper control with state machine
- ✅ Safety timeout (500ms)
- ✅ Dead zone handling

## Project Structure

```
web_robo_soccer/
├── platformio.ini        # PlatformIO configuration
├── src/
│   └── main.cpp         # ESP32 firmware
├── data/                # Files to upload to LittleFS
│   ├── index.html       # Web interface
│   └── controller.js    # Client-side control logic
└── README.md
```

## Setup Instructions

### 1. Install PlatformIO

If not already installed:
```bash
# VS Code: Install PlatformIO IDE extension
```

### 2. Build Firmware

```bash
# In VS Code: Press Ctrl+Alt+B
# Or use PlatformIO CLI:
pio run
```

### 3. Upload Filesystem (LittleFS)

**IMPORTANT:** Upload the data folder BEFORE uploading firmware.

```bash
# In VS Code: 
# 1. Open PlatformIO Core CLI (terminal)
# 2. Run:
pio run --target uploadfs

# This uploads index.html and controller.js to ESP32's LittleFS
```

### 4. Upload Firmware

```bash
# In VS Code: Press Ctrl+Alt+U
# Or use PlatformIO CLI:
pio run --target upload
```

### 5. Monitor Serial Output

```bash
# In VS Code: PlatformIO → Monitor
# Or:
pio device monitor -b 115200
```

## Usage

### Connecting to the Robot

1. Power on the ESP32
2. Connect to WiFi network:
   - **SSID:** `RoboSoccer`
   - **Password:** `12345678`
3. Open browser and navigate to: `http://192.168.4.1`
4. Use the virtual joystick to control the robot
5. Press the FLAPPER button to activate the kicker

### Control Interface

- **Joystick:** Controls robot movement
  - Y-axis: Forward/backward
  - X-axis: Left/right turning
  - Combined: Arc turns
- **Flapper Button:** Press and hold to kick

### Differential Drive Logic

The robot uses differential steering:
- `left_motor = y + x`
- `right_motor = y - x`

**Examples:**
- Forward: X=0, Y=100
- Tank turn right: X=50, Y=0
- Arc turn: X=30, Y=70

## Troubleshooting

### Can't upload filesystem
```bash
# Make sure no serial monitor is running
# Try:
pio run --target erase
pio run --target uploadfs
```

### WebSocket not connecting
- Check ESP32 serial output for IP address
- Verify you're connected to "RoboSoccer" WiFi
- Clear browser cache and reload

### Motors not responding
- Check motor driver connections
- Verify power supply to motors
- Check serial monitor for control packet receipt

### Flapper not working
- Verify servo is connected to GPIO 18
- Check servo power supply (needs 5V)
- Monitor serial for "KICK START" messages

## Configuration

### Change WiFi Credentials

Edit in `src/main.cpp`:
```cpp
const char* ssid = "RoboSoccer";
const char* password = "12345678";
```

### Adjust Motor Pins

Edit pin definitions in `src/main.cpp`:
```cpp
#define LEFT_MOTOR_IN1 32
#define LEFT_MOTOR_IN2 33
// ... etc
```

### Tune Servo Kick Angle

Edit in `src/main.cpp`:
```cpp
const int kickAngle = 50;  // Adjust 0-180
```

### Adjust Dead Zone

Edit in `src/main.cpp`:
```cpp
const int DEAD_ZONE = 5;  // ±5 threshold
```

## Development

### Testing Without Hardware

1. Use the PC test version in `pc_web_test/` folder
2. Open `index.html` in browser
3. Check console for packet output
4. Verify joystick and button behavior

### Modifying Web Interface

1. Edit files in `data/` folder
2. Re-upload filesystem: `pio run --target uploadfs`
3. Hard refresh browser (Ctrl+F5)

## Technical Details

- **Update Rate:** 50 Hz (20ms loop)
- **WebSocket Endpoint:** `/ws`
- **Control Packet Format:** `{"x":-50,"y":80,"f":0}`
- **Timeout:** 500ms (motors stop if no packets received)
- **PWM Frequency:** 1kHz (motors), 50Hz (servo)

## License

Based on DroneBot Workshop mecanum car code, adapted for differential drive with web interface.
