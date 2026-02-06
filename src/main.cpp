#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Adafruit_NeoPixel.h>
#include <esp_wifi.h>

// =============================================================================
// Telnet Server Configuration
// =============================================================================
WiFiServer telnetServer(23);
WiFiClient telnetClient;
bool telnetConnected = false;

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
    "C6:6A:28:7B:36:4A",  // Admin device 1
    "AA:BB:CC:DD:EE:02",  // Organizer's tablet (REPLACE WITH REAL MAC)
    "AA:BB:CC:DD:EE:03",  // Backup admin device (REPLACE WITH REAL MAC)
};
const int adminMACCount = 3;  // Update this when adding/removing MACs

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
#define SERVO_PIN 6

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

// Servo PWM settings
const int SERVO_PWM_FREQ = 50;        // 50Hz for servo
const int SERVO_PWM_RESOLUTION = 16;  // 16-bit for precise control

// Servo angles (from reference code)
const int servo0Deg = 1638;   // ~500us pulse (0°)
const int servo90Deg = 4915;  // ~1500us pulse (90°)
const int kickAngle = 50;     // Kick angle in degrees

// =============================================================================
// Control State Variables
// =============================================================================
volatile int joyX = 0;
volatile int joyY = 0;
volatile bool flapState = false;

// Safety timeout
#define CONTROL_TIMEOUT 500  // milliseconds
unsigned long lastControlTime = 0;

// Control output throttling
unsigned long lastControlPrint = 0;
const unsigned long CONTROL_PRINT_INTERVAL = 200;  // Print every 200ms max
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
const unsigned long BATTERY_READ_INTERVAL = 5000;  // Read every 5 seconds

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
const long kickDelay = 500;       // Time for single kick action
const long kickCooldown = 50;     // Delay between repeated kicks
bool kicking = false;
bool coolingDown = false;
unsigned long cooldownStart = 0;

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
    telnetClient.println(message);
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
    telnetClient.print(buffer);
  }
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
    telnetPrintln("========================\n");
  }
  
  // Check if client disconnected
  if (telnetConnected && telnetClient) {
    if (!telnetClient.connected()) {
      telnetClient.stop();
      telnetConnected = false;
      Serial.println("[Telnet] Client disconnected");
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

void updateMotors() {
  // Differential drive mixing formula
  // left = y + x
  // right = y - x
  int leftSpeed = joyY + joyX;
  int rightSpeed = joyY - joyX;

  setMotorSpeed(leftSpeed, rightSpeed);
}

// =============================================================================
// Servo/Flapper Control Functions (from reference code)
// =============================================================================

void setupServo() {
  ledcSetup(SERVO_PWM_CHANNEL, SERVO_PWM_FREQ, SERVO_PWM_RESOLUTION);
  ledcAttachPin(SERVO_PIN, SERVO_PWM_CHANNEL);
  ledcWrite(SERVO_PWM_CHANNEL, servo0Deg);  // Initialize to 0 degrees
  
  telnetPrintln("Servo initialized");
}

void updateFlapper() {
  unsigned long currentMillis = millis();

  if (!kicking && !coolingDown && flapState) {
    // Start kick - calculate duty cycle for kick angle
    int kickDuty = map(kickAngle, 0, 180, servo0Deg, servo90Deg * 2);
    ledcWrite(SERVO_PWM_CHANNEL, kickDuty);
    previousMillis = currentMillis;
    kicking = true;
    telnetPrintln("KICK START");
    
    // Trigger white flash for kick feedback
    showingWhiteFlash = true;
    whiteFlashStart = currentMillis;
  } else if (kicking && currentMillis - previousMillis >= kickDelay) {
    // Finish kick - return to 0 degrees
    ledcWrite(SERVO_PWM_CHANNEL, servo0Deg);
    kicking = false;
    coolingDown = true;
    cooldownStart = currentMillis;
    telnetPrintln("KICK END - cooling down");
  } else if (coolingDown && currentMillis - cooldownStart >= kickCooldown) {
    // Cooldown complete, ready for next kick
    coolingDown = false;
    telnetPrintln("COOLDOWN COMPLETE");
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
// WiFi and WebSocket Functions
// =============================================================================

void setupWiFi() {
  telnetPrintln("\n=== Starting WiFi AP ===");
  
  // Configure as Access Point
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);

  IPAddress IP = WiFi.softAPIP();
  telnetPrint("AP IP address: ");
  telnetPrintln(IP.toString());
  telnetPrint("SSID: ");
  telnetPrintln(ssid);
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
    bool newF = doc["f"] | false;

    // Admin has priority - always accept admin commands
    // Player commands only accepted if admin is not moving
    if (isAdmin || !isAdminControlling) {
      joyX = newX;
      joyY = newY;
      flapState = newF;
      
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
      telnetPrintf("WebSocket client #%u connected from %s\n", 
                    client->id(), client->remoteIP().toString().c_str());
      
      // Get client's MAC address from IP
      uint8_t mac[6];
      bool macFound = getClientMAC(client->remoteIP(), mac);
      String macStr = macFound ? macToString(mac) : "UNKNOWN";
      telnetPrintf("Client MAC: %s\n", macStr.c_str());
      
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

bool getClientMAC(IPAddress ip, uint8_t* mac) {
  // Get list of connected stations from AP
  wifi_sta_list_t stationList;
  esp_err_t err = esp_wifi_ap_get_sta_list(&stationList);
  
  if (err != ESP_OK || stationList.num == 0) {
    return false;
  }
  
  // Return the most recent connection's MAC (last in list)
  memcpy(mac, stationList.sta[stationList.num - 1].mac, 6);
  return true;
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
