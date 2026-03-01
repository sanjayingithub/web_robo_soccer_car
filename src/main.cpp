#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Adafruit_NeoPixel.h>
#include <esp_wifi.h>
#include <ESP32Servo.h>  // Proper servo library with timer management
#include <cmath>         // For pow() and fabs() in exponential curve

// =============================================================================
// Telnet Server Configuration
// =============================================================================
WiFiServer telnetServer(23);
WiFiClient telnetClient;
bool telnetConnected = false;

// Telnet verbosity control
#define VERBOSE_CONNECTIONS false  // Set to false to reduce connection spam

// =============================================================================
// WiFi Access Point Configuration
// =============================================================================
const char* ssid = "RoboSoccer";
const char* password = "12345678";  // Minimum 8 characters for WPA2

// =============================================================================
// Admin MAC Address Whitelist
// =============================================================================
// Add your admin devices' MAC addresses here
// To get MAC: Connect device, check Serial Monitor for "Client MAC: XX:XX:XX:XX:XX:XX"
const char* adminMACs[] = {
    "74:4C:A1:DC:49:CB",  // Admin device
};
const int adminMACCount = 1;  // Update this when adding/removing MACs

// =============================================================================
// MAC->IP Mapping Table (fixes admin detection bug)
// =============================================================================
struct MACIPMapping {
  uint8_t mac[6];
  IPAddress ip;
  unsigned long lastSeen;
  bool valid;
};

const int MAX_MAC_MAPPINGS = 8;  // Support up to 8 simultaneous connections
MACIPMapping macIPTable[MAX_MAC_MAPPINGS];
int mappingCount = 0;

// Forward declarations for mapping functions
void addOrUpdateMACMapping(uint8_t* mac, IPAddress ip);
void removeMACMapping(uint8_t* mac);
void cleanupStaleMappings();

// =============================================================================
// Client Connection Management (SIMPLE)
// =============================================================================
const int MAX_PLAYERS_IN_QUEUE = 3;  // 1 active + 2 waiting

struct PlayerInfo {
  uint32_t clientId;
  String nickname;
  String ipAddress;
  bool isActive;  // Currently controlling
};

PlayerInfo playerQueue[MAX_PLAYERS_IN_QUEUE];
int playerCount = 0;
uint32_t adminClientId = 0;   // ONE admin from whitelist (0 = none)
bool isAdminControlling = false;  // Is admin actively moving joystick?

// Helper functions
int findPlayerIndex(uint32_t clientId);
int getActivePlayerIndex();
void setActivePlayer(int index);
void removePlayer(uint32_t clientId);
void broadcastPlayerList();

// =============================================================================
// Motor Driver Pin Definitions (DRV8833)
// =============================================================================
// Left Motor (PWM applied to IN pins)
#define LEFT_MOTOR_IN1 2
#define LEFT_MOTOR_IN2 3

// Right Motor (PWM applied to IN pins)
#define RIGHT_MOTOR_IN1 4
#define RIGHT_MOTOR_IN2 5

// =============================================================================
// Servo/Flapper Pin Definition
// =============================================================================
#define SERVO_PIN 21

// Servo object using ESP32Servo library (handles timers automatically)
Servo flapperServo;

// =============================================================================
// NeoPixel and Battery Monitor Pin Definitions
// =============================================================================
#define NEOPIXEL_PIN 7
#define BATTERY_ADC_PIN 0  // ADC1_CH0 (GPIO0 on ESP32-C3)
#define NUM_PIXELS 1

// =============================================================================
// PWM Configuration
// =============================================================================
// Motor PWM channels - DRV8833 uses PWM on IN pins
const int LEFT_MOTOR_IN1_CHANNEL = 0;
const int LEFT_MOTOR_IN2_CHANNEL = 1;
const int RIGHT_MOTOR_IN1_CHANNEL = 2;
const int RIGHT_MOTOR_IN2_CHANNEL = 3;
const int SERVO_PWM_CHANNEL = 4;

// Motor PWM settings
const int MOTOR_PWM_FREQ = 1000;      // 1kHz
const int MOTOR_PWM_RESOLUTION = 8;   // 8-bit (0-255)

// Servo angles (ESP32Servo library uses standard degrees 0-180)
const int servoRestAngle = 0;      // Rest position
const int servoKickAngle = 90;     // Kick position (full throw)

// =============================================================================
// Control State Variables
// =============================================================================
volatile int joyX = 0;
volatile int joyY = 0;
volatile bool flapState = false;

// Motor disable flag for testing
bool motorsDisabled = false;

// Safety timeout
#define CONTROL_TIMEOUT 500  // milliseconds
unsigned long lastControlTime = 0;

// Control output throttling
unsigned long lastControlPrint = 0;
const unsigned long CONTROL_PRINT_INTERVAL = 500;  // Print every 500ms max (was 200ms)
int lastPrintedX = 0;
int lastPrintedY = 0;
bool lastPrintedFlap = false;

