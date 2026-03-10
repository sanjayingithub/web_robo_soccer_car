#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Adafruit_NeoPixel.h>
#include <ESP32Servo.h>
#include <ESP32PWM.h>
#include <esp_task_wdt.h>
#include <cmath>

// =============================================================================
// Telnet Server Configuration
// =============================================================================
WiFiServer telnetServer(23);
WiFiClient telnetClient;
bool telnetConnected = false;
bool telnetAuthenticated = false;  // Require password for admin commands
String telnetCommandBuffer = "";
const char* TELNET_PASSWORD = "29A";  // Admin password

// =============================================================================
// WiFi Access Point Configuration
// =============================================================================
#ifdef BOT_PACHAVANDI
  const char* ssid = "Pachavandi";
  const char* password = "EdaMoneHappyAlle";
#elif BOT_NEELAVANDI
  const char* ssid = "Neelavandi";
  const char* password = "EdaMoneHappyAlle";
#else
  #error "No bot defined! Select 'pachavandi' or 'neelavandi' environment in PlatformIO"
#endif

// =============================================================================
// Player Queue Management (SIMPLE - No Admin)
// =============================================================================
const int MAX_PLAYERS = 3;  // 1 active + 2 waiting

struct PlayerInfo {
  uint32_t clientId;
  String nickname;
  String ipAddress;
  bool isActive;
};

PlayerInfo playerQueue[MAX_PLAYERS];
int playerCount = 0;

// =============================================================================
// Pin Definitions
// =============================================================================
// Motors (DRV8833)
#define LEFT_MOTOR_IN1 2
#define LEFT_MOTOR_IN2 3
#define RIGHT_MOTOR_IN1 4
#define RIGHT_MOTOR_IN2 5

// Servo
#define SERVO_PIN 21

// NeoPixel and Battery
#define NEOPIXEL_PIN 7
#define BATTERY_ADC_PIN 0
#define NUM_PIXELS 1

// =============================================================================
// PWM Configuration
// =============================================================================
const int LEFT_MOTOR_IN1_CHANNEL = 0;
const int LEFT_MOTOR_IN2_CHANNEL = 1;
const int RIGHT_MOTOR_IN1_CHANNEL = 2;
const int RIGHT_MOTOR_IN2_CHANNEL = 3;
const int MOTOR_PWM_FREQ = 1000;
const int MOTOR_PWM_RESOLUTION = 8;

// Motor Calibration (adjust these to balance motor speeds)
// Values: 0.5 to 1.0 (1.0 = full speed, 0.8 = 80% speed, etc.)
float LEFT_MOTOR_CALIBRATION = 1.0;   // Adjust if left motor too fast/slow
float RIGHT_MOTOR_CALIBRATION = 1.0;  // Adjust if right motor too fast/slow

// Servo angles - Bot-specific calibration
#ifdef BOT_PACHAVANDI
  const int servoRestAngle = 60;    // STOP position (neutral)
  const int servoKickAngle = 230;   // Spin direction for kick
#elif BOT_NEELAVANDI
  const int servoRestAngle = 55;    // STOP position (neutral)
  const int servoKickAngle = 230;   // Spin direction for kick
#endif

Servo flapperServo;

// =============================================================================
// Control State
// =============================================================================
volatile int joyX = 0;
volatile int joyY = 0;
volatile bool kickRequested = false;

const int DEAD_ZONE = 5;
#define CONTROL_TIMEOUT 500
unsigned long lastControlTime = 0;

// =============================================================================
// NeoPixel
// =============================================================================
Adafruit_NeoPixel neopixel(NUM_PIXELS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
const uint8_t NEOPIXEL_BRIGHTNESS = 20;

float batteryVoltage = 0.0;
unsigned long lastBatteryRead = 0;
const unsigned long BATTERY_READ_INTERVAL = 10000;

// Voltage Divider Configuration - Bot-specific calibration
#ifdef BOT_PACHAVANDI
  // R1=9.88kΩ, R2=9.96kΩ (Calibrated: 1.791)
  const float VOLTAGE_DIVIDER_RATIO = 1.791;
#elif BOT_NEELAVANDI
  // R1=9.85kΩ, R2=9.91kΩ (Calibrated: 1.749)
  const float VOLTAGE_DIVIDER_RATIO = 1.749;
#endif

const float ADC_REFERENCE_VOLTAGE = 3.3;
const int ADC_RESOLUTION = 4095;

// NeoPixel indicator states
bool showingWhiteFlash = false;
unsigned long whiteFlashStart = 0;
const long WHITE_FLASH_DURATION = 200;

unsigned long lastDisconnectTime = 0;
bool blinkState = false;
unsigned long lastBlinkToggle = 0;

// Player connect fade effect
bool showingConnectFade = false;
unsigned long connectFadeStart = 0;
const long CONNECT_FADE_DURATION = 500;

// Admin motor lock
bool motorsLockedByAdmin = false;

// LED mode: false = battery indicator, true = vehicle ID color
bool ledGreenMode = false;

// =============================================================================
// Web Server
// =============================================================================
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// =============================================================================
// Function Prototypes
// =============================================================================
void setupMotors();
void setupServo();
void setupWiFi();
void setupWebServer();
void setupNeoPixel();
void setupTelnet();
void handleTelnet();
void handleWebSocketMessage(void *arg, uint8_t *data, size_t len, AsyncWebSocketClient *client);
void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, 
                      AwsEventType type, void *arg, uint8_t *data, size_t len);
