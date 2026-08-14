#include <DHT.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// -------------------- DHT11 --------------------
#define DHTPIN 4
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// -------------------- Ultrasonic --------------------
#define TRIG_PIN 5
#define ECHO_PIN 18

// -------------------- Soil Moisture --------------------
#define SOIL_PIN 34

// -------------------- Rain Sensor --------------------
#define RAIN_PIN 35

// -------------------- Motor Relay --------------------
#define RELAY_PIN 26
// NOTE: many relay modules are "active LOW" (LOW = ON, HIGH = OFF).
// If your motor turns on when it should be off, swap HIGH/OFF below.
bool motorOn = false;

long duration;
float distance;

// Change according to your tank
float tankHeight = 13.0;

// -------------------- WiFi + Firebase --------------------
// Add your Wi-Fi credentials locally before uploading to the ESP32.
// Do NOT commit your real credentials to a public GitHub repository.
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

const char* FIREBASE_URL =
  "https://hydropulse-67721-default-rtdb.asia-southeast1.firebasedatabase.app/farms/farm1/latest.json";

const char* CONTROL_URL =
  "https://hydropulse-67721-default-rtdb.asia-southeast1.firebasedatabase.app/farms/farm1/control/mode.json";

int loopCount = 0; // used to upload every ~10th loop (about every 30s)

// Reads the dashboard's chosen mode: "auto", "on", or "off".
// Defaults to "auto" if nothing is set yet or the request fails.
String readControlMode() {
  if (WiFi.status() != WL_CONNECTED) return "auto";

  HTTPClient http;
  http.begin(CONTROL_URL);
  int code = http.GET();
  String mode = "auto";
  if (code == 200) {
    String payload = http.getString();
    payload.replace("\"", "");
    payload.trim();
    if (payload == "on" || payload == "off" || payload == "auto") {
      mode = payload;
    }
  }
  http.end();
  return mode;
}

void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected. IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi failed to connect — will retry later.");
  }
}

void uploadToFirebase(float groundwaterPercent, int soilPercent, int rainPercent, float temperature, float humidity, bool motorState, const char* motorReason) {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    return;
  }

  StaticJsonDocument<384> doc;
  doc["groundwater"]  = groundwaterPercent;
  doc["soilMoisture"] = soilPercent;
  doc["rainfall"]     = rainPercent;
  doc["temperature"]  = temperature;
  doc["humidity"]     = humidity;
  doc["updatedAt"]    = millis();
  doc["motorStatus"]  = motorState ? "on" : "off";
  doc["motorReason"]  = motorReason;

  String payload;
  serializeJson(doc, payload);

  HTTPClient http;
  http.begin(FIREBASE_URL);
  http.addHeader("Content-Type", "application/json");
  int responseCode = http.PUT(payload);

  if (responseCode > 0) {
    Serial.print("Firebase upload OK, code: ");
    Serial.println(responseCode);
  } else {
    Serial.print("Firebase upload FAILED: ");
    Serial.println(http.errorToString(responseCode));
  }
  http.end();
}

void setup() {
  Serial.begin(115200);
  dht.begin();
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); // start with motor OFF

  connectWiFi();

  Serial.println("============================================");
  Serial.println(" HYDROBYTES SENSOR MONITOR ");
  Serial.println("============================================");
}

void loop() {
  //================ DHT11 ====================
  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature();

  //================ Ultrasonic ================
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  duration = pulseIn(ECHO_PIN, HIGH);
  distance = duration * 0.0343 / 2; // raw air gap, in cm — detected normally, nothing else

  // Convert the raw distance straight into a 0–100% "how full is the tank" reading.
  // distance = 0   (sensor right at the surface) -> 100% (full)
  // distance = tankHeight (no water reflecting back) -> 0% (empty)
  float groundwaterPercent = ((tankHeight - distance) / tankHeight) * 100.0;
  if (groundwaterPercent < 0) groundwaterPercent = 0;
  if (groundwaterPercent > 100) groundwaterPercent = 100;

  // TEMP DEBUG — remove once this is working
  Serial.print("[DEBUG] raw duration: ");
  Serial.print(duration);
  Serial.print(" us  |  raw distance: ");
  Serial.print(distance);
  Serial.println(" cm");

  //================ Soil ======================
  int soilRaw = analogRead(SOIL_PIN);
  // Change these after calibration
  int soilPercent = map(soilRaw, 3200, 1200, 0, 100);
  soilPercent = constrain(soilPercent, 0, 100);

  //================ Rain ======================
  int rainRaw = analogRead(RAIN_PIN);
  // Change these after calibration
  int rainPercent = map(rainRaw, 3900, 1700, 0, 100);
  rainPercent = constrain(rainPercent, 0, 100);

  //============================================
  Serial.println("--------------------------------------------");

  // Temperature
  if (isnan(temperature))
    Serial.println("Temperature : Error");
  else {
    Serial.print("Temperature : ");
    Serial.print(temperature);
    Serial.println(" °C");
  }

  // Humidity
  if (isnan(humidity))
    Serial.println("Humidity : Error");
  else {
    Serial.print("Humidity : ");
    Serial.print(humidity);
    Serial.println(" %");
  }

  // Water Level
  Serial.print("Groundwater : ");
  Serial.print(groundwaterPercent);
  Serial.println(" %");

  // Soil Moisture
  Serial.print("Soil Moisture : ");
  Serial.print(soilPercent);
  Serial.println(" %");

  // Rain
  Serial.print("Rain Intensity : ");
  Serial.print(rainPercent);
  Serial.println(" %");

  // Rain Status
  if (rainPercent > 50)
    Serial.println("Rain Status : Rain Detected");
  else
    Serial.println("Rain Status : No Rain");

  //================ Motor Pump (auto + manual override) ================
  String controlMode = readControlMode(); // "auto", "on", or "off" — checked every loop
  const char* motorReason;

  if (controlMode == "on") {
    motorOn = true;
    motorReason = "manualOn";
  } else if (controlMode == "off") {
    motorOn = false;
    motorReason = "manualOff";
  } else {
    // Automatic mode — decide based on soil moisture and rain
    if (!motorOn && soilPercent < 30 && rainPercent < 30) {
      motorOn = true;
      motorReason = "dry";
    } else if (motorOn && soilPercent > 70) {
      motorOn = false;
      motorReason = "sufficient";
    } else if (motorOn && rainPercent >= 50) {
      motorOn = false;
      motorReason = "rain";
    } else {
      motorReason = motorOn ? "dry" : "monitor";
    }
  }
  digitalWrite(RELAY_PIN, motorOn ? HIGH : LOW); // flip HIGH/LOW if your relay is active-LOW

  Serial.print("Motor Pump : ");
  Serial.println(motorOn ? "ON" : "OFF");

  // Irrigation Recommendation
  if (soilPercent < 30 && rainPercent < 30)
    Serial.println("Recommendation : Irrigation Required");
  else if (soilPercent > 70)
    Serial.println("Recommendation : Soil Moist Enough");
  else
    Serial.println("Recommendation : Monitor Field");

  Serial.println("--------------------------------------------");

  // Upload to Firebase roughly every 30 seconds (every 10th loop, since loop runs every 3s)
  loopCount++;
  if (loopCount >= 10) {
    if (!isnan(temperature) && !isnan(humidity)) {
      uploadToFirebase(groundwaterPercent, soilPercent, rainPercent, temperature, humidity, motorOn, motorReason);
    } else {
      Serial.println("Skipping Firebase upload — sensor reading invalid.");
    }
    loopCount = 0;
  }

  delay(3000);
}