// =============================================================================
// NeoPixel and Battery Monitor Variables
// =============================================================================
Adafruit_NeoPixel neopixel(NUM_PIXELS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// Battery monitoring
float batteryVoltage = 0.0;
unsigned long lastBatteryRead = 0;
const unsigned long BATTERY_READ_INTERVAL = 10000;  // Read every 10 seconds (was 5s)

// Voltage divider ratio (10k + 10k = divide by 2)
const float VOLTAGE_DIVIDER_RATIO = 2.0;
const float ADC_REFERENCE_VOLTAGE = 3.3;  // ESP32 ADC reference
const int ADC_RESOLUTION = 4095;  // 12-bit ADC

// NeoPixel brightness (capped for battery life)
const uint8_t NEOPIXEL_BRIGHTNESS = 20;  // Out of 255

// Disconnect indicator
unsigned long lastDisconnectTime = 0;
bool blinkState = false;
unsigned long lastBlinkToggle = 0;

// =============================================================================
// Flapper State Machine (from reference code)
// =============================================================================
unsigned long previousMillis = 0;
const long kickDelay = 1000;      // Time for single kick action (increased for servo movement)
const long kickCooldown = 100;    // Cooldown before next kick
bool kicking = false;
bool coolingDown = false;
unsigned long cooldownStart = 0;
bool kickRequested = false;       // Edge-triggered kick request (INITIALIZED to false)

// White flash feedback
bool showingWhiteFlash = false;
unsigned long whiteFlashStart = 0;
const long WHITE_FLASH_DURATION = 200;  // 200ms white flash

// =============================================================================
// WebSocket and Web Server
// =============================================================================
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// =============================================================================
// Dead Zone Configuration
// =============================================================================
const int DEAD_ZONE = 5;  // Dead zone threshold

// =============================================================================
// Function Prototypes
// =============================================================================
void setupMotors();
void setupServo();
void setupWiFi();
void setupWebServer();
void handleWebSocketMessage(void *arg, uint8_t *data, size_t len);
void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, 
                      AwsEventType type, void *arg, uint8_t *data, size_t len);
void updateMotors();
void setMotorSpeed(int leftSpeed, int rightSpeed);
void stopMotors();
void updateFlapper();
void handleControlTimeout();
bool isAdminMAC(uint8_t* mac);
String macToString(uint8_t* mac);
bool getClientMAC(IPAddress ip, uint8_t* mac);
void sendStatusToClient(AsyncWebSocketClient *client, const char* status, const char* message);
void setupNeoPixel();
void readBatteryVoltage();
void updateNeoPixel();
void setupTelnet();
void handleTelnet();
void telnetPrint(const String& message);
void telnetPrintln(const String& message);
void telnetPrintf(const char* format, ...);

// =============================================================================
// Telnet Helper Functions
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
    telnetClient.print("\r\n");  // Proper Telnet line ending
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
    // Replace \n with \r\n for proper Telnet display
    String output = String(buffer);
    output.replace("\n", "\r\n");
    telnetClient.print(output);
  }
}

// =============================================================================
// Servo Control Wrapper (tracks all writes)
// =============================================================================
void writeServo(int angle, const char* source) {
  telnetPrintf("@@@ SERVO WRITE: %d deg from [%s] (joyX=%d, joyY=%d) @@@\n", 
               angle, source, joyX, joyY);
  flapperServo.write(angle);
}

void setupTelnet() {
  telnetServer.begin();
  telnetServer.setNoDelay(true);
  telnetPrintln("\n=== Telnet Server Started ===");
  telnetPrintln("Connect using: telnet 192.168.4.1");
  telnetPrintln("=============================");
}

void handleTelnet() {
  // Check for new client
  if (telnetServer.hasClient()) {
    if (telnetConnected && telnetClient && telnetClient.connected()) {
      // Reject new connection if already connected
      WiFiClient newClient = telnetServer.available();
      newClient.stop();
      return;
    }
    
    // Accept new client
    telnetClient = telnetServer.available();
    telnetConnected = true;
    telnetPrintln("\n=== Telnet Connected ===");
    telnetPrintln("Robo Soccer Bot Telnet Monitor");
    telnetPrintln("Commands: 't' = test servo, 'q' = quit, 'm' = toggle motors");
    telnetPrintln("========================\n");
  }
  
  // Check if client disconnected
  if (telnetConnected && telnetClient) {
    if (!telnetClient.connected()) {
      telnetClient.stop();
      telnetConnected = false;
      Serial.println("[Telnet] Client disconnected");
      return;
    }
    
    // Check for commands from Telnet client
    if (telnetClient.available()) {
      char cmd = telnetClient.read();
      if (cmd == 't' || cmd == 'T') {
        telnetPrintln("\n=== SERVO TEST ===");
        
        // Attach servo and give it time to initialize
        flapperServo.attach(SERVO_PIN, 1000, 2000);
        delay(50);
        
        writeServo(servoRestAngle, "TEST-0deg");
        delay(1000);
        writeServo(45, "TEST-45deg");
        delay(1000);
        writeServo(servoKickAngle, "TEST-kick");
        delay(1000);
        writeServo(servoRestAngle, "TEST-rest");
        delay(100);  // Let servo complete final movement
        
        // Detach and ground the pin
        flapperServo.detach();
        pinMode(SERVO_PIN, OUTPUT);
        digitalWrite(SERVO_PIN, LOW);
        
        telnetPrintln("=== TEST COMPLETE (pin grounded) ===\n");
      } else if (cmd == 'm' || cmd == 'M') {
        motorsDisabled = !motorsDisabled;
        if (motorsDisabled) {
          telnetPrintln("\n*** MOTORS DISABLED - Testing servo isolation ***\n");
          stopMotors();
        } else {
          telnetPrintln("\n*** MOTORS ENABLED ***\n");
        }
      } else if (cmd == 'q' || cmd == 'Q') {
        telnetPrintln("\nGoodbye!\n");
        telnetClient.stop();
        telnetConnected = false;
      }
    }
  }
}

// =============================================================================
// Motor Control Functions
// =============================================================================

