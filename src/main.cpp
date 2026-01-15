/************************************************************
 *  SMART HELMET – FINAL PRODUCTION + GSM DEBUG
 *  Board  : ESP32 (esp32dev)
 *  Sensors: MQ-2 | MPU6050 | GPS | SIM800L
 *  Backend: Firebase Realtime Database
 ************************************************************/

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <I2Cdev.h>
#include <MPU6050.h>
#include <TinyGPSPlus.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

/* ================= WIFI ================= */
#define WIFI_SSID     "FB KLASS"
#define WIFI_PASSWORD "Pakistan@2025"

/* ================= FIREBASE ================= */
#define API_KEY       "AIzaSyCbC4VuqePApw0s1FU5mG8RYfsj5fL1yUU"
#define DATABASE_URL  "https://smarthelmet-7f2f3-default-rtdb.firebaseio.com"

/* ================= PINS ================= */
#define MPU_SDA 21
#define MPU_SCL 22
#define GAS_ANALOG_PIN 35
#define GPS_RX 16
#define GPS_TX 17
#define GSM_RX 26
#define GSM_TX 27
#define BUZZER_PIN 4
#define LED_PIN 19

/* ================= CONSTANTS ================= */
#define SMOKE_THRESHOLD   700
#define JERK_THRESHOLD    0.0220
#define IMPACT_COOLDOWN   15000
#define ALERT_DURATION    4000

const char PHONE_NUMBER[] = "+93163660757";

/* ================= OBJECTS ================= */
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

MPU6050 mpu;
TinyGPSPlus gps;

/* ================= STATE ================= */
int16_t ax, ay, az;
int smokeBaseline = 0;
bool lastSmokeState = false;
bool simReady = false;
bool accidentLock = false;

unsigned long bootTime = 0;
unsigned long lastImpactMillis = 0;
unsigned long alertStartMillis = 0;
bool alertActive = false;

/* ================= GSM HELPERS ================= */
void flushGSM() {
  while (Serial1.available()) Serial1.read();
}

String readGSM(unsigned long timeout = 5000) {
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < timeout) {
    while (Serial1.available()) {
      resp += char(Serial1.read());
    }
  }
  return resp;
}

bool sendAT(const char *cmd, unsigned long timeout = 5000) {
  flushGSM();
  Serial.print("➡️ AT: "); Serial.println(cmd);
  Serial1.println(cmd);
  String resp = readGSM(timeout);
  Serial.println(resp);
  return resp.indexOf("OK") != -1;
}

/* ================= GSM DEBUG INFO ================= */
void printGSMInfo() {
  Serial.println("\n📡 ===== GSM DEBUG INFO =====");

  sendAT("AT");
  sendAT("AT+CSQ");        // Signal
  sendAT("AT+CREG?");      // Network registration
  sendAT("AT+COPS?");      // Operator
  sendAT("AT+CPIN?");      // SIM status
  sendAT("AT+CMGF=1");     // SMS text mode

  Serial.println("📡 ===========================\n");
}

/* ================= ALERT INDICATOR ================= */
void startAlert() {
  Serial.println("🔔 ALERT ON");
  digitalWrite(BUZZER_PIN, HIGH);
    digitalWrite(LED_PIN, HIGH);

  delay(3000);
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(LED_PIN, LOW);

  alertStartMillis = millis();
  alertActive = true;
}

void updateAlert() {
  if (alertActive && millis() - alertStartMillis >= ALERT_DURATION) {
    Serial.println("🔕 ALERT OFF");
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(LED_PIN, LOW);
    alertActive = false;
  }
}

/* ================= GSM ACTIONS ================= */
bool sendSMS(const String &msg) {
  Serial.println("📩 Sending SMS...");
  flushGSM();

  Serial1.print("AT+CMGS=\"");
  Serial1.print(PHONE_NUMBER);
  Serial1.println("\"");

  unsigned long start = millis();
  while (millis() - start < 5000) {
    if (Serial1.available()) {
      if (Serial1.read() == '>') break;
    }
  }

  Serial1.print(msg);
  Serial1.write(26); // CTRL+Z

  String resp = readGSM(15000);
  Serial.println(resp);

  if (resp.indexOf("+CMGS") != -1) {
    Serial.println("✅ SMS DELIVERED");
    return true;
  } else {
    Serial.println("❌ SMS FAILED");
    return false;
  }
}

void makeCall() {
  Serial.println("📞 Calling...");
  Serial1.print("ATD");
  Serial1.print(PHONE_NUMBER);
  Serial1.println(";");
}

void hangCall() {
  Serial.println("📴 Hanging call");
  Serial1.println("ATH");
}

/* ================= HELPERS ================= */
String makeLocationMessage() {
  if (gps.location.isValid()) {
    return "ACCIDENT ALERT!\nhttps://maps.google.com/?q=" +
           String(gps.location.lat(), 6) + "," +
           String(gps.location.lng(), 6);
  }
  return "ACCIDENT ALERT! Location not fixed";
}