void updateMotors();
void setMotorSpeed(int leftSpeed, int rightSpeed);
void stopMotors();
void updateFlapper();
void handleControlTimeout();
void readBatteryVoltage();
void updateNeoPixel();
void telnetPrint(const String& message);
void telnetPrintln(const String& message);
void telnetPrintf(const char* format, ...);
int findPlayerIndex(uint32_t clientId);
int getActivePlayerIndex();
void setActivePlayer(int index);
void removePlayer(uint32_t clientId);
void broadcastPlayerList();

// =============================================================================
// Telnet Helpers
// =============================================================================
void telnetPrint(const String& message) {
  Serial.print(message);
  if (telnetConnected && telnetClient && telnetClient.connected()) {
    telnetClient.print(message);
  }
}

void telnetPrintln(const String& message) {
  Serial.println(message);
  if (telnetConnected && telnetClient && telnetClient.connected()) {
    telnetClient.print(message);
    telnetClient.print("\r\n");
  }
}

void telnetPrintf(const char* format, ...) {
  char buffer[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  
  Serial.print(buffer);
  if (telnetConnected && telnetClient && telnetClient.connected()) {
    String output = String(buffer);
    output.replace("\n", "\r\n");
    telnetClient.print(output);
  }
}

// =============================================================================
// Motor Control
// =============================================================================
void setupMotors() {
  // Setup ALL PWM channels with IDENTICAL settings first
  for (int ch = 0; ch <= 3; ch++) {
    ledcSetup(ch, MOTOR_PWM_FREQ, MOTOR_PWM_RESOLUTION);
  }
  delay(10);  // Let LEDC stabilize
  
  // Attach pins in symmetrical order
  ledcAttachPin(LEFT_MOTOR_IN1, LEFT_MOTOR_IN1_CHANNEL);
  ledcAttachPin(RIGHT_MOTOR_IN1, RIGHT_MOTOR_IN1_CHANNEL);
  delay(5);
  ledcAttachPin(LEFT_MOTOR_IN2, LEFT_MOTOR_IN2_CHANNEL);
  ledcAttachPin(RIGHT_MOTOR_IN2, RIGHT_MOTOR_IN2_CHANNEL);
  delay(10);
  
  // Ensure clean stop state
  stopMotors();
  delay(50);
  
  // CRITICAL FIX: Reserve motor PWM channels in ESP32PWM library allocation table
  // 
  // Problem: ESP32Servo library uses ESP32PWM for channel allocation. When servo.attach()
  // is called, ESP32PWM::allocatenext() searches for free channels. Without this fix,
  // the library doesn't know channels 0-3 are manually allocated via ledcSetup(), so it
  // may reallocate them for the servo, causing LEDC timer conflicts and motor speed
  // asymmetry after servo operations.
  // 
  // Solution: Mark channels 0-3 as "occupied" by writing non-null pointers to the
  // ESP32PWM::ChannelUsed[] array. This forces the servo library to use channels 4-5
  // (Timer 2) instead, maintaining hardware isolation between motors and servo.
  // 
  // Why 0x1? Any non-null pointer value signals "occupied". Using 0x1 (fake pointer)
  // instead of actual object address prevents the library from trying to dereference it.
  ESP32PWM::ChannelUsed[LEFT_MOTOR_IN1_CHANNEL] = (ESP32PWM*)0x1;  // Fake pointer = occupied
  ESP32PWM::ChannelUsed[LEFT_MOTOR_IN2_CHANNEL] = (ESP32PWM*)0x1;
  ESP32PWM::ChannelUsed[RIGHT_MOTOR_IN1_CHANNEL] = (ESP32PWM*)0x1;
  ESP32PWM::ChannelUsed[RIGHT_MOTOR_IN2_CHANNEL] = (ESP32PWM*)0x1;
  ESP32PWM::PWMCount += 4;  // Increment library's allocation counter
  
  telnetPrintln("Motors initialized (symmetrical LEDC)");
  telnetPrintf("  Left: GPIO %d/%d (CH%d/%d)\n", LEFT_MOTOR_IN1, LEFT_MOTOR_IN2, 
               LEFT_MOTOR_IN1_CHANNEL, LEFT_MOTOR_IN2_CHANNEL);
  telnetPrintf("  Right: GPIO %d/%d (CH%d/%d)\n", RIGHT_MOTOR_IN1, RIGHT_MOTOR_IN2,
               RIGHT_MOTOR_IN1_CHANNEL, RIGHT_MOTOR_IN2_CHANNEL);
  telnetPrintln("  Channels 0-3 reserved (servo will use 4-5)");
}

void setMotorSpeed(int leftSpeed, int rightSpeed) {
  // Check if motors are locked by admin
  if (motorsLockedByAdmin) {
    stopMotors();
    return;
  }
  
  leftSpeed = constrain(leftSpeed, -100, 100);
  rightSpeed = constrain(rightSpeed, -100, 100);

  if (abs(leftSpeed) < DEAD_ZONE) leftSpeed = 0;
  if (abs(rightSpeed) < DEAD_ZONE) rightSpeed = 0;

  // Apply calibration to balance motor speeds
  int leftPWM = map(abs(leftSpeed), 0, 100, 0, 255);
  int rightPWM = map(abs(rightSpeed), 0, 100, 0, 255);
  
  leftPWM = (int)(leftPWM * LEFT_MOTOR_CALIBRATION);
  rightPWM = (int)(rightPWM * RIGHT_MOTOR_CALIBRATION);

  // Debug output - show actual PWM values
  static unsigned long lastDebug = 0;
  if (millis() - lastDebug > 1000 && (leftPWM > 0 || rightPWM > 0)) {
    telnetPrintf("[MOTOR_PWM] L=%d, R=%d (from speeds L=%d, R=%d) | Battery: %.2fV\n", 
                 leftPWM, rightPWM, leftSpeed, rightSpeed, batteryVoltage);
    lastDebug = millis();
  }

  // CRITICAL FIX: Atomic PWM updates to prevent motor speed asymmetry
  //
  // Problem: ESP32-C3 is single-core with WiFi on same core. WiFi stack generates frequent
  // interrupts (50-200μs) for packet handling. If WiFi interrupts occur between left and
  // right motor PWM updates, it creates temporal offset where one motor receives new command
  // before the other. Over many loop iterations (50Hz), these microsecond delays accumulate
  // into noticeable speed differences.
  //
  // Solution: Disable interrupts during all four motor register writes. This ensures both
  // motors receive synchronized commands. The LEDC hardware peripheral continues generating
  // PWM waveforms unaffected - only the register update timing is protected.
  //
  // Note: This is needed despite LEDC being hardware-independent because we're protecting
  // the software register writes, not the PWM generation itself.
  noInterrupts();
  
  // Left motor
  if (leftSpeed > 0) {
    ledcWrite(LEFT_MOTOR_IN1_CHANNEL, leftPWM);
    ledcWrite(LEFT_MOTOR_IN2_CHANNEL, 0);
  } else if (leftSpeed < 0) {
    ledcWrite(LEFT_MOTOR_IN1_CHANNEL, 0);
    ledcWrite(LEFT_MOTOR_IN2_CHANNEL, leftPWM);
  } else {
    ledcWrite(LEFT_MOTOR_IN1_CHANNEL, 0);
    ledcWrite(LEFT_MOTOR_IN2_CHANNEL, 0);
  }

  // Right motor
  if (rightSpeed > 0) {
    ledcWrite(RIGHT_MOTOR_IN1_CHANNEL, rightPWM);
    ledcWrite(RIGHT_MOTOR_IN2_CHANNEL, 0);
  } else if (rightSpeed < 0) {
    ledcWrite(RIGHT_MOTOR_IN1_CHANNEL, 0);
    ledcWrite(RIGHT_MOTOR_IN2_CHANNEL, rightPWM);
  } else {
    ledcWrite(RIGHT_MOTOR_IN1_CHANNEL, 0);
    ledcWrite(RIGHT_MOTOR_IN2_CHANNEL, 0);
  }
  
  interrupts();  // Re-enable interrupts after atomic write
}

void stopMotors() {
  ledcWrite(LEFT_MOTOR_IN1_CHANNEL, 0);
  ledcWrite(LEFT_MOTOR_IN2_CHANNEL, 0);
  ledcWrite(RIGHT_MOTOR_IN1_CHANNEL, 0);
  ledcWrite(RIGHT_MOTOR_IN2_CHANNEL, 0);
}

int applyExponentialCurve(int value, float exponent) {
  if (value == 0) return 0;
  float normalized = value / 100.0;
  normalized = constrain(normalized, -1.0, 1.0);
  float sign = (normalized >= 0) ? 1.0 : -1.0;
  float curved = sign * pow(fabs(normalized), exponent);
  int result = (int)(curved * 100.0);
  return constrain(result, -100, 100);
}

void updateMotors() {
  int curvedX = applyExponentialCurve(joyX, 2.5);
  int leftSpeed = joyY + curvedX;
  int rightSpeed = joyY - curvedX;
  setMotorSpeed(leftSpeed, rightSpeed);
}

// =============================================================================
// Servo Control
// =============================================================================
void setupServo() {
  telnetPrintln("Servo initializing");
  flapperServo.setPeriodHertz(50);
  pinMode(SERVO_PIN, OUTPUT);
  digitalWrite(SERVO_PIN, LOW);
  telnetPrintln("Servo initialized (pin grounded)");
}

void updateFlapper() {
  if (kickRequested) {
    kickRequested = false;
    
    // White flash feedback
    showingWhiteFlash = true;
    whiteFlashStart = millis();
    neopixel.setPixelColor(0, neopixel.Color(NEOPIXEL_BRIGHTNESS, NEOPIXEL_BRIGHTNESS, NEOPIXEL_BRIGHTNESS));
    neopixel.show();
    
    // Stop motors during servo operation for two reasons:
    // 1. Power: Servo draws high inrush current (500mA-1A), combined with motors could
    //    cause voltage sag and brownout reset
    // 2. Blocking delays: The delay() calls below freeze the entire loop, preventing
    //    updateMotors() from running and making the robot unresponsive to joystick input
    // Total kick duration: 275ms (barely noticeable to user)
    stopMotors();
    telnetPrintln("[KICK]");
    
    flapperServo.attach(SERVO_PIN, 1000, 2000);
    delay(50);
    
    flapperServo.write(servoRestAngle);
    delay(50);
    
    flapperServo.write(servoKickAngle);
    delay(175);  // Main kick action
    
    flapperServo.write(servoRestAngle);
    delay(50);
    
    flapperServo.detach();
    pinMode(SERVO_PIN, OUTPUT);
    digitalWrite(SERVO_PIN, LOW);
    
    telnetPrintln("[KICK] Complete");
  }
}

// =============================================================================
// Player Queue Management
// =============================================================================
int findPlayerIndex(uint32_t clientId) {
  for (int i = 0; i < playerCount; i++) {
    if (playerQueue[i].clientId == clientId) return i;
  }
  return -1;
}

int getActivePlayerIndex() {
  for (int i = 0; i < playerCount; i++) {
    if (playerQueue[i].isActive) return i;
  }
  return -1;
}

void setActivePlayer(int index) {
  // Stop motors and reset joystick when switching active player
  // This prevents motors from stuck running with previous player's commands
  stopMotors();
  joyX = 0;
  joyY = 0;
  lastControlTime = millis();
  
  for (int i = 0; i < playerCount; i++) {
    playerQueue[i].isActive = false;
  }
  if (index >= 0 && index < playerCount) {
    playerQueue[index].isActive = true;
    telnetPrintf("Active: %s\n", playerQueue[index].nickname.c_str());
    broadcastPlayerList();
  }
}

void removePlayer(uint32_t clientId) {
  int index = findPlayerIndex(clientId);
  if (index == -1) return;
  
  bool wasActive = playerQueue[index].isActive;
  
  for (int i = index; i < playerCount - 1; i++) {
    playerQueue[i] = playerQueue[i + 1];
  }
  playerCount--;
  
  if (wasActive && playerCount > 0) {
    setActivePlayer(0);
  }
  
  broadcastPlayerList();
}

void broadcastPlayerList() {
  StaticJsonDocument<512> doc;
  doc["type"] = "playerList";
  doc["count"] = playerCount;
  
  JsonArray players = doc.createNestedArray("players");
  for (int i = 0; i < playerCount; i++) {
    JsonObject player = players.createNestedObject();
    player["nickname"] = playerQueue[i].nickname;
    player["ip"] = playerQueue[i].ipAddress;
    player["active"] = playerQueue[i].isActive;
    player["index"] = i;
  }
  
  String output;
  serializeJson(doc, output);
  ws.textAll(output);
  
  // Send control status to each player
  for (int i = 0; i < playerCount; i++) {
    AsyncWebSocketClient *client = ws.client(playerQueue[i].clientId);
    if (client) {
      StaticJsonDocument<200> statusDoc;
      statusDoc["type"] = "controlStatus";
      statusDoc["hasControl"] = playerQueue[i].isActive;
      statusDoc["position"] = i + 1;
      statusDoc["queueSize"] = playerCount;
      
      String statusOutput;
      serializeJson(statusDoc, statusOutput);
      client->text(statusOutput);
    }
  }
}

// =============================================================================
// WiFi and WebSocket
// =============================================================================
void setupWiFi() {
  telnetPrintln("\n=== Starting WiFi AP ===");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);
  IPAddress IP = WiFi.softAPIP();
  telnetPrint("AP IP: ");
  telnetPrintln(IP.toString());
  telnetPrint("SSID: ");
  telnetPrintln(ssid);
  telnetPrintln("========================");
}