void setupMotors() {
  // Setup PWM channels for all motor control pins
  // DRV8833 uses PWM on both IN1 and IN2 pins
  ledcSetup(LEFT_MOTOR_IN1_CHANNEL, MOTOR_PWM_FREQ, MOTOR_PWM_RESOLUTION);
  ledcAttachPin(LEFT_MOTOR_IN1, LEFT_MOTOR_IN1_CHANNEL);
  
  ledcSetup(LEFT_MOTOR_IN2_CHANNEL, MOTOR_PWM_FREQ, MOTOR_PWM_RESOLUTION);
  ledcAttachPin(LEFT_MOTOR_IN2, LEFT_MOTOR_IN2_CHANNEL);
  
  ledcSetup(RIGHT_MOTOR_IN1_CHANNEL, MOTOR_PWM_FREQ, MOTOR_PWM_RESOLUTION);
  ledcAttachPin(RIGHT_MOTOR_IN1, RIGHT_MOTOR_IN1_CHANNEL);
  
  ledcSetup(RIGHT_MOTOR_IN2_CHANNEL, MOTOR_PWM_FREQ, MOTOR_PWM_RESOLUTION);
  ledcAttachPin(RIGHT_MOTOR_IN2, RIGHT_MOTOR_IN2_CHANNEL);

  // Initialize motors stopped
  stopMotors();
  
  telnetPrintln("Motors initialized (DRV8833 mode)");
}

void setMotorSpeed(int leftSpeed, int rightSpeed) {
  // Clamp values to [-100, 100]
  leftSpeed = constrain(leftSpeed, -100, 100);
  rightSpeed = constrain(rightSpeed, -100, 100);

  // Apply dead zone
  if (abs(leftSpeed) < DEAD_ZONE) leftSpeed = 0;
  if (abs(rightSpeed) < DEAD_ZONE) rightSpeed = 0;

  // Map to PWM range (0-255)
  int leftPWM = map(abs(leftSpeed), 0, 100, 0, 255);
  int rightPWM = map(abs(rightSpeed), 0, 100, 0, 255);

  // DRV8833 control: Apply PWM to one pin, LOW to the other
  // Left motor
  if (leftSpeed > 0) {
    // Forward: IN1 = PWM, IN2 = LOW
    ledcWrite(LEFT_MOTOR_IN1_CHANNEL, leftPWM);
    ledcWrite(LEFT_MOTOR_IN2_CHANNEL, 0);
  } else if (leftSpeed < 0) {
    // Reverse: IN1 = LOW, IN2 = PWM
    ledcWrite(LEFT_MOTOR_IN1_CHANNEL, 0);
    ledcWrite(LEFT_MOTOR_IN2_CHANNEL, leftPWM);
  } else {
    // Stop: Both LOW (coast)
    ledcWrite(LEFT_MOTOR_IN1_CHANNEL, 0);
    ledcWrite(LEFT_MOTOR_IN2_CHANNEL, 0);
  }

  // Right motor
  if (rightSpeed > 0) {
    // Forward: IN1 = PWM, IN2 = LOW
    ledcWrite(RIGHT_MOTOR_IN1_CHANNEL, rightPWM);
    ledcWrite(RIGHT_MOTOR_IN2_CHANNEL, 0);
  } else if (rightSpeed < 0) {
    // Reverse: IN1 = LOW, IN2 = PWM
    ledcWrite(RIGHT_MOTOR_IN1_CHANNEL, 0);
    ledcWrite(RIGHT_MOTOR_IN2_CHANNEL, rightPWM);
  } else {
    // Stop: Both LOW (coast)
    ledcWrite(RIGHT_MOTOR_IN1_CHANNEL, 0);
    ledcWrite(RIGHT_MOTOR_IN2_CHANNEL, 0);
  }
}

void stopMotors() {
  // DRV8833: Set both inputs LOW for coast/stop
  ledcWrite(LEFT_MOTOR_IN1_CHANNEL, 0);
  ledcWrite(LEFT_MOTOR_IN2_CHANNEL, 0);
  ledcWrite(RIGHT_MOTOR_IN1_CHANNEL, 0);
  ledcWrite(RIGHT_MOTOR_IN2_CHANNEL, 0);
}

void disableMotorPWM() {
  // Completely detach LEDC from pins and pull LOW to eliminate interference
  ledcDetachPin(LEFT_MOTOR_IN1);
  ledcDetachPin(LEFT_MOTOR_IN2);
  ledcDetachPin(RIGHT_MOTOR_IN1);
  ledcDetachPin(RIGHT_MOTOR_IN2);
  
  // Pull pins to ground
  pinMode(LEFT_MOTOR_IN1, OUTPUT);
  pinMode(LEFT_MOTOR_IN2, OUTPUT);
  pinMode(RIGHT_MOTOR_IN1, OUTPUT);
  pinMode(RIGHT_MOTOR_IN2, OUTPUT);
  digitalWrite(LEFT_MOTOR_IN1, LOW);
  digitalWrite(LEFT_MOTOR_IN2, LOW);
  digitalWrite(RIGHT_MOTOR_IN1, LOW);
  digitalWrite(RIGHT_MOTOR_IN2, LOW);
}

void enableMotorPWM() {
  // Reattach LEDC channels to pins
  ledcAttachPin(LEFT_MOTOR_IN1, LEFT_MOTOR_IN1_CHANNEL);
  ledcAttachPin(LEFT_MOTOR_IN2, LEFT_MOTOR_IN2_CHANNEL);
  ledcAttachPin(RIGHT_MOTOR_IN1, RIGHT_MOTOR_IN1_CHANNEL);
  ledcAttachPin(RIGHT_MOTOR_IN2, RIGHT_MOTOR_IN2_CHANNEL);
  
  // Set to stopped state
  stopMotors();
}

// =============================================================================
// Motor Control Functions
// =============================================================================

// Apply exponential curve to input for more natural control
// Small inputs = gentle response, large inputs = aggressive response
int applyExponentialCurve(int value, float exponent) {
  // Handle zero input immediately
  if (value == 0) return 0;
  
  // Normalize to -1.0 to +1.0
  float normalized = value / 100.0;
  
  // Clamp to valid range (safety check)
  if (normalized > 1.0) normalized = 1.0;
  if (normalized < -1.0) normalized = -1.0;
  
  // Preserve sign
  float sign = (normalized >= 0) ? 1.0 : -1.0;
  
  // Apply exponential curve (cube for turning feel)
  float curved = sign * pow(fabs(normalized), exponent);
  
  // Scale back to -100 to +100
  int result = (int)(curved * 100.0);
  
  // Clamp result to valid range
  if (result > 100) result = 100;
  if (result < -100) result = -100;
  
  return result;
}

