#include <WiFi.h>
#include <HTTPClient.h>
#include <ModbusMaster.h>
#include <time.h>

// WiFi
const char* ssid = "Tta888";
const char* password = "indonesia1";

// ThingSpeak
const char* server = "http://api.thingspeak.com/update";
const char* apiKey = "LV2JBXLIH8QP46MR";

// Modbus
ModbusMaster pzem;
#define MAX485_DE_RE 4
#define UART_RX 16
#define UART_TX 17

// Relay
#define RELAY_PIN 25
#define RELAY_ACTIVE LOW
#define RELAY_INACTIVE HIGH

// Zona waktu WIB
const long gmtOffset_sec = 7 * 3600;
const int daylightOffset_sec = 0;

const unsigned long interval = 120000;
unsigned long lastMillis = 0;
bool sensor1JustActivated = true;

void preTransmission()  { digitalWrite(MAX485_DE_RE, HIGH); }
void postTransmission() { digitalWrite(MAX485_DE_RE, LOW); }

void connectToWiFi() {
  Serial.print("Menghubungkan ke WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi terhubung");
}

void checkWiFiReconnect() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi terputus! Mencoba sambung ulang...");
    WiFi.disconnect();
    WiFi.begin(ssid, password);
    unsigned long startAttemptTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
      delay(500);
      Serial.print(".");
    }
    Serial.println(WiFi.status() == WL_CONNECTED ? "\nWiFi tersambung ulang." : "\nGagal sambung ulang.");
  }
}

void syncTimeNTP() {
  configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWaktu NTP tersinkronisasi");
}

void waitToNextSecondZero() {
  struct tm timeinfo;
  getLocalTime(&timeinfo);
  int sec = timeinfo.tm_sec;
  delay((60 - sec) * 1000);
  lastMillis = millis();
}

// ✅ Sensor ID 1 hanya aktif jam 07:00 - 17:00
bool isSensor1Active() {
  struct tm timeinfo;
  getLocalTime(&timeinfo);
  int h = timeinfo.tm_hour;
  return (h >= 7 && h < 17);
}

void readSensor(uint8_t slaveID, float &voltage, float &current, float &power) {
  pzem.begin(slaveID, Serial2);

  voltage = current = power = 0;
  uint8_t result;

  result = pzem.readInputRegisters(0x0000, 1);
  if (result == pzem.ku8MBSuccess)
    voltage = pzem.getResponseBuffer(0x0000) * 0.01;
  else
    Serial.printf("❌ Gagal baca tegangan (slave %d)\n", slaveID);

  result = pzem.readInputRegisters(0x0001, 1);
  if (result == pzem.ku8MBSuccess)
    current = pzem.getResponseBuffer(0x0000) * 0.01;
  else
    Serial.printf("❌ Gagal baca arus (slave %d)\n", slaveID);

  result = pzem.readInputRegisters(0x0002, 2);
  if (result == pzem.ku8MBSuccess) {
    uint32_t raw = ((uint32_t)pzem.getResponseBuffer(1) << 16) | pzem.getResponseBuffer(0);
    power = raw * 0.1;
  } else
    Serial.printf("❌ Gagal baca daya (slave %d)\n", slaveID);
}

bool kirimKeThingSpeak(float v1, float c1, float p1, float v2, float c2, float p2, float v3, float c3) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = String(server) + "?api_key=" + apiKey +
                 "&field1=" + String(v1, 2) +
                 "&field2=" + String(c1, 2) +
                 "&field3=" + String(p1, 1) +
                 "&field4=" + String(v2, 2) +
                 "&field5=" + String(c2, 2) +
                 "&field6=" + String(p2, 1) +
                 "&field7=" + String(v3, 2) +
                 "&field8=" + String(c3, 2);
    Serial.println("URL: " + url);
    http.begin(url);
    int httpResponseCode = http.GET();
    http.end();
    if (httpResponseCode > 0) {
      Serial.printf("✅ Data terkirim ke ThingSpeak (HTTP %d)\n", httpResponseCode);
      return true;
    } else {
      Serial.printf("❌ Gagal kirim (%d). Akan dicoba ulang.\n", httpResponseCode);
      return false;
    }
  } else {
    Serial.println("❌ WiFi tidak tersedia untuk kirim data.");
    return false;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(MAX485_DE_RE, OUTPUT);
  digitalWrite(MAX485_DE_RE, LOW);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_INACTIVE);

  Serial2.begin(9600, SERIAL_8N2, UART_RX, UART_TX);
  pzem.begin(1, Serial2);
  pzem.preTransmission(preTransmission);
  pzem.postTransmission(postTransmission);

  connectToWiFi();
  syncTimeNTP();
  waitToNextSecondZero();
}

void loop() {
  static bool lastSensor1Active = false;
  bool currentSensor1Active = isSensor1Active();

  if (currentSensor1Active && !lastSensor1Active) {
    Serial.println("Sensor 1 baru aktif, menunggu 10 detik untuk stabilisasi...");
    delay(10000);
    sensor1JustActivated = false;
  }

  lastSensor1Active = currentSensor1Active;

  checkWiFiReconnect();

  if (millis() - lastMillis >= interval) {
    lastMillis = millis();

    float v1 = 0, c1 = 0, p1 = 0;
    float v2 = 0, c2 = 0, p2 = 0;
    float v3 = 0, c3 = 0, p3 = 0;

    if (isSensor1Active()) readSensor(1, v1, c1, p1);
    delay(1000);
    readSensor(2, v2, c2, p2);
    delay(1000);
    readSensor(3, v3, c3, p3);
    delay(1000);

    // ✅ Nyalakan Relay jika Tegangan Sensor ID 2 > 27V (Relay Aktif LOW)
    digitalWrite(RELAY_PIN, v2 > 26 ? RELAY_ACTIVE : RELAY_INACTIVE);
    Serial.printf("Relay: %s\n", v2 > 25.0 ? "ON (LOW)" : "OFF (HIGH)");

    // ✅ Kirim data ke ThingSpeak
    kirimKeThingSpeak(v1, c1, p1, v2, c2, p2, v3, c3);
  }
}