void handleWebSocketMessage(void *arg, uint8_t *data, size_t len, AsyncWebSocketClient *client) {
  AwsFrameInfo *info = (AwsFrameInfo*)arg;
  
  if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
    data[len] = 0;
    
    StaticJsonDocument<300> doc;
    DeserializationError error = deserializeJson(doc, (char*)data);
    
    if (error) return;

    // Nickname registration
    if (doc.containsKey("nickname")) {
      String nickname = doc["nickname"].as<String>();
      nickname.trim();
      if (nickname.length() > 10) nickname = nickname.substring(0, 10);
      
      int playerIndex = findPlayerIndex(client->id());
      if (playerIndex != -1) {
        playerQueue[playerIndex].nickname = nickname;
        telnetPrintf("Player registered: %s (pos %d/%d)\n", nickname.c_str(), playerIndex + 1, MAX_PLAYERS);
        broadcastPlayerList();
      }
      return;
    }

    // Control commands - only from active player
    int playerIndex = findPlayerIndex(client->id());
    if (playerIndex == -1 || !playerQueue[playerIndex].isActive) {
      return;  // Ignore commands from waiting players
    }

    // Extract control values
    int newX = doc["x"] | 0;
    int newY = doc["y"] | 0;
    bool newKick = (doc["f"] | 0) != 0;

    joyX = newX;
    joyY = newY;
    
    // Edge detection - only trigger on rising edge (button press)
    // This prevents kick spamming: button held = one kick, not continuous kicks
    static bool lastKickState = false;
    if (newKick && !lastKickState) {
      kickRequested = true;
      telnetPrintln("[KICK] Button pressed");
    }
    lastKickState = newKick;
    
    lastControlTime = millis();
  }
}