void updateMotors() {
  // Check if motors are disabled for testing
  if (motorsDisabled) {
    setMotorSpeed(0, 0);
    return;
  }
  
  // Apply exponential curve to turning for better control
  // Exponent 2.5 = gentle at center, aggressive at edges
  int curvedX = applyExponentialCurve(joyX, 2.5);
  
  // Differential drive mixing formula
  // left = y + x
  // right = y - x
  int leftSpeed = joyY + curvedX;
  int rightSpeed = joyY - curvedX;

  // DEBUG: Show motor calculations periodically
  static unsigned long lastMotorDebug = 0;
  static int lastLeftSpeed = 0;
  static int lastRightSpeed = 0;
  if (millis() - lastMotorDebug >= 1000 || 
      abs(leftSpeed - lastLeftSpeed) > 20 || 
      abs(rightSpeed - lastRightSpeed) > 20) {
    telnetPrintf("[MOTORS] L=%d, R=%d (from x=%d->%d, y=%d)\n", 
                 leftSpeed, rightSpeed, joyX, curvedX, joyY);
    lastMotorDebug = millis();
    lastLeftSpeed = leftSpeed;
    lastRightSpeed = rightSpeed;
  }

  setMotorSpeed(leftSpeed, rightSpeed);
}

// =============================================================================
// Servo/Flapper Control Functions (from reference code)
// =============================================================================

void setupServo() {
  telnetPrintln("Servo initializing...");
  telnetPrintf("  GPIO Pin: %d\n", SERVO_PIN);
  telnetPrintln("  Using ESP32Servo library (dedicated timer)");
  
  // CRITICAL: Use timer 1 explicitly (motors use timer 0)
  // ESP32PWM::allocateTimer(1);  // Commented - returns void, auto-allocated
  flapperServo.setPeriodHertz(50);
  
  // Don't attach servo at startup - keep pin grounded to avoid interference
  pinMode(SERVO_PIN, OUTPUT);
  digitalWrite(SERVO_PIN, LOW);
  
  telnetPrintln("Servo initialized (pin grounded to prevent interference)");
}

void updateFlapper() {
  unsigned long currentMillis = millis();

  // DEBUG: Show flapper state every second
  static unsigned long lastFlapperDebug = 0;
  if (currentMillis - lastFlapperDebug >= 1000) {
    telnetPrintf("[FLAPPER_STATE] requested=%d, flapState=%d\n",
                 kickRequested, flapState);
    lastFlapperDebug = currentMillis;
  }

  // Check if kick was requested (edge-triggered)
  if (kickRequested) {
    kickRequested = false;
    
    // Trigger white flash IMMEDIATELY for instant visual feedback
    showingWhiteFlash = true;
    whiteFlashStart = millis();
    
    // Manually update NeoPixel NOW before blocking delays
    neopixel.setPixelColor(0, neopixel.Color(NEOPIXEL_BRIGHTNESS, NEOPIXEL_BRIGHTNESS, NEOPIXEL_BRIGHTNESS));
    neopixel.show();
    
    // Stop motors during kick
    stopMotors();
    telnetPrintln("[FLAPPER] Executing kick...");
    
    // Attach servo with generous initialization time
    telnetPrintln("[FLAPPER] Attaching servo...");
    flapperServo.attach(SERVO_PIN, 1000, 2000);
    delay(100);  // INCREASED: Give library more time under WiFi load
    
    // Verify attachment by writing rest position first
    writeServo(servoRestAngle, "PRE_KICK_REST");
    delay(50);  // Let servo reach rest before kicking
    
    // Execute BLOCKING kick sequence
    writeServo(servoKickAngle, "KICK_BLOCKING");
    delay(500);  // Hold kick position
    
    writeServo(servoRestAngle, "RETURN_BLOCKING");
    delay(150);  // INCREASED: Ensure servo completes return movement
    
    // Detach and ground the pin to prevent interference
    telnetPrintln("[FLAPPER] Detaching servo...");
    flapperServo.detach();
    pinMode(SERVO_PIN, OUTPUT);
    digitalWrite(SERVO_PIN, LOW);
    
    telnetPrintln("[FLAPPER] Kick complete (pin grounded)");
  }
}

// =============================================================================
// Player Queue Management Functions
// =============================================================================

int findPlayerIndex(uint32_t clientId) {
  for (int i = 0; i < playerCount; i++) {
    if (playerQueue[i].clientId == clientId) {
      return i;
    }
  }
  return -1;
}

int getActivePlayerIndex() {
  for (int i = 0; i < playerCount; i++) {
    if (playerQueue[i].isActive) {
      return i;
    }
  }
  return -1;
}

void setActivePlayer(int index) {
  // Deactivate all players
  for (int i = 0; i < playerCount; i++) {
    playerQueue[i].isActive = false;
  }
  
  // Activate selected player
  if (index >= 0 && index < playerCount) {
    playerQueue[index].isActive = true;
    telnetPrintf("Active player: %s (%s)\n", 
                  playerQueue[index].nickname.c_str(), 
                  playerQueue[index].ipAddress.c_str());
    
    // Notify all players
    broadcastPlayerList();
  }
}

