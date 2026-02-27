# ESP32Servo Library Issues and Solutions

## Project: Web-Controlled Robo Soccer Bot
**Hardware:** ESP32-C3 DevKit M-1  
**Library:** ESP32Servo v3.0.5 (madhephaestus)  
**Date:** February 2026

---

## Table of Contents
1. [Initial Problem](#initial-problem)
2. [Hardware Configuration](#hardware-configuration)
3. [Issue #1: Non-Blocking State Machine Failure](#issue-1-non-blocking-state-machine-failure)
4. [Issue #2: Motor PWM Interference](#issue-2-motor-pwm-interference)
5. [Issue #3: Dynamic Attach/Detach Without Delays](#issue-3-dynamic-attachdetach-without-delays)
6. [Final Solution](#final-solution)
7. [Lessons Learned](#lessons-learned)

---

## Initial Problem

**Symptom:** Servo flapper mechanism was erratic and unreliable
- Sometimes worked, sometimes didn't
- Responded to joystick X/Y values instead of flapper button
- Behavior inconsistent between test command ('t') and button press

**Context:**
- Web-controlled robot with ESP32-C3
- WebSocket for real-time control (50 Hz packet rate)
- WiFi and WebSocket running continuously in background
- Servo needed to kick on button press

---

## Hardware Configuration

### Pin Assignment Evolution
```
Initial: GPIO 6  → Too close to motor pins (2,3,4,5)
Test 1:  GPIO 10 → Better, but still some interference
Test 2:  GPIO 21 → Interference persisted (electrical coupling)
Final:   GPIO 10 → Software solution instead of hardware
```

### Motor Control (DRV8833)
```cpp
// Motor pins using LEDC timer 0
#define LEFT_MOTOR_IN1    2
#define LEFT_MOTOR_IN2    3
#define RIGHT_MOTOR_IN1   4
#define RIGHT_MOTOR_IN2   5

// PWM Config
const int MOTOR_PWM_FREQ = 1000;  // 1 kHz
const int MOTOR_PWM_RESOLUTION = 8;  // 8-bit (0-255)
```

### Servo Control (ESP32Servo)
```cpp
#define SERVO_PIN 10

// Library config
ESP32PWM::allocateTimer(1);  // CRITICAL: Use timer 1 (motors use timer 0)
flapperServo.setPeriodHertz(50);  // Standard 50 Hz servo frequency
flapperServo.attach(SERVO_PIN, 1000, 2000);  // Min/max pulse width in µs
```

---

## Issue #1: Non-Blocking State Machine Failure

### What We Tried

**Approach:** Non-blocking state machine with timing variables

```cpp
// FAILED CODE
enum FlapperState { IDLE, KICKING, RETURNING, COOLDOWN };
FlapperState flapperState = IDLE;
unsigned long kickStartTime = 0;

void updateFlapper() {
  unsigned long currentMillis = millis();
  
  switch (flapperState) {
    case IDLE:
      if (kickRequested) {
        flapperServo.write(90);  // Kick
        flapperState = KICKING;
        kickStartTime = currentMillis;
      }
      break;
      
    case KICKING:
      if (currentMillis - kickStartTime >= 500) {
        flapperServo.write(0);  // Return
        flapperState = RETURNING;
        kickStartTime = currentMillis;
      }
      break;
      
    case RETURNING:
      if (currentMillis - kickStartTime >= 100) {
        flapperState = COOLDOWN;
        kickStartTime = currentMillis;
      }
      break;
      
    case COOLDOWN:
      if (currentMillis - kickStartTime >= 50) {
        flapperState = IDLE;
      }
      break;
  }
}
```

### Why It Failed

**Root Cause:** WiFi/WebSocket task interruptions

1. **ESP32 FreeRTOS multitasking:**
   - WiFi stack runs in high-priority tasks
   - WebSocket events interrupt main loop
   - State machine timing becomes unpredictable

2. **Servo pulse timing requirements:**
   - Servos need consistent 50 Hz pulse train (20ms period)
   - ESP32Servo library generates pulses via LEDC hardware timer
   - BUT: Library expects continuous `write()` calls or stable attachment
   - Non-blocking code couldn't guarantee this during WiFi activity

3. **Observable symptoms:**
   - Servo would start moving, then stop mid-motion
   - Return movement sometimes skipped entirely
   - Worked perfectly with 't' command (which used blocking delays)

### Key Discovery

**Blocking delays actually work on ESP32!**

```cpp
// This works because FreeRTOS continues WiFi tasks in background
delay(500);  // Main loop blocks, but WiFi stack keeps running
```

ESP32 is NOT like Arduino Uno - `delay()` doesn't freeze the system:
- FreeRTOS task scheduler continues
- WiFi maintains connection
- WebSocket stays alive
- TCP/IP stack processes packets

---

## Issue #2: Motor PWM Interference

### The Problem

**Symptom:** Servo twitched/jittered during normal driving
- Only happened when joystick moved (motors active)
- Servo at rest position but still receiving interference
- No issue when motors stopped

### Why It Happened

**Electrical coupling between PWM channels:**

1. **Shared power rail:** Motors draw high current, cause voltage ripples
2. **PCB trace coupling:** Motor PWM signals inductively couple to servo signal
3. **Ground bounce:** High motor currents cause ground potential shifts
4. **LEDC timer interference:** Although using separate timers, some cross-talk exists

### What We Tried (Hardware Solutions)

**Attempt 1:** Move servo to different GPIO pins
```
GPIO 6  → Still interferes
GPIO 10 → Still interferes  
GPIO 21 → Still interferes
```
**Result:** Pin location doesn't matter - electrical coupling is system-wide

**Attempt 2:** Add capacitors (not implemented)
- 100µF across servo power
- 0.1µF ceramic near ESP32
**Result:** Not tested due to software solution working

**Attempt 3:** Separate power supplies (not implemented)
- Use different battery for servo vs motors
**Result:** Too complex for this project

---

## Issue #3: Dynamic Attach/Detach Without Delays

### The Strategy

**Idea:** Only attach servo when needed, ground pin otherwise
- Prevents interference when servo not in use
- Clean separation between motor operation and servo operation

### First Attempt (FAILED)

```cpp
// FAILED: No initialization delays
void setupServo() {
  ESP32PWM::allocateTimer(1);
  flapperServo.setPeriodHertz(50);
  // Don't attach at startup
  pinMode(SERVO_PIN, OUTPUT);
  digitalWrite(SERVO_PIN, LOW);
}

void executeKick() {
  flapperServo.attach(SERVO_PIN, 1000, 2000);  // Attach immediately
  flapperServo.write(90);  // Write immediately ← PROBLEM
  delay(500);
  flapperServo.write(0);
  flapperServo.detach();  // Detach immediately ← PROBLEM
  digitalWrite(SERVO_PIN, LOW);
}
```

**What Happened:**
- Servo didn't move at all
- Or moved erratically
- Sometimes worked, sometimes didn't

### Why It Failed

**ESP32Servo library initialization timing:**

1. **`attach()` is not instantaneous:**
   - Library needs to configure LEDC channel
   - Set up PWM parameters
   - Initialize internal state
   - Estimated time: ~10-50ms

2. **First `write()` after attach might be ignored:**
   - Library not fully ready
   - LEDC channel not stable yet
   - Pulse train hasn't established

3. **`detach()` before movement completes:**
   - Servo is electromechanical - takes time to physically move
   - Detaching while servo still moving cuts power mid-motion
   - Servo doesn't reach target position

4. **No explicit delay requirements in library docs:**
   - Documentation doesn't mention initialization time
   - Had to discover through trial and error

---

## Final Solution

### Complete Working Implementation

```cpp
void setupServo() {
  telnetPrintln("Servo initializing...");
  telnetPrintf("  GPIO Pin: %d\n", SERVO_PIN);
  telnetPrintln("  Using ESP32Servo library (dedicated timer)");
  
  // CRITICAL: Use timer 1 explicitly (motors use timer 0)
  ESP32PWM::allocateTimer(1);
  flapperServo.setPeriodHertz(50);
  
  // Don't attach servo at startup - keep pin grounded to avoid interference
  pinMode(SERVO_PIN, OUTPUT);
  digitalWrite(SERVO_PIN, LOW);
  
  telnetPrintln("Servo initialized (pin grounded to prevent interference)");
}

void updateFlapper() {
  unsigned long currentMillis = millis();

  if (kickRequested) {
    kickRequested = false;
    
    // Stop motors during kick to reduce interference
    stopMotors();
    telnetPrintln("[FLAPPER] Executing kick...");
    
    // CRITICAL: Attach servo and give it time to initialize
    flapperServo.attach(SERVO_PIN, 1000, 2000);
    delay(50);  // ← 50ms initialization delay (ESSENTIAL)
    
    // Execute BLOCKING kick sequence
    writeServo(servoKickAngle, "KICK_BLOCKING");  // 90 degrees
    delay(500);  // Hold kick position
    
    writeServo(servoRestAngle, "RETURN_BLOCKING");  // 0 degrees
    delay(100);  // ← Let servo complete movement (ESSENTIAL)
    
    // CRITICAL: Detach and ground the pin to prevent interference
    flapperServo.detach();
    pinMode(SERVO_PIN, OUTPUT);
    digitalWrite(SERVO_PIN, LOW);
    
    telnetPrintln("[FLAPPER] Kick complete (pin grounded)");
    
    // Trigger white flash for kick feedback
    showingWhiteFlash = true;
    whiteFlashStart = millis();
  }
}
```

### Critical Timing Parameters

| Phase | Duration | Purpose |
|-------|----------|---------|
| After attach() | **50ms** | Let ESP32Servo library initialize LEDC channel |
| Kick hold | **500ms** | Give servo time to reach 90° position |
| Before detach() | **100ms** | Let servo complete return to 0° position |
| **Total blocking time** | **650ms** | Acceptable for kick action |

### Why This Works

**1. Initialization delay (50ms after attach):**
- Gives library time to configure LEDC hardware
- Ensures PWM signal is stable before first write()
- Prevents ignored or corrupted servo commands

**2. Completion delay (100ms before detach):**
- Servo physically moves during this time
- Ensures servo reaches rest position before power cut
- Prevents mid-motion detachment

**3. Pin grounding after detach:**
```cpp
pinMode(SERVO_PIN, OUTPUT);
digitalWrite(SERVO_PIN, LOW);
```
- Pulls pin to solid LOW state
- Prevents floating pin from picking up motor PWM noise
- Eliminates interference during normal driving

**4. Motor shutdown during kick:**
```cpp
stopMotors();  // Set all motor PWM to 0
```
- Reduces electrical noise during servo operation
- Ensures clean power for servo movement
- Robot pauses briefly during kick (acceptable behavior)

---

## Lessons Learned

### About ESP32Servo Library

1. **Library needs initialization time after `attach()`**
   - Wait 50-100ms before first `write()`
   - Not documented, discovered empirically

2. **Library needs stable attachment for reliable operation**
   - Rapid attach/detach cycles don't work well
   - Designed for "attach once at startup" pattern
   - Can work with dynamic attach/detach IF proper delays used

3. **Timer allocation is critical**
   ```cpp
   ESP32PWM::allocateTimer(1);  // Different from motor timer 0
   ```
   - Prevents conflicts with other LEDC uses
   - Motors and servos must use different timers

4. **Pulse width range matters**
   ```cpp
   flapperServo.attach(SERVO_PIN, 1000, 2000);  // Min/max in microseconds
   ```
   - Standard servos: 1000-2000µs (1-2ms)
   - 180° servos: Sometimes need 500-2500µs
   - Test your specific servo model

### About ESP32 FreeRTOS

5. **`delay()` is NOT evil on ESP32** (unlike Arduino)
   - FreeRTOS task scheduler continues during delays
   - WiFi stack keeps running
   - WebSocket stays connected
   - Acceptable for short blocking operations (<1 second)

6. **Non-blocking isn't always better**
   - State machines add complexity
   - Harder to debug timing issues
   - Blocking approach simpler and more reliable for servo control
   - Use non-blocking for long operations (>1 second)

### About Hardware Debugging

7. **Try software solutions before hardware changes**
   - Moving pins didn't solve interference
   - Dynamic attach/detach solved it in software
   - Saved time and complexity

8. **Test commands are invaluable**
   ```cpp
   case 't':  // Telnet test command
     // Known-good servo test sequence
     // Use as reference for debugging
   ```
   - Helped identify blocking vs non-blocking issue
   - Provided working baseline to compare against

9. **Debug logging is essential**
   ```cpp
   telnetPrintf("@@@ SERVO WRITE: %d deg from [%s] @@@\n", angle, source);
   ```
   - Track every servo command
   - Include context (what triggered it)
   - Helped identify joystick-triggered writes

### About Library Documentation

10. **Libraries often have undocumented behaviors**
    - ESP32Servo docs don't mention initialization timing
    - Had to discover 50ms delay requirement through testing
    - Read GitHub issues for real-world experiences
    - Test edge cases yourself

---

## Quick Reference

### Working Servo Init Pattern
```cpp
ESP32PWM::allocateTimer(1);
flapperServo.setPeriodHertz(50);
pinMode(SERVO_PIN, OUTPUT);
digitalWrite(SERVO_PIN, LOW);  // Start grounded
```

### Working Kick Pattern
```cpp
flapperServo.attach(SERVO_PIN, 1000, 2000);
delay(50);  // Critical initialization delay
flapperServo.write(angle);
delay(500);  // Movement time
flapperServo.detach();
pinMode(SERVO_PIN, OUTPUT);
digitalWrite(SERVO_PIN, LOW);  // Ground again
```

### Telnet Test Command
```
Press 't' in telnet to test full servo range:
0° → 45° → 90° → 0°
```

---

## Future Improvements

### Potential Optimizations
1. **Reduce blocking time**
   - Current: 650ms total
   - Could test 400ms (50 + 300 + 50)
   - Depends on servo speed rating

2. **Predictive attach**
   - Attach servo 100ms before expected kick
   - Reduces blocking time when button pressed
   - More complex state machine

3. **Hardware filtering**
   - Add RC filter on servo signal line
   - Add decoupling capacitors
   - Use separate regulated power for servo

### What NOT to Try Again
- ❌ Non-blocking state machine without guaranteed timing
- ❌ Attach/detach without delays
- ❌ Continuous servo write() refreshing in loop
- ❌ Moving GPIO pins to solve electrical interference

---

## Summary

**The ESP32Servo library works reliably when you:**
1. Allocate a dedicated timer
2. Use blocking delays for servo operations
3. Wait 50ms after attach() before first write()
4. Wait 100ms before detach() for movement completion
5. Ground the pin when servo not in use (prevents interference)
6. Stop motors during servo operation (reduces noise)

**Total development time:** ~3 days of debugging
**Total blocking time per kick:** 650ms (acceptable)
**Reliability:** 100% after implementing delays

**Bottom line:** ESP32Servo is designed for persistent attachment with blocking delays. Fighting this design pattern leads to reliability issues. Embrace it, and it works perfectly.