void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                      AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT: {
      if (playerCount >= MAX_PLAYERS) {
        telnetPrintln("Queue full - rejecting connection");
        
        StaticJsonDocument<100> doc;
        doc["status"] = "denied";
        doc["message"] = "Queue full. Try again later.";
        String output;
        serializeJson(doc, output);
        client->text(output);
        
        client->close();
        return;
      }
      
      // Add to queue
      playerQueue[playerCount].clientId = client->id();
      playerQueue[playerCount].nickname = "Player" + String(playerCount + 1);
      playerQueue[playerCount].ipAddress = client->remoteIP().toString();
      playerQueue[playerCount].isActive = (playerCount == 0);  // First player is active
      playerCount++;
      
      telnetPrintf("Player joined: pos %d/%d, IP %s\n", playerCount, MAX_PLAYERS, client->remoteIP().toString().c_str());
      
      // Trigger player connect fade effect
      showingConnectFade = true;
      connectFadeStart = millis();
      
      // Request nickname
      StaticJsonDocument<100> doc;
      doc["type"] = "requestNickname";
      String output;
      serializeJson(doc, output);
      client->text(output);
      
      broadcastPlayerList();
      break;
    }
      
    case WS_EVT_DISCONNECT: {
      telnetPrintf("Player disconnected: ID %u\n", client->id());
      removePlayer(client->id());
      
      // Always reset motors/joystick on disconnect to prevent stuck commands
      stopMotors();
      joyX = 0;
      joyY = 0;
      
      if (playerCount == 0) {
        lastDisconnectTime = millis();
      }
      break;
    }
      
    case WS_EVT_DATA:
      handleWebSocketMessage(arg, data, len, client);
      break;
      
    default:
      break;
  }
}