void removePlayer(uint32_t clientId) {
  int index = findPlayerIndex(clientId);
  if (index == -1) return;
  
  bool wasActive = playerQueue[index].isActive;
  
  // Shift array left
  for (int i = index; i < playerCount - 1; i++) {
    playerQueue[i] = playerQueue[i + 1];
  }
  playerCount--;
  
  // If active player left, activate next in queue
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
  
  // Send to admin
  if (adminClientId != 0) {
    AsyncWebSocketClient *adminClient = ws.client(adminClientId);
    if (adminClient) {
      adminClient->text(output);
    }
  }
  
  // Send control status to each player
  for (int i = 0; i < playerCount; i++) {
    AsyncWebSocketClient *client = ws.client(playerQueue[i].clientId);
    if (client) {
      StaticJsonDocument<200> statusDoc;
      statusDoc["type"] = "controlStatus";
      statusDoc["hasControl"] = playerQueue[i].isActive && !isAdminControlling;
      statusDoc["adminControlling"] = isAdminControlling;
      statusDoc["position"] = i + 1;
      statusDoc["queueSize"] = playerCount;
      
      String statusOutput;
      serializeJson(statusDoc, statusOutput);
      client->text(statusOutput);
    }
  }
}

// =============================================================================
// WiFi Event Handler (tracks MAC->IP mappings)
// =============================================================================

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED: {
      // Device connected to AP - get MAC and wait for IP assignment
      uint8_t* mac = info.wifi_ap_staconnected.mac;
      telnetPrintf("[WiFi] Device connected: %02X:%02X:%02X:%02X:%02X:%02X\n",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      break;
    }
    
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED: {
      // Device got IP - now we can map MAC->IP
      wifi_sta_list_t stationList;
      esp_err_t err = esp_wifi_ap_get_sta_list(&stationList);
      
      if (err == ESP_OK && stationList.num > 0) {
        // Get the most recently connected device (assumption: last in list just got IP)
        uint8_t* mac = stationList.sta[stationList.num - 1].mac;
        IPAddress assignedIP = IPAddress(info.wifi_ap_staipassigned.ip.addr);
        
        addOrUpdateMACMapping(mac, assignedIP);
        
        telnetPrintf("[WiFi] IP assigned: %s -> %02X:%02X:%02X:%02X:%02X:%02X\n",
                     assignedIP.toString().c_str(),
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      }
      break;
    }
    
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: {
      // Device disconnected - remove from mapping
      uint8_t* mac = info.wifi_ap_stadisconnected.mac;
      removeMACMapping(mac);
      
      telnetPrintf("[WiFi] Device disconnected: %02X:%02X:%02X:%02X:%02X:%02X\n",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      break;
    }
    
    default:
      break;
  }
}

// =============================================================================
// WiFi and WebSocket Functions
// =============================================================================

void setupWiFi() {
  telnetPrintln("\n=== Starting WiFi AP ===");
  
  // Initialize MAC->IP mapping table
  for (int i = 0; i < MAX_MAC_MAPPINGS; i++) {
    macIPTable[i].valid = false;
  }
  mappingCount = 0;
  
  // Register WiFi event handler BEFORE starting AP
  WiFi.onEvent(onWiFiEvent);
  
  // Configure as Access Point
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);

  IPAddress IP = WiFi.softAPIP();
  telnetPrint("AP IP address: ");
  telnetPrintln(IP.toString());
  telnetPrint("SSID: ");
  telnetPrintln(ssid);
  telnetPrintln("WiFi event tracking enabled");
  telnetPrintln("========================");
}

