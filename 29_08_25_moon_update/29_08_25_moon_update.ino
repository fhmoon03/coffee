// Update date: 08/28/2025
// Version: Niter_Coffee_v1_2_3_mod_heater_btn
// Changes: Removed thermistor/temperature logic, heater controlled by push-button (pin 35) and water sensor check.
// ===================== Include Libraries =====================
#include <WiFi.h>
#include <WiFiManager.h>          // Version 2.0.17
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <math.h>
#include <vector>

// ===================== Configuration =====================
#define BUZZER_FREQ 2000
#define BUZZER_DURATION 200
#define WIFI_CHECK_INTERVAL 500  // 500 ms WiFi check interval
#define WIFI_TIMEOUT 180000      // 3 minutes WiFi timeout before reset
#define ORDER_COOLDOWN 40000     // 40 seconds delay between orders (website update time)

// OLED Config
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C
#define SDA_PIN 21
#define SCL_PIN 22
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Hardware Pins (updated per user)
#define HEATER_BUTTON_PIN 35     // Heater Push Button (active LOW)
#define HEATER_PIN 4
#define CUP_DISPENSER_PIN 16
#define WATER_SENSOR_PIN 34

#define BUZZER_PIN 14 
#define COFFEE_SOLENOID_PIN 27 
#define COFFEE_MOTOR_PIN 26 
#define COFFEE_MIXER_PIN 25
#define TEA_SOLENOID_PIN 32 
#define TEA_MOTOR_PIN 33 
#define TEA_MIXER_PIN 17

// Server Config
const char* serverHost = "clkbx.com";
const char* fetchPath = "/Niter_Coffee/fetch_orders.php";
const char* statusPath = "/Niter_Coffee/update_heating_status.php";
const char* deliveryPath = "/Niter_Coffee/delivery_confirmation.php";

// Variables
const char* currentProductName = "";
bool wifiConnected = false;
unsigned long lastWifiCheckTime = 0;
unsigned long wifiConnectionStartTime = 0;
unsigned long lastOrderProcessTime = 0;
bool orderInProgress = false;


// ===================== Sensor & Control Functions =====================
// globals for moving average
const int BUF_SIZE = 8;
int buf[BUF_SIZE];
int bufIdx = 0;
bool bufFilled = false;

// thresholds (from calibration)
int TH_ON = 1609;
int TH_OFF = 2414;
bool waterPresent = false;


// Add at the top with other globals
unsigned long systemStartMillis = 0;
const unsigned long WDT_RESET_INTERVAL = 18000000UL; // 5 hours in ms (5*60*60*1000)

// ===================== Display Functions =====================
void updateDisplay(const String& l1, const String& l2 = "", const String& l3 = "", const String& l4 = "") {
  Serial.printf("[DISPLAY] Updating display: %s | %s | %s | %s\n", l1.c_str(), l2.c_str(), l3.c_str(), l4.c_str());
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(l1);
  display.setTextSize(2);
  display.setCursor(0, 16);
  display.println(l2);
  display.setTextSize(1);
  display.setCursor(0, 32);
  display.println(l3);
  display.setCursor(0, 48);
  display.println(l4);
  display.display();
}

// ===================== Watchdog Timer =====================
void initWiFi(); // forward

void checkWiFiAndReset() {
  unsigned long currentTime = millis();
  
  if (WiFi.status() == WL_CONNECTED) {
    lastWifiCheckTime = currentTime;
    wifiConnected = true;
    return;
  }
  
  if (wifiConnected) {
    Serial.println("[WiFi] Connection lost!");
    wifiConnected = false;
    wifiConnectionStartTime = currentTime;
    updateDisplay("WiFi Disconnected", "Reconnecting...");
  } else if (currentTime - wifiConnectionStartTime > WIFI_TIMEOUT) {
    Serial.println("[WiFi] Connection timeout, resetting...");
    updateDisplay("WiFi Timeout", "Resetting...");
    delay(2000);
    ESP.restart();
  }
  
  if (currentTime - lastWifiCheckTime > WIFI_CHECK_INTERVAL) {
    Serial.println("[WiFi] Not connected, attempting to reconnect...");
    initWiFi();
    lastWifiCheckTime = currentTime;
  }
}

void waitForWiFiReady(unsigned long duration = 70000) {
  unsigned long start = millis();
  while (millis() - start < duration) {
    updateDisplay("Waiting for Router", "", "WiFi Ready in...", 
                  String((duration - (millis() - start)) / 1000) + " sec");
    delay(100);
  }
}