void setupWebServer() {
  if (!LittleFS.begin(true)) {
    telnetPrintln("LittleFS mount failed");
    return;
  }
  telnetPrintln("LittleFS mounted");

  ws.onEvent(onWebSocketEvent);
  server.addHandler(&ws);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/index.html", "text/html");
  });

  server.on("/controller.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/controller.js", "application/javascript");
  });

  server.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
  });

  server.begin();
  telnetPrintln("Web server started");
}

void handleControlTimeout() {
  if (millis() - lastControlTime > CONTROL_TIMEOUT) {
    stopMotors();
    joyX = 0;
    joyY = 0;
  }
}

// =============================================================================
// NeoPixel and Battery
// =============================================================================
void setupNeoPixel() {
  neopixel.begin();
  neopixel.setBrightness(255);
  neopixel.clear();
  neopixel.show();
  analogSetAttenuation(ADC_11db);
  telnetPrintln("NeoPixel initialized");
}

void readBatteryVoltage() {
  if (millis() - lastBatteryRead >= BATTERY_READ_INTERVAL) {
    int adcSum = 0;
    for (int i = 0; i < 10; i++) {
      adcSum += analogRead(BATTERY_ADC_PIN);
      delay(1);
    }
    int adcValue = adcSum / 10;
    float adcVoltage = (adcValue * ADC_REFERENCE_VOLTAGE) / ADC_RESOLUTION;
    batteryVoltage = adcVoltage * VOLTAGE_DIVIDER_RATIO;
    lastBatteryRead = millis();
  }
}