void handleWebSocketMessage(void *arg, uint8_t *data, size_t len, AsyncWebSocketClient *client) {
  AwsFrameInfo *info = (AwsFrameInfo*)arg;
  
  if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
    data[len] = 0;  // Null-terminate
    
    // Parse JSON
    StaticJsonDocument<300> doc;
    DeserializationError error = deserializeJson(doc, (char*)data);
    
    if (error) {
      telnetPrint("JSON parse error: ");
      telnetPrintln(error.c_str());
      return;
    }

    // Handle nickname registration (new player)
    if (doc.containsKey("nickname")) {
      String nickname = doc["nickname"].as<String>();
      nickname.trim();
      
      // Validate nickname (max 10 chars, ASCII only)
      if (nickname.length() > 10) {
        nickname = nickname.substring(0, 10);
      }
      
      int playerIndex = findPlayerIndex(client->id());
      if (playerIndex != -1) {
        playerQueue[playerIndex].nickname = nickname;
        
        // Detailed connection info
        telnetPrintln("\n=== PLAYER REGISTERED ===");
        telnetPrintf("Position: %d/%d\n", playerIndex + 1, MAX_PLAYERS_IN_QUEUE);
        telnetPrintf("Nickname: %s\n", nickname.c_str());
        telnetPrintf("IP Address: %s\n", playerQueue[playerIndex].ipAddress.c_str());
        telnetPrintf("Client ID: %u\n", playerQueue[playerIndex].clientId);
        telnetPrintf("Status: %s\n", playerQueue[playerIndex].isActive ? "ACTIVE" : "WAITING");
        telnetPrintln("========================\n");
        
        broadcastPlayerList();
      }
      return;
    }

    // Admin: Switch active player
    if (doc.containsKey("switchPlayer") && client->id() == adminClientId) {
      int newIndex = doc["switchPlayer"];
      if (newIndex >= 0 && newIndex < playerCount) {
        setActivePlayer(newIndex);
        telnetPrintf("Admin switched to player %d\n", newIndex);
      }
      return;
    }

    // Admin: Kick specific player
    if (doc.containsKey("kickPlayer") && client->id() == adminClientId) {
      int kickIndex = doc["kickPlayer"];
      if (kickIndex >= 0 && kickIndex < playerCount) {
        uint32_t kickClientId = playerQueue[kickIndex].clientId;
        AsyncWebSocketClient *kickClient = ws.client(kickClientId);
        if (kickClient) {
          sendStatusToClient(kickClient, "kicked", "Admin removed you");
          kickClient->close();
          telnetPrintf("Admin kicked player %d\n", kickIndex);
        }
      }
      return;
    }

    // Simple control priority: Admin > Active Player
    bool isAdmin = (client->id() == adminClientId);
    int playerIndex = findPlayerIndex(client->id());
    bool isActivePlayer = (playerIndex != -1 && playerQueue[playerIndex].isActive);
    
    if (!isAdmin && !isActivePlayer) {
      // Ignore commands from waiting players
      return;
    }

    // Extract control values
    int newX = doc["x"] | 0;
    int newY = doc["y"] | 0;
    // FIX: JavaScript sends 0/1, not true/false, so parse as int
    bool newF = (doc["f"] | 0) != 0;

    // DEBUG: Show ALL received values
    static unsigned long lastDebugPrint = 0;
    if (millis() - lastDebugPrint >= 200) {
      telnetPrintf("[WS_IN] x=%d, y=%d, f=%d (joyX=%d, joyY=%d, flapState=%d)\n", 
                   newX, newY, newF, joyX, joyY, flapState);
      lastDebugPrint = millis();
    }

    // Admin has priority - always accept admin commands
    // Player commands only accepted if admin is not moving
    if (isAdmin || !isAdminControlling) {
      joyX = newX;
      joyY = newY;
      
      // Detect flapper state change and log it
      if (newF != flapState) {
        flapState = newF;
        if (flapState) {
          // Rising edge - button pressed, request kick
          kickRequested = true;
          telnetPrintf("[FLAPPER] Button PRESSED - Kick requested (f: %d->1)\n", !flapState);
        } else {
          telnetPrintf("[FLAPPER] Button RELEASED (f: 1->0)\n");
        }
      }
      
      // Track if admin is actively moving joystick
      if (isAdmin) {
        bool nowControlling = (newX != 0 || newY != 0 || newF);
        if (nowControlling != isAdminControlling) {
          isAdminControlling = nowControlling;
          broadcastPlayerList();  // Update all players about control status
        }
      }
      
      lastControlTime = millis();
      
      // Throttle control output - only print periodically or on significant change
      unsigned long now = millis();
      bool significantChange = (abs(newX - lastPrintedX) > 10 || 
                                abs(newY - lastPrintedY) > 10 || 
                                newF != lastPrintedFlap);
      
      if (significantChange || (now - lastControlPrint >= CONTROL_PRINT_INTERVAL)) {
        int leftSpeed = joyY + joyX;
        int rightSpeed = joyY - joyX;
        
        const char* source = isAdmin ? "ADMIN" : playerQueue[playerIndex].nickname.c_str();
        telnetPrintf("[%s] Joy(x:%d, y:%d) -> Motors(L:%d, R:%d) | Flap:%s\n", 
                      source, joyX, joyY, leftSpeed, rightSpeed, flapState ? "ON" : "OFF");
        
        lastControlPrint = now;
        lastPrintedX = newX;
        lastPrintedY = newY;
        lastPrintedFlap = newF;
      }
    }
  }
}

void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                      AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT: {
      if (VERBOSE_CONNECTIONS) {
        telnetPrintf("WebSocket client #%u connected from %s\\n", 
                      client->id(), client->remoteIP().toString().c_str());
      }
      
      // Get client's MAC address from IP
      uint8_t mac[6];
      bool macFound = getClientMAC(client->remoteIP(), mac);
      String macStr = macFound ? macToString(mac) : "UNKNOWN";
      if (VERBOSE_CONNECTIONS) {
        telnetPrintf("Client MAC: %s\\n", macStr.c_str());
      }
      
      // Check if this is an admin device
      if (macFound && isAdminMAC(mac)) {
        // Admin device detected
        if (adminClientId != 0) {
          telnetPrintf("✗ Admin slot occupied (Client #%u)\n", adminClientId);
          sendStatusToClient(client, "denied", "Admin already connected");
          client->close();
          return;
        }
        
        adminClientId = client->id();
        
        // Detailed admin connection info
        telnetPrintln("\n=== ADMIN CONNECTED ===");
        telnetPrintf("Client ID: %u\n", client->id());
        telnetPrintf("IP Address: %s\n", client->remoteIP().toString().c_str());
        telnetPrintf("MAC Address: %s\n", macStr.c_str());
        telnetPrintln("Access: GRANTED");
        telnetPrintln("=======================\n");
        
        sendStatusToClient(client, "admin", "Admin access granted");
        
        // Send current player list to admin
        broadcastPlayerList();
      } else {
        // Regular player
        if (playerCount >= MAX_PLAYERS_IN_QUEUE) {
          telnetPrintf("✗ Player queue full\n");
          sendStatusToClient(client, "denied", "Queue is full. Please try again later.");
          client->close();
          return;
        }
        
        // Add to queue
        playerQueue[playerCount].clientId = client->id();
        playerQueue[playerCount].nickname = "Player" + String(playerCount + 1);
        playerQueue[playerCount].ipAddress = client->remoteIP().toString();
        playerQueue[playerCount].isActive = (playerCount == 0);  // First player is active
        playerCount++;
        
        telnetPrintf("✓ PLAYER joined - Position: %d/%d | IP: %s | Waiting for nickname...\n", 
                      playerCount, MAX_PLAYERS_IN_QUEUE, client->remoteIP().toString().c_str());
        
        // Request nickname from player
        StaticJsonDocument<100> doc;
        doc["type"] = "requestNickname";
        String output;
        serializeJson(doc, output);
        client->text(output);
        
        broadcastPlayerList();
      }
      break;
    }
      
    case WS_EVT_DISCONNECT:
      telnetPrintln("\n=== CLIENT DISCONNECTED ===");
      telnetPrintf("Client ID: %u\n", client->id());
      
      if (client->id() == adminClientId) {
        telnetPrintln("Role: ADMIN");
        telnetPrintln("Motors: STOPPED");
        telnetPrintln("========================\n");
        
        adminClientId = 0;
        isAdminControlling = false;
        stopMotors();
        lastDisconnectTime = millis();  // Trigger red blink
      } else {
        int disconnectedIndex = findPlayerIndex(client->id());
        if (disconnectedIndex != -1) {
          telnetPrintf("Role: PLAYER\n");
          telnetPrintf("Nickname: %s\n", playerQueue[disconnectedIndex].nickname.c_str());
          telnetPrintf("Remaining players: %d\n", playerCount - 1);
          telnetPrintln("========================\n");
        }
        
        removePlayer(client->id());
        if (playerCount == 0) {
          stopMotors();
          lastDisconnectTime = millis();  // Trigger red blink
        }
      }
      break;
      
    case WS_EVT_DATA:
      handleWebSocketMessage(arg, data, len, client);
      break;
      
    case WS_EVT_PONG:
    case WS_EVT_ERROR:
      break;
  }
}

