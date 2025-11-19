#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "SPIFFS.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <DHT.h>
#include "time.h"  // ⏰ para obtener la fecha y hora

// ==== Firebase ====
const char* FB_HOST = "https://greenhousefuture-73514-default-rtdb.firebaseio.com";
const char* FB_PATH = "/esp32/metrics.json";

// Wi-Fi
constexpr char WIFI_SSID[] = "AppVirus";
constexpr char WIFI_PASS[] = "alMarquez1876??";
constexpr char WIFI_HOST[] = "GreenhouseFuture";
constexpr char TEST_URL[]  = "http://example.com/";

// Pines
constexpr int PIN_SOIL        = 32;
constexpr int PIN_WATER_LEVEL = 39;
constexpr int PIN_LIGHT       = 5;
constexpr int PIN_ACSENSE     = 33;
constexpr int PIN_LDR         = 34;

// DHT22
#define DHTPIN  4
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// Calibraciones
constexpr int SOIL_RAW_DRY     = 3200;
constexpr int SOIL_RAW_WET     = 1200;
constexpr int WATER_RAW_EMPTY  = 4000;
constexpr int WATER_RAW_FULL   = 1200;

// Web
constexpr uint16_t HTTP_PORT = 80;

// Timers
constexpr uint32_t WIFI_CHECK_MS   = 5000;
constexpr uint32_t INGEST_EVERY_MS = 60000;

#define ENABLE_CLOUD_INGEST 1

WebServer server(HTTP_PORT);
bool spiffs_ok = false;
bool lightCmd  = false;

// ------------ utilidades ------------
template<int N>
int medianRead(int pin) {
  int v[N];
  for (int i = 0; i < N; i++) {
    v[i] = analogRead(pin);
    delay(3);
  }
  for (int i = 0; i < N - 1; i++)
    for (int j = i + 1; j < N; j++)
      if (v[j] < v[i]) {
        int t = v[i];
        v[i] = v[j];
        v[j] = t;
      }
  return v[N / 2];
}

inline bool adcLooksDisconnected(int raw) {
  return (raw < 60 || raw > 4030);
}

inline float soilPctFromRaw(int raw) {
  raw = constrain(raw, SOIL_RAW_WET, SOIL_RAW_DRY);
  return 100.0f * (SOIL_RAW_DRY - raw) / float(SOIL_RAW_DRY - SOIL_RAW_WET);
}

inline float waterLevelPctFromRaw(int raw) {
  raw = constrain(raw, WATER_RAW_FULL, WATER_RAW_EMPTY);
  return 100.0f * (WATER_RAW_EMPTY - raw) / float(WATER_RAW_EMPTY - WATER_RAW_FULL);
}

// ---------- obtener fecha/hora actual ----------
String getTimeStamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "1970-01-01 00:00:00";  // fallback si no hay hora NTP
  }
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buffer);
}

void printNetInfo() {
  Serial.println("\n--- RED ---");
  Serial.printf("IP: %s\n",   WiFi.localIP().toString().c_str());
  Serial.printf("GW: %s\n",   WiFi.gatewayIP().toString().c_str());
  Serial.printf("MASK: %s\n", WiFi.subnetMask().toString().c_str());
  Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
  Serial.println("------------\n");
}

bool connectWifi(uint32_t timeout_ms = 15000) {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname(WIFI_HOST);
  Serial.printf("Conectando a \"%s\" ...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < timeout_ms) {
    Serial.print(".");
    delay(280);
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi conectado!");
    printNetInfo();
    return true;
  }
  Serial.println("No se pudo conectar al Wi-Fi");
  return false;
}

bool httpProbe(const char* url) {
  HTTPClient http;
  http.setConnectTimeout(4000);
  if (!http.begin(url)) return false;
  int code = http.GET();
  Serial.printf("HTTP GET %s -> %d\n", url, code);
  http.end();
  return code > 0;
}