void updateNeoPixel() {
  unsigned long currentMillis = millis();
  
  // Priority-based LED indicator system:
  // Higher priority states take precedence over lower ones. System checks from
  // priority 1 (highest) to 5 (lowest) and displays first active state.
  
  // PRIORITY 1: Motor lock by admin - RED SOLID
  // Indicates motors are disabled by admin command, robot won't respond to controls
  if (motorsLockedByAdmin) {
    neopixel.setPixelColor(0, neopixel.Color(NEOPIXEL_BRIGHTNESS, 0, 0));  // RED
    neopixel.show();
    return;
  }
  
  // PRIORITY 2: Kick active - WHITE FLASH (200ms)
  // Brief flash provides immediate feedback for kick button press
  if (showingWhiteFlash && currentMillis - whiteFlashStart >= WHITE_FLASH_DURATION) {
    showingWhiteFlash = false;
  }
  
  if (showingWhiteFlash) {
    neopixel.setPixelColor(0, neopixel.Color(NEOPIXEL_BRIGHTNESS, NEOPIXEL_BRIGHTNESS, NEOPIXEL_BRIGHTNESS));
    neopixel.show();
    return;
  }
  
  // PRIORITY 3: Player disconnect - PURPLE BLINK (2 seconds, 200ms on/off)
  // Shows when last player disconnects, alerts admin that robot is idle
  if (currentMillis - lastDisconnectTime < 2000) {
    if (currentMillis - lastBlinkToggle >= 200) {
      blinkState = !blinkState;
      lastBlinkToggle = currentMillis;
      if (blinkState) {
        // PURPLE (128, 0, 128) scaled to brightness
        uint8_t purple = map(128, 0, 255, 0, NEOPIXEL_BRIGHTNESS);
        neopixel.setPixelColor(0, neopixel.Color(purple, 0, purple));
      } else {
        neopixel.setPixelColor(0, neopixel.Color(0, 0, 0));  // OFF
      }
      neopixel.show();
    }
    return;
  }
  
  // PRIORITY 4: Player connect - PINK/MAGENTA FADE (500ms fade in/out)
  if (showingConnectFade && currentMillis - connectFadeStart >= CONNECT_FADE_DURATION) {
    showingConnectFade = false;
  }
  
  if (showingConnectFade) {
    unsigned long elapsed = currentMillis - connectFadeStart;
    float progress = (float)elapsed / CONNECT_FADE_DURATION;
    
    // Fade in then out: 0 -> 1 -> 0
    float fade = (progress < 0.5) ? (progress * 2.0) : ((1.0 - progress) * 2.0);
    uint8_t brightness = (uint8_t)(fade * NEOPIXEL_BRIGHTNESS);
    
    // MAGENTA/PINK (255, 0, 255)
    neopixel.setPixelColor(0, neopixel.Color(brightness, 0, brightness));
    neopixel.show();
    return;
  }
  
  // PRIORITY 5: Default display - Vehicle ID or Battery
  if (ledGreenMode) {
    // Vehicle ID Mode - Bot-specific color
    #ifdef BOT_PACHAVANDI
      neopixel.setPixelColor(0, neopixel.Color(0, NEOPIXEL_BRIGHTNESS, 0));  // GREEN
    #elif BOT_NEELAVANDI
      neopixel.setPixelColor(0, neopixel.Color(0, 0, NEOPIXEL_BRIGHTNESS));  // BLUE
    #endif
  } else {
    // Battery Mode - Map voltage to percentage (4.2V=100%, 3.0V=0%)
    float batteryPercent = ((batteryVoltage - 3.0) / (4.2 - 3.0)) * 100.0;
    batteryPercent = constrain(batteryPercent, 0, 100);
    
    if (batteryPercent > 80) {
      // 100-80%: CYAN (0, 255, 255)
      neopixel.setPixelColor(0, neopixel.Color(0, NEOPIXEL_BRIGHTNESS, NEOPIXEL_BRIGHTNESS));
    } else if (batteryPercent > 40) {
      // 80-40%: YELLOW (255, 255, 0)
      neopixel.setPixelColor(0, neopixel.Color(NEOPIXEL_BRIGHTNESS, NEOPIXEL_BRIGHTNESS, 0));
    } else if (batteryPercent > 10) {
      // 40-10%: ORANGE solid (255, 50, 0)
      uint8_t orangeGreen = map(50, 0, 255, 0, NEOPIXEL_BRIGHTNESS);
      neopixel.setPixelColor(0, neopixel.Color(NEOPIXEL_BRIGHTNESS, orangeGreen, 0));
    } else {
      // 10-0%: ORANGE blink (500ms on/off)
      if (currentMillis - lastBlinkToggle >= 500) {
        blinkState = !blinkState;
        lastBlinkToggle = currentMillis;
      }
      if (blinkState) {
        uint8_t orangeGreen = map(50, 0, 255, 0, NEOPIXEL_BRIGHTNESS);
        neopixel.setPixelColor(0, neopixel.Color(NEOPIXEL_BRIGHTNESS, orangeGreen, 0));
      } else {
        neopixel.setPixelColor(0, neopixel.Color(0, 0, 0));  // OFF
      }
    }
  }
  neopixel.show();
}

// =============================================================================
// Telnet
// =============================================================================
void setupTelnet() {
  telnetServer.begin();
  telnetServer.setNoDelay(true);
  telnetPrintln("\n=== Telnet Started ===");
  telnetPrintln("Connect: telnet 192.168.4.1");
}

