# Robo Soccer Bot - SIMPLE VERSION

## Description
Simple player queue version without admin controls. First player to connect gets control. When they disconnect, next player in queue automatically gets control.

## Features
- ✅ Player queue (3 players max: 1 active + 2 waiting)
- ✅ Automatic control handoff when player disconnects
- ✅ Nickname for each player
- ✅ Motor control with exponential turning curve
- ✅ Servo kick mechanism
- ✅ NeoPixel status indicator
- ✅ Battery monitoring
- ✅ Telnet debugging
- ❌ No admin control (everyone equal)
- ❌ No MAC whitelist complexity

## Hardware
- ESP32-C3 DevKit M-1
- DRV8833 motor driver (GPIO 2-5)
- Servo on GPIO 21
- NeoPixel on GPIO 7
- Battery monitor on GPIO 0

## Usage
1. Upload code to ESP32
2. Upload filesystem (data folder)
3. Connect to WiFi: `RoboSoccer` / `12345678`
4. Open browser: `http://192.168.4.1`
5. Enter your nickname
6. First player gets control immediately
7. Others wait in queue

## Telnet Debugging
```
telnet 192.168.4.1
```

## WiFi Credentials
- SSID: `RoboSoccer`
- Password: `12345678`

## How It Works
1. **First Player**: Gets control immediately
2. **Waiting Players**: See their position in queue
3. **Disconnect**: Next player in queue automatically gets control
4. **Full Queue**: New players rejected until someone leaves

## KISS Principle
This version follows Keep It Simple, Stupid:
- No complex MAC address tracking
- No admin/player role confusion
- Simple first-come-first-served queue
- Automatic handoff on disconnect
- Clean and maintainable code

## Status Indicators (NeoPixel)
- 🟢 Green: Battery good (>3.7V)
- 🟡 Yellow: Battery moderate (3.4-3.7V)
- 🔴 Red: Battery low (<3.4V)
- ⚪ White flash: Kick executed
- 🔴 Blinking red: Player disconnected

## Next Steps
If you need admin control later, you can be the first player to connect and stay connected throughout the demo!