// Call this in setup to initialize buffer
void initWaterBuffer(int initialValue = 4095) {
  for (int i = 0; i < BUF_SIZE; i++) buf[i] = initialValue;
  bufIdx = 0;
  bufFilled = true;
}

// ===================== Setup =====================
void setup() {
  Serial.begin(115200);
  Serial.println("[SYSTEM] Initializing system...");
  
  Wire.begin(SDA_PIN, SCL_PIN);
  display.begin(OLED_ADDRESS, true);
  display.setRotation(0);
  display.setTextColor(1);
  updateDisplay("Initializing...", "", "Please wait");

  waitForWiFiReady(); // Non-blocking 70s wait
  initPins();
  initWiFi();
  initWaterBuffer();

  buzzerBeep(3);
  // No temperature sensor anymore. Show placeholder.
  updateDisplay(" ", "Temp: N/A", "READY", " ");
  Serial.println("[SYSTEM] System Ready");
  systemStartMillis = millis();  // Initialize the start time here
}

// ===================== Main Loop =====================
void loop() {
  static unsigned long lastDisplayUpdate = 0;
  static unsigned long lastOrderCheck = 0;
  static unsigned long lastWaterWarning = 0;
  const unsigned long now = millis();

  checkWiFiAndReset();

  // Check water — if no water, show warning and ensure heater is off
  if (!checkWater()) {
    if (now - lastWaterWarning >= 5000) {
      updateDisplay("Ready", "Warning!", "No water detected");
      lastWaterWarning = now;
    }
    // Send heater status = 2 (No Water) and temperature placeholder -1
    sendHeaterStatusFor(2, -1.0);
    // Ensure heater is OFF
    digitalWrite(HEATER_PIN, LOW);
    // still allow order checking but machine will not heat
  }

  // Update default display periodically
  if (now - lastDisplayUpdate >= 5000) {
    updateDisplay(" ", "Ready", " ");
    lastDisplayUpdate = now;
  }

  controlHeater();

  // Check for new orders only if not currently processing and cooldown has passed
  if (!orderInProgress && (now - lastOrderProcessTime >= ORDER_COOLDOWN)) {
    if (now - lastOrderCheck >= 3000) {
      if (WiFi.status() == WL_CONNECTED) {
        fetchAndProcessOrders();
      }
      lastOrderCheck = now;
    }
  }

  // Check if 5 hours passed, then restart ESP32
  if (millis() - systemStartMillis >= WDT_RESET_INTERVAL) {
    Serial.println("[WDT] 5 hours elapsed, restarting ESP32...");
    updateDisplay("System Restarting", "5 Hours Uptime");
    delay(2000);
    ESP.restart();
  }
}

// ===================== Initialization Functions =====================
void initPins() {
  Serial.println("[HARDWARE] Initializing pins...");
  int pins[] = { HEATER_PIN, CUP_DISPENSER_PIN, BUZZER_PIN, 
                COFFEE_SOLENOID_PIN, COFFEE_MOTOR_PIN, COFFEE_MIXER_PIN, 
                TEA_SOLENOID_PIN, TEA_MOTOR_PIN, TEA_MIXER_PIN };
  for (int pin : pins) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
  }

  pinMode(WATER_SENSOR_PIN, INPUT);
  pinMode(HEATER_BUTTON_PIN, INPUT_PULLUP); // Button with pull-up, active LOW
  Serial.println("[HARDWARE] All pins initialized");
}

void initWiFi() {
  Serial.println("[WiFi] Initializing WiFi connection...");
  WiFiManager wm;
  
  // Uncomment for testing to reset saved WiFi credentials
  // wm.resetSettings();
  
  wm.setConfigPortalTimeout(180); // 3 minutes timeout
  wm.setConnectTimeout(50); // 50 seconds connection timeout
  
  // Set custom hotspot credentials
  const char* hotspotName = "NiterCoffeeAP";
  const char* hotspotPassword = "clkbx@2k25";
  
  // Configure AP callback
  wm.setAPCallback([](WiFiManager *wm) {
    Serial.println("[WiFi] Entered config mode");
    Serial.print("[WiFi] AP SSID: ");
    Serial.println(wm->getConfigPortalSSID());
    Serial.print("[WiFi] AP Password: ");
    Serial.println("clkbx@2k25");
  });
  
  updateDisplay("Connecting to WiFi", "Please wait...");
  
  // Start configuration portal with custom parameters
  if (!wm.autoConnect(hotspotName, hotspotPassword)) {
    Serial.println("[WiFi] Failed to connect and hit timeout");
    updateDisplay("WiFi Failed", "Restarting...");
    delay(3000);
    ESP.restart();
  }
  
  Serial.println("[WiFi] Connected successfully!");
  Serial.print("[WiFi] IP Address: ");
  Serial.println(WiFi.localIP());
  wifiConnected = true;
  lastWifiCheckTime = millis();
  wifiConnectionStartTime = lastWifiCheckTime;
  
  updateDisplay("WiFi Connected", WiFi.localIP().toString(), "Ready");
  delay(2000);
}

