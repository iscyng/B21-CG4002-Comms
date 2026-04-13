#include <WiFi.h>
#include <AsyncMqttClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BNO055.h>
#include <utility/imumaths.h>

// Initialize the BNO055 sensor (ID 55, default I2C address 0x28)
Adafruit_BNO055 bno = Adafruit_BNO055(55, 0x28, &Wire);

unsigned long lastIMUSend = 0;
int sequence_num = 1;

// --- VIBRATOR SETTINGS ---
#define VIB_PIN 9 // <--- UPDATE THIS IF YOU USE A DIFFERENT PIN
unsigned long vibrateStartTime = 0;
bool isVibrating = false;

// --- TIMER INTERRUPT ---
volatile bool timerTriggered = false;
hw_timer_t *timer = NULL;

void IRAM_ATTR onTimer() {
  timerTriggered = true;
}

// --- CONFIGURATION ---
#define WIFI_SSID "XXXX"
#define WIFI_PASSWORD "XXXX"
#define MQTT_HOST "XXXX" // <--- IP Addr of EC2 instance
#define MQTT_PORT 1883

AsyncMqttClient mqttClient;
TimerHandle_t mqttReconnectTimer;
TimerHandle_t wifiReconnectTimer;

// Must outlive setCl ientId() — AsyncMqttClient does NOT copy the string
static String clientId;

void connectToWifi() {
  Serial.println("Connecting to Wi-Fi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void connectToMqtt() {
  static unsigned long lastAttempt = 0;
  if (mqttClient.connected()) return;
  if (millis() - lastAttempt < 2000) return;   // rate-limit reconnect attempts
  lastAttempt = millis();
  Serial.println("Connecting to MQTT...");
  mqttClient.connect();
}

void onWifiConnect(WiFiEvent_t event, WiFiEventInfo_t info) {
  Serial.println("✅ Connected to Wi-Fi!");
  Serial.print("📍 FireBeetle IP Address: ");
  Serial.println(WiFi.localIP());
  // Don't call connectToMqtt() directly — GOT_IP can fire multiple times.
  // Schedule it through the timer instead.
  xTimerStop(wifiReconnectTimer, 0);
  xTimerStart(mqttReconnectTimer, 0);
}

void onWifiDisconnect(WiFiEvent_t event, WiFiEventInfo_t info) {
  Serial.println("Disconnected from Wi-Fi.");
  xTimerStop(mqttReconnectTimer, 0);  // stop trying MQTT while wifi is down
  xTimerStop(wifiReconnectTimer, 0);
  xTimerStart(wifiReconnectTimer, 0);
}

void onMqttConnect(bool sessionPresent) {
  Serial.println("Connected to MQTT Broker!");
  mqttClient.subscribe("wand/cmd", 0);
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  Serial.print("Disconnected from MQTT, reason: ");
  Serial.println((int)reason);
  if (WiFi.isConnected()) xTimerStart(mqttReconnectTimer, 0);
}

// --- LIGHTWEIGHT IOT ENCRYPTION ---
String xorCipher(String data) {
  char key[] = "NusEngineering";
  String output = data;
  for (int i = 0; i < data.length(); i++) {
    output[i] = data[i] ^ key[i % (sizeof(key) - 1)];
  }
  return output;
}

void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
  String raw_message = "";
  for (int i = 0; i < len; i++) {
    raw_message += (char)payload[i];
  }

  String message = xorCipher(raw_message);

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, message);

  if (!error) {
    // --- CHECK FOR VIBRATE COMMAND ---
    if (doc.containsKey("action") && doc["action"] == "vibrate") {
      digitalWrite(VIB_PIN, HIGH);
      isVibrating = true;
      vibrateStartTime = millis();
      Serial.println("📳 VIBRATE COMMAND RECEIVED!");
    }

    // (Existing dummy reply logic preserved)
    if (doc.containsKey("sequence")) {
      int seq = doc["sequence"];
      float ax = doc["payload"]["ax"];
      float ay = doc["payload"]["ay"];
      float az = doc["payload"]["az"];

      StaticJsonDocument<256> responseDoc;
      responseDoc["type"] = "esp32_reply";
      responseDoc["sequence"] = seq;
      responseDoc["drawing"] = true;

      JsonObject payloadObj = responseDoc.createNestedObject("payload");
      payloadObj["ax"] = ax;
      payloadObj["ay"] = ay;
      payloadObj["az"] = az;

      responseDoc["modified_by"] = "FireBeetle_1";

      char outBuffer[256];
      serializeJson(responseDoc, outBuffer);

      String encryptedOut = xorCipher(String(outBuffer));

      mqttClient.publish("esp32/to/laptop", 0, false, encryptedOut.c_str(), encryptedOut.length());
    }
  }
}

void setup() {
  Serial.begin(115200);

  // --- Initialize Vibrator Pin ---
  pinMode(VIB_PIN, OUTPUT);
  digitalWrite(VIB_PIN, LOW);

  // --- Initialize BNO055 ---
  Wire.begin();
  if (!bno.begin()) {
    Serial.println("❌ Failed to find BNO055 chip! Check I2C wiring.");
  } else {
    Serial.println("✅ BNO055 Found and Initialized!");
    delay(100);
    bno.setExtCrystalUse(true);
  }

  // Timer setup: 1 MHz tick, fire every 20000 us = 50 Hz
  timer = timerBegin(1000000);
  timerAttachInterrupt(timer, &onTimer);
  timerAlarm(timer, 20000, true, 0);

  mqttReconnectTimer = xTimerCreate("mqttTimer", pdMS_TO_TICKS(2000), pdFALSE, (void*)0, [](TimerHandle_t t) { connectToMqtt(); });
  wifiReconnectTimer = xTimerCreate("wifiTimer", pdMS_TO_TICKS(5000), pdFALSE, (void*)0, [](TimerHandle_t t) { connectToWifi(); });

  WiFi.onEvent(onWifiConnect, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(onWifiDisconnect, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onMessage(onMqttMessage);
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);

  // --- Unique client ID derived from chip MAC ---
  clientId = "FireBeetle_Glove_" + String((uint32_t)ESP.getEfuseMac(), HEX);
  mqttClient.setClientId(clientId.c_str());
  Serial.print("MQTT Client ID: ");
  Serial.println(clientId);

  WiFi.setSleep(false);
  connectToWifi();
}

void loop() {
  // --- NON-BLOCKING VIBRATOR OFF-SWITCH ---
  if (isVibrating && (millis() - vibrateStartTime > 300)) {
    digitalWrite(VIB_PIN, LOW);
    isVibrating = false;
  }

  // Stream live IMU data on hardware timer tick
  if (mqttClient.connected() && timerTriggered) {
    timerTriggered = false;

    imu::Vector<3> accel = bno.getVector(Adafruit_BNO055::VECTOR_LINEARACCEL);
    imu::Vector<3> gyro  = bno.getVector(Adafruit_BNO055::VECTOR_GYROSCOPE);

    StaticJsonDocument<512> doc;
    doc["type"] = "wand_data";
    doc["sequence"] = sequence_num++;
    doc["drawing"] = true;

    JsonObject payload = doc.createNestedObject("payload");
    payload["ax"] = accel.x();
    payload["ay"] = accel.y();
    payload["az"] = accel.z();
    payload["gx"] = gyro.x();
    payload["gy"] = gyro.y();
    payload["gz"] = gyro.z();

    char jsonBuffer[512];
    serializeJson(doc, jsonBuffer);

    mqttClient.publish("wand/imu/raw", 0, false, jsonBuffer);
  }
}