#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>            // I2C
#include <ArduinoJson.h>     // Library Manager
#include <Adafruit_SHT31.h>
#include "SPIFFS.h"          // servir index.html desde /Data

// --- Wi-Fi ---
const char* ssid     = "AppVirus";
const char* password = "alMarquez1876??";

// --- Pines ---
const int PIN_SOIL    = 36;   // ADC1_CH0 (humedad suelo, sonda capacitiva)
const int PIN_PH      = 39;   // ADC1_CH3 (módulo pH acondicionador)
const int PIN_LIGHT   = 5;    // relé luz
const int PIN_ACSENSE = 32;   // opcional detector AC

// --- Calibraciones ---
int   soil_dry = 3200;        // lectura en aire/seco
int   soil_wet = 1200;        // lectura saturado
float ph_a = -0.0048, ph_b = 13.2;  // pH = a*raw + b (tras calibración 4.01/7.00)

// --- Objetos ---
Adafruit_SHT31 sht31 = Adafruit_SHT31();
WebServer server(80);
bool lightCmd = false;

// ============ Utilidades ============
template<int N>
int medianRead(int pin) {
  static_assert(N > 0, "N must be > 0");
  int v[N];
  for (int i = 0; i < N; i++) { v[i] = analogRead(pin); delay(5); }
  for (int i = 0; i < N - 1; i++)
    for (int j = i + 1; j < N; j++)
      if (v[j] < v[i]) { int t = v[i]; v[i] = v[j]; v[j] = t; }
  return v[N / 2];
}

float soilPctFromRaw(int raw) {
  raw = constrain(raw, soil_wet, soil_dry);
  return 100.0f * (soil_dry - raw) / (soil_dry - soil_wet); // 0..100%
}

// ============ Handlers HTTP ============
void handleMetrics() {
  StaticJsonDocument<512> d;

  // Aire
  float t = sht31.readTemperature();
  float h = sht31.readHumidity();

  // Suelo
  int   soilRaw  = medianRead<9>(PIN_SOIL);
  float soilPct  = soilPctFromRaw(soilRaw);

  // pH
  int   phRaw = medianRead<15>(PIN_PH);
  float phVal = ph_a * phRaw + ph_b;

  // Luz
  bool acOn = digitalRead(PIN_ACSENSE); // si no hay sensor, quedará 0

  d["air"]["temp_c"]         = t;
  d["air"]["rh_pct"]         = h;
  d["soil"]["moisture_pct"]  = soilPct;
  d["soil"]["raw"]           = soilRaw;
  d["water"]["ph"]           = phVal;
  d["water"]["raw"]          = phRaw;
  d["light"]["commanded_on"] = lightCmd;
  d["light"]["sensed_on"]    = acOn;
  d["rssi"]                  = WiFi.RSSI();

  server.sendHeader("Cache-Control", "no-store");
  String out; serializeJson(d, out);
  server.send(200, "application/json", out);
}

void handleLight() {
  if (server.hasArg("plain")) {
    StaticJsonDocument<64> d;
    if (deserializeJson(d, server.arg("plain")) == DeserializationError::Ok) {
      lightCmd = d["on"];
      digitalWrite(PIN_LIGHT, lightCmd ? HIGH : LOW);
    }
  }
  server.send(204);
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ============ Setup / Loop ============
void setup() {
  Serial.begin(115200);

  // GPIO
  pinMode(PIN_LIGHT, OUTPUT);            digitalWrite(PIN_LIGHT, LOW);
  pinMode(PIN_ACSENSE, INPUT_PULLDOWN);  // según tu módulo

  // ADC
  analogReadResolution(12);              // 0..4095
  // analogSetPinAttenuation(PIN_SOIL, ADC_11db);
  // analogSetPinAttenuation(PIN_PH,   ADC_11db);

  // I2C + SHT31
  Wire.begin(21, 22);
  if (!sht31.begin(0x44)) {
    Serial.println("No se encontró SHT31 en 0x44");
  }

  // SPIFFS: servir /index.html subido con ESP32 Sketch Data Upload
  if (!SPIFFS.begin(true)) {
    Serial.println("Error al montar SPIFFS");
  } else {
    // Raíz "/"
    server.on("/", []() {
      File f = SPIFFS.open("/index.html", "r");
      if (!f) { server.send(500, "text/plain", "index.html no encontrado"); return; }
      server.streamFile(f, "text/html; charset=utf-8");
      f.close();
    });

    // Por si piden explícitamente /index.html
    server.on("/index.html", []() {
      File f = SPIFFS.open("/index.html", "r");
      if (!f) { server.send(500, "text/plain", "index.html no encontrado"); return; }
      server.streamFile(f, "text/html; charset=utf-8");
      f.close();
    });
  }

  // Wi-Fi
  WiFi.begin(ssid, password);
  Serial.print("Conectando a WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.print("\nIP: "); Serial.println(WiFi.localIP());

  // API
  server.on("/api/metrics", HTTP_GET,  handleMetrics);
  server.on("/api/light",   HTTP_POST, handleLight);
  
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("Servidor web iniciado.");
}

void loop() {
  server.handleClient();
}