// ===================== Buzzer Functions =====================
void buzzerBeep(int count) {
  Serial.printf("[BUZZER] Beeping %d times\n", count);
  for (int i = 0; i < count; i++) {
    tone(BUZZER_PIN, BUZZER_FREQ, BUZZER_DURATION);
    delay(BUZZER_DURATION + 100);
  }
  noTone(BUZZER_PIN);
}







// simple moving average
int readWaterAvg() {
  long s = 0;
  int count = bufFilled ? BUF_SIZE : bufIdx;
  if (count == 0) return 0;
  for (int i = 0; i < count; i++) s += buf[i];
  return (int)(s / count);
}

// main water check function
bool checkWater() {
  int raw = analogRead(WATER_SENSOR_PIN);
  buf[bufIdx++] = raw;
  if (bufIdx >= BUF_SIZE) bufIdx = 0, bufFilled = true;

  int avg = readWaterAvg();

  // hysteresis logic
  if (!waterPresent && avg < TH_ON) {
    waterPresent = true;
  } else if (waterPresent && avg > TH_OFF) {
    waterPresent = false;
  }

  // optional: debug print
  // Serial.printf("[SENSOR] ADC=%d avg=%d Water=%s\n", raw, avg, waterPresent ? "YES" : "NO");

  return waterPresent;
}

// No temperature sensor anymore. Provide placeholder function for compatibility.
float readTemperature() {
  // Return -1 to indicate "no sensor / not available"
  return -1.0;
}

// Heater control: ON only while button is pressed (active LOW) and water present
void controlHeater() {
  static bool heaterState = false;

  bool buttonPressed = digitalRead(HEATER_BUTTON_PIN) == LOW; // Button active LOW
  bool waterOK = checkWater();

  if (buttonPressed && waterOK) {
    if (!heaterState) {
      Serial.println("[HEATER] Button pressed + water OK → Heater ON");
      buzzerBeep(1);
    }
    digitalWrite(HEATER_PIN, HIGH);
    heaterState = true;
    // send heater status ON (1) with placeholder temperature -1
    sendHeaterStatusFor(1, -1.0);
  } else {
    if (heaterState) {
      Serial.println("[HEATER] Heater OFF");
    }
    digitalWrite(HEATER_PIN, LOW);
    heaterState = false;
    // send heater status OFF (0) with placeholder temperature -1
    sendHeaterStatusFor(0, -1.0);
  }
}

// ===================== Network Functions =====================
void sendHeaterStatusFor(int status, float temperature) {
  static unsigned long lastSendTime = 0;
  unsigned long currentTime = millis();
  
  // Throttle status updates to avoid spamming the server
  if (currentTime - lastSendTime < 5000 && status != 2) {
    return;
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Not connected, cannot send heater status");
    return;
  }

  HTTPClient http;
  String url = String("http://") + serverHost + statusPath;

  http.begin(url);
  http.setTimeout(3000);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  // Format temperature to 2 decimal places; use -1.00 when no sensor
  String tempStr = String(temperature, 2);
  String postData = "status=" + String(status) + "&temperature=" + tempStr;
  Serial.printf("[HTTP] Sending heater status: %s\n", postData.c_str());
  
  int httpResponseCode = http.POST(postData);

  if (httpResponseCode > 0) {
    Serial.printf("[HTTP] Heater status sent successfully. Status=%d, Temp=%s | Response: %d\n", 
                 status, tempStr.c_str(), httpResponseCode);
    lastSendTime = currentTime;
  } else {
    Serial.printf("[HTTP] Error sending heater status: %s\n", http.errorToString(httpResponseCode).c_str());
  }

  http.end();
}

void sendDeliveryConfirmation(const String& orderId, int productId, int quantity) {
  Serial.printf("[ORDER] Sending delivery confirmation for order %s (product %d, qty %d)\n", 
               orderId.c_str(), productId, quantity);
               
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Not connected, cannot send delivery confirmation");
    return;
  }

  HTTPClient http;
  String url = String("http://") + serverHost + deliveryPath;

  http.begin(url);
  http.setTimeout(3000);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> jsonDoc;
  jsonDoc["id"] = orderId;
  jsonDoc["product_id"] = productId;
  jsonDoc["product_quantity"] = quantity;
  jsonDoc["delivery_time"] = millis();

  String jsonPayload;
  serializeJson(jsonDoc, jsonPayload);
  Serial.printf("[HTTP] Delivery confirmation payload: %s\n", jsonPayload.c_str());

  int httpResponseCode = http.POST(jsonPayload);

  if (httpResponseCode > 0) {
    Serial.printf("[HTTP] Delivery confirmation sent successfully, response: %d\n", httpResponseCode);
    String response = http.getString();
    Serial.printf("[HTTP] Server response: %s\n", response.c_str());
  } else {
    Serial.printf("[HTTP] Error sending delivery confirmation: %s\n", http.errorToString(httpResponseCode).c_str());
  }

  http.end();
}