// ---------- JSON ----------
String buildMetricsJson() {
  StaticJsonDocument<512> d;

  // aire
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t) && !isnan(h)) {
    d["air"]["temp_c"] = t;
    d["air"]["rh_pct"] = h;
    d["air"]["sensor"] = "dht22";
    d["air"]["status"] = "ok";
  } else {
    d["air"]["status"] = "no_sensor";
  }

  // suelo
  int soilRaw = medianRead<9>(PIN_SOIL);
  if (adcLooksDisconnected(soilRaw)) {
    d["soil"]["status"] = "no_sensor";
  } else {
    d["soil"]["moisture_pct"] = soilPctFromRaw(soilRaw);
    d["soil"]["raw"]          = soilRaw;
    d["soil"]["status"]       = "ok";
  }

  // nivel agua
  int waterRaw = medianRead<9>(PIN_WATER_LEVEL);
  if (adcLooksDisconnected(waterRaw)) {
    d["water"]["status"] = "no_sensor";
  } else {
    d["water"]["level_pct"] = waterLevelPctFromRaw(waterRaw);
    d["water"]["raw"]       = waterRaw;
    d["water"]["status"]    = "ok";
  }

  // luz ambiente
  int lightRaw = analogRead(PIN_LDR);
  float pct = 100.0f * (1.0f - (lightRaw / 4095.0f));
  pct = constrain(pct, 0.0f, 100.0f);
  d["env_light"]["raw"]     = lightRaw;
  d["env_light"]["percent"] = (int)pct;
  d["env_light"]["status"]  = "ok";

  // relé / ac sense
  bool acOn = digitalRead(PIN_ACSENSE);
  d["light"]["commanded_on"] = lightCmd;
  d["light"]["sensed_on"]    = acOn;

  // conexión y fecha de actualización
  d["rssi"] = WiFi.RSSI();
  d["timestamp"] = getTimeStamp();   // 🕒 NUEVO CAMPO agregado aquí

  String out;
  serializeJson(d, out);
  return out;
}

// ---------- Firebase ----------
bool pushToFirebase(const String& jsonPayload) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(4000);

  HTTPClient http;
  String url = String(FB_HOST) + FB_PATH;

  if (!http.begin(client, url)) {
    Serial.println("Firebase: begin() falló");
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  int code = http.PUT(jsonPayload);
  Serial.printf("Firebase PUT -> %d\n", code);

  if (code > 0) {
    String resp = http.getString();
    Serial.println(resp);
  }
  http.end();
  return (code >= 200 && code < 300);
}

// ---------- HTTP handlers ----------
void handleRoot() {
  if (spiffs_ok) {
    File f = SPIFFS.open("/index.html", "r");
    if (!f) {
      server.send(500, "text/plain", "index.html no encontrado en SPIFFS");
      return;
    }
    server.streamFile(f, "text/html; charset=utf-8");
    f.close();
  } else {
    server.send(200, "text/html; charset=utf-8",
      "<h1>GreenhouseFuture ESP32</h1><p>SPIFFS no montado o sin index.html</p><p>Prueba /api/metrics</p>");
  }
}

void handleMetrics() {
  String body = buildMetricsJson();
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", body);
}

void handleNotFound() {
  server.send(404, "text/plain; charset=utf-8", "Not found");
}

// ---------- setup ----------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Greenhouse Future • ESP32 (solo DHT22) ===");

  pinMode(PIN_LIGHT, OUTPUT);
  digitalWrite(PIN_LIGHT, LOW);
  pinMode(PIN_ACSENSE, INPUT_PULLDOWN);

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_SOIL,        ADC_11db);
  analogSetPinAttenuation(PIN_WATER_LEVEL, ADC_11db);
  analogSetPinAttenuation(PIN_LDR,         ADC_11db);

  dht.begin();
  spiffs_ok = SPIFFS.begin(true);

  bool ok = connectWifi(15000);

  // Configurar zona horaria y servidor NTP
  configTime(-3 * 3600, 0, "pool.ntp.org", "time.nist.gov"); // UTC-3 para Chile

  server.on("/", handleRoot);
  server.on("/index.html", handleRoot);
  server.on("/api/metrics", HTTP_GET, handleMetrics);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Servidor web iniciado.");

  if (ok) {
    httpProbe(TEST_URL);
  }
}

// ---------- loop ----------
void loop() {
  server.handleClient();

  static uint32_t lastWifiChk = 0;
  uint32_t now = millis();

  if (now - lastWifiChk > WIFI_CHECK_MS) {
    lastWifiChk = now;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Wi-Fi caído, reintentando…");
      connectWifi(8000);
    }
  }

#if ENABLE_CLOUD_INGEST
  static uint32_t lastIngest = 0;
  if (WiFi.status() == WL_CONNECTED && (now - lastIngest) > INGEST_EVERY_MS) {
    lastIngest = now;
    String json = buildMetricsJson();
    bool ok = pushToFirebase(json);
    Serial.printf("Ingest a Firebase: %s\n", ok ? "OK" : "FALLÓ");
  }
#endif
}