/* ================= FIREBASE ================= */
void pushLiveData(float jerk, float netAcc,
                  float axg, float ayg, float azg,
                  int gas, bool smoke,
                  bool gpsFix, double lat, double lng) {

  FirebaseJson json;
  json.set("gas", gas);
  json.set("smoke", smoke);
  json.set("jerk", jerk);
  json.set("net_acc", netAcc);
  json.set("ax", axg);
  json.set("ay", ayg);
  json.set("az", azg);
  json.set("accident", accidentLock);
  json.set("sim_ready", simReady);
  json.set("gps_fix", gpsFix);

  if (gpsFix) {
    json.set("latitude", lat);
    json.set("longitude", lng);
  }

  json.set("timestamp/.sv", "timestamp");

  Firebase.RTDB.updateNode(&fbdo, "/helmet/live", &json);
}

void pushAlertHistory(
  const String &type,
  const String &message,
  float jerk,
  float netAcc,
  bool gpsFix,
  double lat,
  double lng
) {
  FirebaseJson alert;

  alert.set("type", type);                 // "accident" | "smoke"
  alert.set("message", message);
  alert.set("jerk", jerk);
  alert.set("net_acc", netAcc);
  alert.set("gps_fix", gpsFix);

  if (gpsFix) {
    alert.set("latitude", lat);
    alert.set("longitude", lng);
  }

  alert.set("timestamp/.sv", "timestamp");

  Firebase.RTDB.pushJSON(
    &fbdo,
    "/helmet/alerts_history",
    &alert
  );

  Serial.println("📌 Alert saved to history");
}


/* ================= ALERT LOGIC ================= */
void triggerAccident(float jerk, float netAcc) {
  Serial.println("🚨🚨🚨 ACCIDENT DETECTED 🚨🚨🚨");
  accidentLock = true;
  lastImpactMillis = millis();

  startAlert();
  printGSMInfo();
    bool gpsFix = gps.location.isValid();
  double lat = gpsFix ? gps.location.lat() : 0.0;
  double lng = gpsFix ? gps.location.lng() : 0.0;

  
 pushAlertHistory(
    "accident",
    "Accident detected",
    jerk,
    netAcc,
    gpsFix,
    lat,
    lng
  );
  if (simReady) {
    if (sendSMS(makeLocationMessage())) {
      delay(15000);
      makeCall();
      delay(20000);
      hangCall();
    }
  } else {
    Serial.println("❌ SIM NOT READY");
  }
}

/* ================= SETUP ================= */
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n🚀 SMART HELMET BOOTING (FINAL DEBUG)");

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("📡 Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(200); Serial.print(".");
  }
  Serial.println("\n✅ WiFi Connected");

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  Firebase.signUp(&config, &auth, "", "");
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  Serial.println("🔥 Firebase Connected");

  long sum = 0;
  for (int i = 0; i < 100; i++) {
    sum += analogRead(GAS_ANALOG_PIN);
    delay(15);
  }
  smokeBaseline = sum / 100;
  Serial.print("🧪 Smoke baseline: ");
  Serial.println(smokeBaseline);

  Wire.begin(MPU_SDA, MPU_SCL);
  mpu.initialize();
  Serial.println(mpu.testConnection() ? "📐 MPU6050 OK" : "❌ MPU6050 FAIL");

  Serial2.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  Serial.println("🛰️ GPS Started");

  Serial1.begin(9600, SERIAL_8N1, GSM_RX, GSM_TX);
  delay(15000);

  printGSMInfo();

  simReady = true;
  bootTime = millis();
  Serial.println("✅ SYSTEM READY\n");
}

/* ================= LOOP ================= */
void loop() {

  updateAlert();
  while (Serial2.available()) gps.encode(Serial2.read());

  int gasValue = analogRead(GAS_ANALOG_PIN);
  // ================= SMOKE ALERT =================
if (gasValue > 700 && !lastSmokeState && !alertActive) {
  Serial.println("🔥🔥 SMOKE LEVEL HIGH!");
  startAlert();
  lastSmokeState = true;
}

// Reset when smoke goes down
if (gasValue <= 650) {
  lastSmokeState = false;
}

  bool smokeNow = (gasValue - smokeBaseline) > SMOKE_THRESHOLD;
  Serial.print("🔥 GAS ADC: "); Serial.println(gasValue);

  mpu.getAcceleration(&ax, &ay, &az);
  float axg = ax / 16384.0;
  float ayg = ay / 16384.0;
  float azg = az / 16384.0;

  float mag = sqrt(axg*axg + ayg*ayg + azg*azg);
  float netAcc = abs(mag - 1.0);
  static float lastNet = 0;
  float jerk = abs(netAcc - lastNet);
  lastNet = netAcc;

  Serial.print("⚡ NetAcc: "); Serial.print(netAcc, 4);
  Serial.print(" | Jerk: "); Serial.println(jerk, 4);

  if (jerk > JERK_THRESHOLD &&
      millis() - lastImpactMillis > IMPACT_COOLDOWN &&
      millis() - bootTime > 10000 &&
      !accidentLock) {

    triggerAccident(jerk, netAcc);
  }

  if (accidentLock && millis() - lastImpactMillis > IMPACT_COOLDOWN) {
    Serial.println("🔓 Accident lock reset");
    accidentLock = false;
  }

bool gpsFix = gps.location.isValid();
double lat = gpsFix ? gps.location.lat() : 0.0;
double lng = gpsFix ? gps.location.lng() : 0.0;

pushLiveData(
  jerk, netAcc,
  axg, ayg, azg,
  gasValue, smokeNow,
  gpsFix, lat, lng
);

  Serial.println("====================================");
  delay(800);
}