int lastProcessedOrderId = 0;

void fetchAndProcessOrders() {
  Serial.println("[ORDER] Checking for new orders...");

  if (WiFi.status() != WL_CONNECTED || orderInProgress) {
    Serial.println("[ORDER] Skipping - WiFi not connected or order in progress");
    return;
  }

  HTTPClient http;
  String url = String("http://") + serverHost + fetchPath;

  http.begin(url);
  http.setTimeout(3000);
  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[HTTP] GET failed, error: %s\n", http.errorToString(httpCode).c_str());
    http.end();
    return;
  }

  String response = http.getString();
  http.end();

  if (response == "[{\"id\":\"0\",\"product_id\":\"0\",\"product_quantity\":0}]" ||
      response == "[0]" ||
      response == "[]") {
    Serial.println("[ORDER] No orders available");
    return;
  }

  Serial.printf("[HTTP] Received order data: %s\n", response.c_str());

  int start = response.indexOf('[');
  int end = response.lastIndexOf(']') + 1;
  if (start == -1 || end == -1) {
    Serial.println("[JSON] Invalid JSON format in response");
    return;
  }

  String json = response.substring(start, end);
  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, json);
  if (error) {
    Serial.println("[JSON] Parse error: " + String(error.c_str()));
    return;
  }

  JsonArray orders = doc.as<JsonArray>();
  if (orders.size() == 0) {
    Serial.println("[ORDER] No new orders found");
    return;
  }

  orderInProgress = true;

  for (JsonObject order : orders) {
    if (order["id"] == 0 || order["product_id"] == 0) {
      continue;
    }

    int idStr = String(order["id"]).toInt();  // safe conversion
    int pid = order["product_id"];
    int qty = order["product_quantity"];

    // Prevent processing the same or older order
    if (idStr <= lastProcessedOrderId) { 
      Serial.printf("[ORDER] Skipping already processed order %d\n", idStr);
      continue;
    }

    if (qty <= 0) qty = 1;
    if (qty > 5) qty = 5;

    currentProductName = (pid == 1) ? "Coffee" :
                         (pid == 2) ? "Tea" :
                         (pid == 3) ? "Coffee (No Cup)" :
                         (pid == 5) ? "Tea (No Cup)" :
                         "Unknown";

    Serial.printf("[ORDER] Processing order %d: %d x %s\n", idStr, qty, currentProductName);

    bool success = true;
    for (int i = 0; i < qty; i++) {
      updateDisplay(String("Welcome! Order: ") + idStr, String("Preparing ") + currentProductName);

      if (!processSingleOrder(pid)) {
        Serial.printf("[ORDER] Failed to process order %d (attempt %d/%d)\n", idStr, i + 1, qty);
        success = false;
        break;
      }
    }

    if (success) {
      sendDeliveryConfirmation(String(idStr), pid, qty);
      Serial.printf("[ORDER] Successfully processed order %d\n", idStr);
      lastProcessedOrderId = idStr;  // Remember this ID to prevent reprocessing
    }
  }

  orderInProgress = false;
  lastOrderProcessTime = millis();
  Serial.printf("[ORDER] Order processing complete. Cooldown until %lu\n", lastOrderProcessTime + ORDER_COOLDOWN);
}

// ===================== Order Processing =====================
bool processSingleOrder(int pid) {
  Serial.printf("[ORDER] Starting single order process for product %d\n", pid);
  try {
    switch (pid) {
      case 1: deliverCoffee(); return true;
      case 2: deliverTea(); return true;
      case 3: deliverCoffeeNoCup(); return true;
      case 5: deliverTeaNoCup(); return true;
      default:
        Serial.printf("[ORDER] Unknown product ID: %d\n", pid);
        return false;
    }
  } catch (...) {
    Serial.println("[ORDER] Exception during order processing");
    return false;
  }
}

// ===================== Beverage Delivery =====================
void dispenseCup() {
  Serial.println("[MACHINE] Dispensing cup...");
  digitalWrite(CUP_DISPENSER_PIN, HIGH);
  delay(1500);
  digitalWrite(CUP_DISPENSER_PIN, LOW);
  delay(2000);
}

