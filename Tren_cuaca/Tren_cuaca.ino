#include <WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ThingSpeak.h>
#include <BH1750.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <DHT_U.h>
#include <Adafruit_INA219.h>
#include <Wire.h>

// --- Wi-Fi ---
const char* ssid = "Tta888";
const char* password = "indonesia1";

// --- ThingSpeak ---
const unsigned long channelID = 2847507;
const char* writeAPIKey = "1E8OJPAZQOYD2GT5";
WiFiClient client;

// --- NTP ---
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 25200, 60000); // GMT+7

// --- Sensor DHT22 ---
#define DHTPIN 15
#define DHTTYPE DHT22
DHT_Unified dht(DHTPIN, DHTTYPE);

// --- Sensor BH1750 ---
BH1750 lightMeter(0x23);

// --- Sensor INA219 ---
Adafruit_INA219 ina219(0x41);

// --- Konstanta panel surya untuk irradiance ---
const float EFFICIENCY = 0.15;
const float AREA = 0.00759;
const float CALIBRATION_FACTOR = 878.3;

// --- Timing ---
unsigned long lastSendTime = 0;
unsigned long lastWifiCheck = 0;
unsigned long lastSuccessSendTime = 0;
bool retrySending = false;
bool dataCollectionStarted = false;
unsigned long startDataCollectionTime = 0;
String initialTime;
int consecutiveFailures = 0;

// --- Helper: Dapatkan waktu untuk display ---
String getDisplayTime() {
  int hour = timeClient.getHours();
  int minute = timeClient.getMinutes();
  int second = timeClient.getSeconds();
  char timeStr[9];
  sprintf(timeStr, "%02d:%02d:%02d", hour, minute, second);
  return String(timeStr);
}

// --- Setup ---
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("🔄 Starting...");

  // Inisialisasi Wi-Fi
  WiFi.begin(ssid, password);
  Serial.print("🔌 Connecting to Wi-Fi");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 30000) {
    Serial.print(".");
    delay(300);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n❌ Gagal terhubung ke Wi-Fi. Restart...");
    ESP.restart();
  }
  Serial.println("\n✅ Connected to Wi-Fi");

  // Inisialisasi NTP
  timeClient.begin();
  Serial.print("⏳ Syncing NTP time...");
  unsigned long ntpStart = millis();
  while (!timeClient.update() && millis() - ntpStart < 10000) {
    Serial.print(".");
    timeClient.forceUpdate();
    delay(500);
  }
  if (!timeClient.isTimeSet()) {
    Serial.println("\n❌ Gagal sinkronisasi NTP. Restart...");
    ESP.restart();
  }
  Serial.println("\n✅ NTP time synced.");

  // Hitung waktu mulai pengambilan data
  unsigned long currentEpoch = timeClient.getEpochTime();
  int currentHour = timeClient.getHours();
  int currentMinute = timeClient.getMinutes();
  int currentSecond = timeClient.getSeconds();
  Serial.printf("Current time: %02d:%02d:%02d (Epoch: %lu)\n", currentHour, currentMinute, currentSecond, currentEpoch);

  int nextMinute = currentMinute + 1;
  int startHour = currentHour;
  if (nextMinute >= 60) {
    nextMinute = 0;
    startHour = (startHour + 1) % 24;
  }
  startDataCollectionTime = currentEpoch - currentSecond + (nextMinute - currentMinute) * 60;
  initialTime = String(startHour, DEC) + ":" + (nextMinute < 10 ? "0" : "") + String(nextMinute, DEC) + ":00";
  Serial.printf("⏰ Data collection will start at %02d:%02d:00 (Epoch: %lu)\n", startHour, nextMinute, startDataCollectionTime);

  // Inisialisasi ThingSpeak
  ThingSpeak.begin(client);

  // Inisialisasi I2C dan sensor
  Wire.begin();
  if (!ina219.begin()) {
    Serial.println("❌ Gagal inisialisasi INA219! Cek koneksi.");
    while (1) delay(10);
  }
  Serial.println("✅ INA219 diinisialisasi.");
  lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
  dht.begin();

  lastSuccessSendTime = millis();
}

// --- Fungsi untuk mencoba reconnect Wi-Fi ---
bool reconnectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  Serial.println("🔁 Attempting to reconnect Wi-Fi...");
  WiFi.disconnect();
  WiFi.begin(ssid, password);

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ Wi-Fi reconnected.");
    return true;
  } else {
    Serial.println("\n❌ Failed to reconnect Wi-Fi.");
    return false;
  }
}