void setupWebServer() {
  // Initialize LittleFS
  if (!LittleFS.begin(true)) {
    telnetPrintln("LittleFS mount failed");
    return;
  }
  telnetPrintln("LittleFS mounted successfully");

  // Setup WebSocket
  ws.onEvent(onWebSocketEvent);
  server.addHandler(&ws);

  // Serve static files
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/index.html", "text/html");
  });

  server.on("/controller.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/controller.js", "application/javascript");
  });

  // Handle 404
  server.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
  });

  // Start server
  server.begin();
  telnetPrintln("Web server started");
}

void handleControlTimeout() {
  unsigned long now = millis();
  
  if (now - lastControlTime > CONTROL_TIMEOUT) {
    // Timeout - stop everything
    stopMotors();
    joyX = 0;
    joyY = 0;
    flapState = false;
  }
}

// =============================================================================
// MAC Address Helper Functions
// =============================================================================

String macToString(uint8_t* mac) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(macStr);
}

// Add or update MAC->IP mapping in table
void addOrUpdateMACMapping(uint8_t* mac, IPAddress ip) {
  // Check if MAC already exists - update it
  for (int i = 0; i < MAX_MAC_MAPPINGS; i++) {
    if (macIPTable[i].valid) {
      bool macMatch = true;
      for (int j = 0; j < 6; j++) {
        if (macIPTable[i].mac[j] != mac[j]) {
          macMatch = false;
          break;
        }
      }
      if (macMatch) {
        // Update existing entry
        macIPTable[i].ip = ip;
        macIPTable[i].lastSeen = millis();
        telnetPrintf("[MAC_TABLE] Updated: %s -> %s\n", 
                     ip.toString().c_str(), macToString(mac).c_str());
        return;
      }
    }
  }
  
  // Find empty slot
  for (int i = 0; i < MAX_MAC_MAPPINGS; i++) {
    if (!macIPTable[i].valid) {
      memcpy(macIPTable[i].mac, mac, 6);
      macIPTable[i].ip = ip;
      macIPTable[i].lastSeen = millis();
      macIPTable[i].valid = true;
      mappingCount++;
      
      telnetPrintf("[MAC_TABLE] Added: %s -> %s (total: %d)\n",
                   ip.toString().c_str(), macToString(mac).c_str(), mappingCount);
      return;
    }
  }
  
  // Table full - remove oldest entry
  int oldestIndex = 0;
  unsigned long oldestTime = macIPTable[0].lastSeen;
  for (int i = 1; i < MAX_MAC_MAPPINGS; i++) {
    if (macIPTable[i].valid && macIPTable[i].lastSeen < oldestTime) {
      oldestTime = macIPTable[i].lastSeen;
      oldestIndex = i;
    }
  }
  
  memcpy(macIPTable[oldestIndex].mac, mac, 6);
  macIPTable[oldestIndex].ip = ip;
  macIPTable[oldestIndex].lastSeen = millis();
  macIPTable[oldestIndex].valid = true;
  
  telnetPrintf("[MAC_TABLE] Replaced oldest: %s -> %s\n",
               ip.toString().c_str(), macToString(mac).c_str());
}

// Remove MAC from mapping table
void removeMACMapping(uint8_t* mac) {
  for (int i = 0; i < MAX_MAC_MAPPINGS; i++) {
    if (macIPTable[i].valid) {
      bool macMatch = true;
      for (int j = 0; j < 6; j++) {
        if (macIPTable[i].mac[j] != mac[j]) {
          macMatch = false;
          break;
        }
      }
      if (macMatch) {
        macIPTable[i].valid = false;
        mappingCount--;
        telnetPrintf("[MAC_TABLE] Removed: %s (total: %d)\n",
                     macToString(mac).c_str(), mappingCount);
        return;
      }
    }
  }
}

// Clean up stale mappings (called periodically)
void cleanupStaleMappings() {
  unsigned long now = millis();
  const unsigned long STALE_TIMEOUT = 300000;  // 5 minutes
  
  for (int i = 0; i < MAX_MAC_MAPPINGS; i++) {
    if (macIPTable[i].valid && (now - macIPTable[i].lastSeen) > STALE_TIMEOUT) {
      telnetPrintf("[MAC_TABLE] Cleaning stale entry: %s\n",
                   macToString(macIPTable[i].mac).c_str());
      macIPTable[i].valid = false;
      mappingCount--;
    }
  }
}