void deliverCoffee() {
  Serial.println("[MACHINE] Starting coffee delivery");
  buzzerBeep(1);
  dispenseCup();
  digitalWrite(COFFEE_SOLENOID_PIN, HIGH);
  digitalWrite(COFFEE_MIXER_PIN, HIGH);
  Serial.println("[MACHINE] Coffee mixer ON");
  delay(3001);
  
  digitalWrite(COFFEE_MOTOR_PIN, HIGH);
  Serial.println("[MACHINE] Coffee motor ON");
  delay(2250);
  
  digitalWrite(COFFEE_MOTOR_PIN, LOW);
  Serial.println("[MACHINE] Coffee motor OFF");
  delay(2000);
  
  digitalWrite(COFFEE_SOLENOID_PIN, LOW);
  digitalWrite(COFFEE_MIXER_PIN, LOW);
  Serial.println("[MACHINE] Coffee solenoid and mixer OFF");
  delay(4000);
  
  buzzerBeep(2);
  updateDisplay("Coffee Ready", "Thank You!");
  Serial.println("[MACHINE] Coffee delivery complete");
  delay(7000);
}

void deliverTea() {
  Serial.println("[MACHINE] Starting tea delivery");
  buzzerBeep(1);
  dispenseCup();
  digitalWrite(TEA_SOLENOID_PIN, HIGH);
  digitalWrite(TEA_MIXER_PIN, HIGH);
  Serial.println("[MACHINE] Tea mixer ON");
  delay(2500);
  
  digitalWrite(TEA_MOTOR_PIN, HIGH);
  Serial.println("[MACHINE] Tea motor ON");
  delay(2001);// milk tea 2700ms 
  
  digitalWrite(TEA_MOTOR_PIN, LOW);
  Serial.println("[MACHINE] Tea motor OFF");
  delay(2000);
  
  digitalWrite(TEA_SOLENOID_PIN, LOW);
  digitalWrite(TEA_MIXER_PIN, LOW);
  Serial.println("[MACHINE] Tea solenoid and mixer OFF");
  delay(4000);
  
  buzzerBeep(2);
  updateDisplay("Tea Ready", "Thank You!");
  Serial.println("[MACHINE] Tea delivery complete");
  delay(7000);
}

void deliverCoffeeNoCup() {
  Serial.println("[MACHINE] Starting coffee (no cup) delivery");
  buzzerBeep(1);
  digitalWrite(COFFEE_SOLENOID_PIN, HIGH);
  digitalWrite(COFFEE_MIXER_PIN, HIGH);
  Serial.println("[MACHINE] Coffee mixer ON");
  delay(2001);
  
  digitalWrite(COFFEE_MOTOR_PIN, HIGH);
  Serial.println("[MACHINE] Coffee motor ON");
  delay(2250);
  
  digitalWrite(COFFEE_MOTOR_PIN, LOW);
  Serial.println("[MACHINE] Coffee motor OFF");
  delay(3000);
  
  digitalWrite(COFFEE_SOLENOID_PIN, LOW);
  digitalWrite(COFFEE_MIXER_PIN, LOW);
  Serial.println("[MACHINE] Coffee solenoid and mixer OFF");
  delay(3000);
  
  buzzerBeep(2);
  updateDisplay("Coffee Ready", "Thank You!");
  Serial.println("[MACHINE] Coffee (no cup) delivery complete");
  delay(7000);
}

void deliverTeaNoCup() {
  Serial.println("[MACHINE] Starting tea (no cup) delivery");
  buzzerBeep(1);
  digitalWrite(TEA_SOLENOID_PIN, HIGH);
  digitalWrite(TEA_MIXER_PIN, HIGH);
  Serial.println("[MACHINE] Tea mixer ON");
  delay(1500);
  
  digitalWrite(TEA_MOTOR_PIN, HIGH);
  Serial.println("[MACHINE] Tea motor ON");
  delay(2001);
  
  digitalWrite(TEA_MOTOR_PIN, LOW);
  Serial.println("[MACHINE] Tea motor OFF");
  delay(3000);
  
  digitalWrite(TEA_SOLENOID_PIN, LOW);
  digitalWrite(TEA_MIXER_PIN, LOW);
  Serial.println("[MACHINE] Tea solenoid and mixer OFF");
  delay(3000);
  
  buzzerBeep(2);
  updateDisplay("Tea Ready", "Thank You!");
  Serial.println("[MACHINE] Tea (no cup) delivery complete");
  delay(7000);
}