// --- Loop utama ---
void loop() {
  unsigned long currentMillis = millis();

  // Reconnect Wi-Fi jika terputus
  if (WiFi.status() != WL_CONNECTED && currentMillis - lastWifiCheck >= 30000) {
    lastWifiCheck = currentMillis;
    reconnectWiFi();
  }

  // Periksa apakah waktu mulai pengambilan data sudah tercapai
  if (!dataCollectionStarted && timeClient.getEpochTime() >= startDataCollectionTime) {
    dataCollectionStarted = true;
    Serial.println("✅ Data collection started!");
    lastSendTime = currentMillis;
  }

  // Kirim data setiap menit
  if (dataCollectionStarted && currentMillis - lastSendTime >= 120000) {
    lastSendTime = currentMillis;
    retrySending = false;

    String currentTime = getDisplayTime();
    Serial.printf("📊 [%s] Membaca sensor cuaca...\n", currentTime.c_str());

    // Baca BH1750
    float lux = lightMeter.readLightLevel();
    if (isnan(lux) || lux < 0) {
      Serial.println("⚠️ BH1750: Gagal membaca intensitas cahaya, set ke -1.");
      lux = -1;
    }
    Serial.printf("BH1750: Intensitas Cahaya = %.2f lux\n", lux);

    // Baca DHT22
    sensors_event_t event;
    float temp = -1, hum = -1;
    dht.temperature().getEvent(&event);
    if (!isnan(event.temperature)) {
      temp = event.temperature;
    } else {
      Serial.println("⚠️ DHT22: Gagal membaca suhu, set ke -1.");
    }
    dht.humidity().getEvent(&event);
    if (!isnan(event.relative_humidity)) {
      hum = event.relative_humidity;
    } else {
      Serial.println("⚠️ DHT22: Gagal membaca kelembapan, set ke -1.");
    }
    Serial.printf("DHT22: Suhu = %.2f °C | Kelembapan = %.2f %%\n", temp, hum);

    // Baca INA219
    float busVoltage_V = ina219.getBusVoltage_V();
    float shuntVoltage_mV = ina219.getShuntVoltage_mV();
    float current_mA = ina219.getCurrent_mA();
    if (isnan(current_mA) || !isfinite(current_mA)) {
      current_mA = 0;
      Serial.println("⚠️ INA219: Arus tidak valid, set ke 0.");
    }
    float loadVoltage = busVoltage_V + (shuntVoltage_mV / 1000.0);
    float current_A = current_mA / 1000.0;
    float power_W = loadVoltage * current_A;
    float solarIrradiance = power_W * CALIBRATION_FACTOR;
    if (!isfinite(solarIrradiance)) {
      solarIrradiance = 0;
      Serial.println("⚠️ INA219: Irradiance tidak valid, set ke 0.");
    }
    Serial.printf("Mini Panel Surya (INA219): Tegangan = %.2f V | Arus = %.2f A | Daya = %.2f W | Irradiance = %.2f W/m²\n", 
                  loadVoltage, current_A, power_W, solarIrradiance);

    // Kirim ke ThingSpeak
    Serial.println("🚀 Mengirim ke ThingSpeak...");
    ThingSpeak.setField(1, temp);
    ThingSpeak.setField(2, hum);
    ThingSpeak.setField(3, lux);
    ThingSpeak.setField(4, solarIrradiance);
    ThingSpeak.setField(5, busVoltage_V);
    ThingSpeak.setField(6, current_mA);

    if (!reconnectWiFi()) {
      Serial.println("⚠️ Wi-Fi tidak terhubung. Skip pengiriman...");
      consecutiveFailures++;
    } else {
      int responseCode = -1;
      unsigned long retryStart = millis();
      unsigned long lastAttempt = 0;
      const unsigned long retryInterval = 3000;

      while (millis() - retryStart < 30000) {
        if (millis() - lastAttempt >= retryInterval || lastAttempt == 0) {
          responseCode = ThingSpeak.writeFields(channelID, writeAPIKey);
          lastAttempt = millis();
          Serial.printf("📤 Attempt sending... Response code: %d\n", responseCode);
          if (responseCode == 200) {
            Serial.println("✅ Berhasil mengirim data.");
            lastSuccessSendTime = millis();
            consecutiveFailures = 0;
            retrySending = false;
            break;
          } else {
            Serial.println("❌ Gagal kirim. Mencoba ulang...");
            retrySending = true;
          }
        }
        delay(10);
        yield();
      }

      if (responseCode != 200) {
        Serial.println("⚠️ Gagal kirim dalam 30 detik. Skip ke siklus berikutnya.");
        consecutiveFailures++;
        retrySending = false;
      }
    }

    if (consecutiveFailures >= 1) {
      Serial.println("🚨 Gagal mengirim 3 kali berturut-turut. Restart ESP...");
      ESP.restart();
    }

    Serial.println("✅ Loop selesai.\n");

    if (millis() - lastSuccessSendTime > 3600000) {
      Serial.println("🚨 Sudah 1 jam gagal terus. Restart ESP...");
      ESP.restart();
    }
  } else if (!dataCollectionStarted) {
    static unsigned long lastWaitMessage = 0;
    if (currentMillis - lastWaitMessage >= 10000) {
      lastWaitMessage = currentMillis;
      unsigned long currentEpoch = timeClient.getEpochTime();
      int startHour = timeClient.getHours();
      int startMinute = timeClient.getMinutes() + 1;
      if (startMinute >= 60) {
        startMinute = 0;
        startHour = (startHour + 1) % 24;
      }
      Serial.printf("⏳ Waiting for data collection to start at %02d:%02d:00 (Current epoch: %lu, Start epoch: %lu)\n",
                    startHour, startMinute, currentEpoch, startDataCollectionTime);
    }
  }

  yield();
}