// FIXED: Now correctly looks up MAC by IP using our mapping table
bool getClientMAC(IPAddress ip, uint8_t* mac) {
  // Search mapping table for this IP
  for (int i = 0; i < MAX_MAC_MAPPINGS; i++) {
    if (macIPTable[i].valid && macIPTable[i].ip == ip) {
      memcpy(mac, macIPTable[i].mac, 6);
      macIPTable[i].lastSeen = millis();  // Update last seen time
      return true;
    }
  }
  
  // Not found in mapping table
  telnetPrintf("[MAC_TABLE] WARNING: No MAC found for IP %s\n", ip.toString().c_str());
  return false;
}

bool isAdminMAC(uint8_t* mac) {
  String macStr = macToString(mac);
  
  for (int i = 0; i < adminMACCount; i++) {
    if (macStr.equalsIgnoreCase(adminMACs[i])) {
      return true;
    }
  }
  return false;
}

void sendStatusToClient(AsyncWebSocketClient *client, const char* status, const char* message) {
  if (!client) return;
  
  StaticJsonDocument<200> doc;
  doc["status"] = status;
  doc["message"] = message;
  doc["isAdmin"] = (client->id() == adminClientId);
  
  String output;
  serializeJson(doc, output);
  client->text(output);
}

// =============================================================================
// NeoPixel and Battery Monitor Functions
// =============================================================================

void setupNeoPixel() {
  neopixel.begin();
  neopixel.setBrightness(255);  // We control brightness in color values
  neopixel.clear();
  neopixel.show();
  
  // Configure ADC for battery monitoring
  analogSetAttenuation(ADC_11db);  // 0-3.6V range
  
  telnetPrintln("NeoPixel initialized");
}

void readBatteryVoltage() {
  unsigned long currentMillis = millis();
  
  if (currentMillis - lastBatteryRead >= BATTERY_READ_INTERVAL) {
    // Read ADC value (average of 10 samples for stability)
    int adcSum = 0;
    for (int i = 0; i < 10; i++) {
      adcSum += analogRead(BATTERY_ADC_PIN);
      delay(1);
    }
    int adcValue = adcSum / 10;
    
    // Convert ADC to voltage
    float adcVoltage = (adcValue * ADC_REFERENCE_VOLTAGE) / ADC_RESOLUTION;
    batteryVoltage = adcVoltage * VOLTAGE_DIVIDER_RATIO;
    
    lastBatteryRead = currentMillis;
    
    telnetPrintf("Battery: %.2fV (ADC: %d)\n", batteryVoltage, adcValue);
  }
}

void updateNeoPixel() {
  unsigned long currentMillis = millis();
  
  // Check if white flash timer expired
  if (showingWhiteFlash && currentMillis - whiteFlashStart >= WHITE_FLASH_DURATION) {
    showingWhiteFlash = false;
  }
  
  // Priority 1: White flash feedback (highest priority)
  if (showingWhiteFlash) {
    neopixel.setPixelColor(0, neopixel.Color(NEOPIXEL_BRIGHTNESS, NEOPIXEL_BRIGHTNESS, NEOPIXEL_BRIGHTNESS));
    neopixel.show();
    return;
  }
  
  // Priority 2: Disconnection warning (2 seconds after disconnect)
  if (currentMillis - lastDisconnectTime < 2000) {
    // Blink red at 5Hz
    if (currentMillis - lastBlinkToggle >= 100) {
      blinkState = !blinkState;
      lastBlinkToggle = currentMillis;
      
      if (blinkState) {
        neopixel.setPixelColor(0, neopixel.Color(NEOPIXEL_BRIGHTNESS, 0, 0));  // Red
      } else {
        neopixel.setPixelColor(0, neopixel.Color(0, 0, 0));  // Off
      }
      neopixel.show();
    }
    return;
  }
  
  // Priority 3: Admin control indicator
  if (isAdminControlling) {
    neopixel.setPixelColor(0, neopixel.Color(NEOPIXEL_BRIGHTNESS, 0, NEOPIXEL_BRIGHTNESS));  // Magenta
    neopixel.show();
    return;
  }
  
  // Priority 4 (Default): Battery voltage indicator
  if (batteryVoltage > 3.7) {
    // Good battery level - Green
    neopixel.setPixelColor(0, neopixel.Color(0, NEOPIXEL_BRIGHTNESS, 0));
  } else if (batteryVoltage > 3.4) {
    // Moderate battery level - Yellow
    neopixel.setPixelColor(0, neopixel.Color(NEOPIXEL_BRIGHTNESS, NEOPIXEL_BRIGHTNESS, 0));
  } else {
    // Low battery level - Red
    neopixel.setPixelColor(0, neopixel.Color(NEOPIXEL_BRIGHTNESS, 0, 0));
  }
  neopixel.show();
}

// =============================================================================
// Arduino Setup and Loop
// =============================================================================

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  telnetPrintln("\n\n=================================");
  telnetPrintln("Robo Soccer Bot - Web Controller");
  telnetPrintln("=================================\n");

  // Initialize hardware
  setupMotors();
  setupServo();
  setupNeoPixel();
  
  // Initialize WiFi and web server
  setupWiFi();
  setupTelnet();
  setupWebServer();

  telnetPrintln("\n=== System Ready ===");
  telnetPrintln("Connect to WiFi and navigate to:");
  telnetPrintln("http://192.168.4.1");
  telnetPrintln("Telnet: telnet 192.168.4.1");
  telnetPrintln("====================\n");
  
  lastControlTime = millis();
}

void loop() {
  // Clean up WebSocket clients
  ws.cleanupClients();
  
  // Handle Telnet connections
  handleTelnet();

  // Update flapper state machine
  updateFlapper();

  // Update motors based on current control state
  updateMotors();

  // Handle control timeout
  handleControlTimeout();
  
  // Update battery monitoring and NeoPixel display
  readBatteryVoltage();
  updateNeoPixel();

  // Small delay for stability
  delay(20);  // 50 Hz update rate
}