void handleTelnet() {
  if (telnetServer.hasClient()) {
    if (telnetConnected && telnetClient && telnetClient.connected()) {
      WiFiClient newClient = telnetServer.available();
      newClient.stop();
      return;
    }
    telnetClient = telnetServer.available();
    telnetConnected = true;
    telnetAuthenticated = false;  // Reset authentication on new connection
    telnetPrintln("\n=== Telnet Connected ===");
    telnetPrintln("Enter password for admin access:");
  }
  
  // Read and parse commands
  while (telnetConnected && telnetClient && telnetClient.available()) {
    char c = telnetClient.read();
    
    if (c == '\n' || c == '\r') {
      if (telnetCommandBuffer.length() > 0) {
        telnetCommandBuffer.trim();
        
        // Check authentication first
        if (!telnetAuthenticated) {
          // Clean password input
          String cleanPassword = telnetCommandBuffer;
          cleanPassword.trim();
          
          // PuTTY terminal emulator sends leading apostrophe (ASCII 39) - remove it
          if (cleanPassword.length() > 0 && cleanPassword.charAt(0) == 39) {
            cleanPassword = cleanPassword.substring(1);
          }
          
          if (cleanPassword == TELNET_PASSWORD) {
            telnetAuthenticated = true;
            telnetPrintln("\n=== Access Granted ===");
            telnetPrintln("Admin Commands:");
            telnetPrintln("  'p' - Print player list");
            telnetPrintln("  's' - Toggle motor lock (LED shows RED)");
            telnetPrintln("  'n' - Next user (skip current)");
            telnetPrintln("  'rm <#>' - Remove player by position (1-3)");
            #ifdef BOT_PACHAVANDI
              telnetPrintln("  't' - Toggle LED mode (battery/green)");
            #elif BOT_NEELAVANDI
              telnetPrintln("  't' - Toggle LED mode (battery/blue)");
            #endif
          } else {
            telnetPrintln("Incorrect password. Disconnecting.");
            telnetClient.stop();
            telnetConnected = false;
            telnetAuthenticated = false;
          }
          telnetCommandBuffer = "";
          return;
        }
        
        telnetCommandBuffer.toLowerCase();
        
        // Print player list
        if (telnetCommandBuffer == "p") {
          telnetPrintf("[ADMIN] Players in queue: %d/%d\n", playerCount, MAX_PLAYERS);
          if (playerCount == 0) {
            telnetPrintln("  (empty)");
          } else {
            for (int i = 0; i < playerCount; i++) {
              telnetPrintf("  %d. %s (%s) %s\n", 
                          i + 1, 
                          playerQueue[i].nickname.c_str(), 
                          playerQueue[i].ipAddress.c_str(),
                          playerQueue[i].isActive ? "[CONTROLLING]" : "[WAITING]");
            }
          }
        }
        
        // Remove player by position (rm 1, rm 2, rm 3 )
        else if (telnetCommandBuffer.startsWith("rm ")) {
          String posStr = telnetCommandBuffer.substring(3);
          posStr.trim();
          
          // Input validation: Check if position string is valid
          if (posStr.length() == 0 || posStr.length() > 2) {
            telnetPrintln("[ADMIN] Invalid position format. Usage: rm <1-3>");
            telnetCommandBuffer = "";
            continue;
          }
          
          // Validate all characters are digits
          bool isValid = true;
          for (unsigned int i = 0; i < posStr.length(); i++) {
            if (!isdigit(posStr.charAt(i))) {
              isValid = false;
              break;
            }
          }
          
          if (!isValid) {
            telnetPrintln("[ADMIN] Invalid position format. Usage: rm <1-3>");
            telnetCommandBuffer = "";
            continue;
          }
          
          int position = posStr.toInt();
          
          // Range validation
          if (position < 1 || position > playerCount) {
            telnetPrintf("[ADMIN] Invalid position: %d (valid: 1-%d)\n", position, playerCount);
          } else {
            int index = position - 1;  // Convert to 0-based index
            String removedNickname = playerQueue[index].nickname;
            uint32_t removedClientId = playerQueue[index].clientId;
            bool wasActive = playerQueue[index].isActive;
            
            // Send kicked message to removed player BEFORE removing them
            AsyncWebSocketClient *kickedClient = ws.client(removedClientId);
            if (kickedClient) {
              kickedClient->text("{\"type\":\"kicked\",\"message\":\"❌ You were removed by admin\"}");
              delay(100);  // Give time for message to send
            }
            
            // Remove player from queue (shift everyone after them up)
            for (int i = index; i < playerCount - 1; i++) {
              playerQueue[i] = playerQueue[i + 1];
            }
            playerCount--;
            
            // If we removed the active player, activate the new first player
            if (wasActive && playerCount > 0) {
              stopMotors();
              joyX = 0;
              joyY = 0;
              lastControlTime = millis();
              
              playerQueue[0].isActive = true;
              telnetPrintf("[ADMIN] Removed '%s' (pos %d), control given to '%s'\n", 
                          removedNickname.c_str(), position, playerQueue[0].nickname.c_str());
              broadcastPlayerList();
            } else {
              telnetPrintf("[ADMIN] Removed '%s' (pos %d) from waiting list\n", 
                          removedNickname.c_str(), position);
              if (playerCount == 0) {
                stopMotors();
                joyX = 0;
                joyY = 0;
              }
              broadcastPlayerList();
            }
          }
        }
        
        // Motor lock toggle command
        else if (telnetCommandBuffer == "s") {
          motorsLockedByAdmin = !motorsLockedByAdmin;
          stopMotors();
          
          if (motorsLockedByAdmin) {
            telnetPrintln("[ADMIN] Motors LOCKED!");
            ws.textAll("{\"type\":\"adminMessage\",\"message\":\"🔒 Motors locked by admin\"}");
          } else {
            telnetPrintln("[ADMIN] Motors UNLOCKED!");
            ws.textAll("{\"type\":\"adminMessage\",\"message\":\"🔓 Motors unlocked\"}");
          }
        }
        
        // Skip current user and give control to next
        else if (telnetCommandBuffer == "n") {
          if (playerCount > 0 && playerQueue[0].isActive) {
            String removedNickname = playerQueue[0].nickname;
            uint32_t removedClientId = playerQueue[0].clientId;
            
            // Send kicked message to removed player BEFORE removing them
            AsyncWebSocketClient *kickedClient = ws.client(removedClientId);
            if (kickedClient) {
              kickedClient->text("{\"type\":\"kicked\",\"message\":\"❌ You were removed by admin\"}");
              delay(100);  // Give time for message to send
            }
            
            // Remove current player
            for (int i = 0; i < playerCount - 1; i++) {
              playerQueue[i] = playerQueue[i + 1];
            }
            playerCount--;
            
            // Activate next player if any
            if (playerCount > 0) {
              stopMotors();
              joyX = 0;
              joyY = 0;
              lastControlTime = millis();
              
              playerQueue[0].isActive = true;
              telnetPrintf("[ADMIN] Removed '%s', control given to '%s'\n", 
                          removedNickname.c_str(), playerQueue[0].nickname.c_str());
              
              // Update all players' control status
              broadcastPlayerList();
            } else {
              stopMotors();
              joyX = 0;
              joyY = 0;
              telnetPrintf("[ADMIN] Removed '%s', no players in queue\n", removedNickname.c_str());
              stopMotors();
            }
          } else {
            telnetPrintln("[ADMIN] No active player to remove");
          }
        }
        
        // Toggle LED mode (battery indicator vs solid color)
        else if (telnetCommandBuffer == "t") {
          ledGreenMode = !ledGreenMode;
          
          if (ledGreenMode) {
            #ifdef BOT_PACHAVANDI
              telnetPrintln("[ADMIN] LED mode: SOLID GREEN");
            #elif BOT_NEELAVANDI
              telnetPrintln("[ADMIN] LED mode: SOLID BLUE");
            #endif
          } else {
            telnetPrintln("[ADMIN] LED mode: BATTERY INDICATOR");
          }
        }
        
        else if (telnetCommandBuffer.length() > 0) {
          telnetPrintln("Unknown command. Available: 'p' (list), 's' (motor lock), 'n' (next), 'rm <#>' (remove), 't' (LED mode)");
        }
        
        telnetCommandBuffer = "";
      }
    } else if (c >= 32 && c <= 126) {  // Printable characters only
      telnetCommandBuffer += c;
      if (telnetCommandBuffer.length() > 20) {  // Prevent buffer overflow
        telnetCommandBuffer = "";
      }
    }
  }
  
  if (telnetConnected && telnetClient && !telnetClient.connected()) {
    telnetClient.stop();
    telnetConnected = false;
    telnetAuthenticated = false;  // Clear authentication on disconnect
    telnetCommandBuffer = "";
  }
}

// =============================================================================
// Setup and Loop
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  telnetPrintln("\n\n=================================");
  telnetPrintln("Robo Soccer Bot - SIMPLE VERSION");
  telnetPrintln("=================================\n");

  // Initialize watchdog timer: 10 second timeout
  // If loop() doesn't call esp_task_wdt_reset() within 10 seconds, system will auto-reboot
  // This prevents hangs from WiFi stack crashes or infinite loops
  esp_task_wdt_init(10, true);  // 10 second timeout, panic on trigger
  esp_task_wdt_add(NULL);       // Add current task to watchdog monitoring
  telnetPrintln("Watchdog timer enabled (10s timeout)");

  setupMotors();
  setupServo();
  setupNeoPixel();
  setupWiFi();
  setupTelnet();
  setupWebServer();

  telnetPrintln("\n=== System Ready ===");
  telnetPrintln("Connect to: http://192.168.4.1");
  telnetPrintln("====================\n");
  
  lastControlTime = millis();
}

void loop() {
  // Feed the watchdog timer to prevent auto-reboot
  // This tells the watchdog "system is still running normally"
  esp_task_wdt_reset();
  
  ws.cleanupClients();
  handleTelnet();
  updateFlapper();
  updateMotors();
  handleControlTimeout();
  readBatteryVoltage();
  updateNeoPixel();
  delay(20);
}
