#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include "config.h"
#include <time.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "esp_sleep.h"
#include <vector>

// ----------------------------- CONFIGURACIÓN ---------------------------------

// Pines y hardware
static constexpr int HUMIDITY_SENSOR_PIN = 34;
static constexpr int WATER_LEVEL_SENSOR_PIN = 35;
static constexpr int ONE_WIRE_BUS = 4;
static constexpr int PUMP_PIN = 25;

// ADC mapeo
static constexpr int TIERRA_SECA = 4095;
static constexpr int TIERRA_HUMEDA = 1000;

// Device / MQTT
#ifndef DEVICE_ID
#define DEVICE_ID "device_name"
#endif

static const String MQTT_TOPIC_BASE = String("sensors/") + DEVICE_ID;
static const String MQTT_TOPIC_CONFIG = MQTT_TOPIC_BASE + "/config";
static const String MQTT_TOPIC_EVENTS = MQTT_TOPIC_BASE + "/events";
static const String MQTT_CLIENT_NAME = String("ESP32Client_") + DEVICE_ID;

// Defaults
static constexpr unsigned long DEFAULT_RIEGO_MS = 60000UL;
static constexpr float DEFAULT_UMBRAL = 70.0f;

// Sleep hours
static constexpr int HORA_INACT_START = 16; // +2 UTC = 18h CET
static constexpr int HORA_INACT_END = 8;    // +2 UTC = 10h CET

// Intervalos de muestreo no bloqueante
static constexpr unsigned long HUMEDAD_SAMPLE_INTERVAL_MS = 2000UL;
static constexpr int HUMEDAD_SAMPLE_COUNT = 3;
static constexpr unsigned long LOOP_PUBLISH_INTERVAL_MS = 5000UL;

// NTP
static constexpr unsigned long NTP_TIMEOUT_MS = 30000UL; // timeout para sincronizar NTP

// ------------------------------------------------------------------------------

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
WiFiClient espClient;
PubSubClient mqttClient(espClient);
Preferences preferences;

// Estado global
typedef struct
{
  float humedadUmbral = DEFAULT_UMBRAL;
  unsigned long duracionRiego = DEFAULT_RIEGO_MS;
  bool nivelAgua = true;
  bool bloqueoSinAgua = true;
} DeviceState;

DeviceState state;

static bool bombaEncendida = false;
static unsigned long bombaStartMillis = 0;
static float lastHumedad = 0.0f;

// Para muestreo no bloqueante
static int sampleIndex = 0;
static int belowCount = 0;
static unsigned long lastSampleMillis = 0;
static bool samplingInProgress = false;
static unsigned long lastPublishMillis = 0;

static bool ntpSynced = false;

std::vector<String> eventosPendientes;

// -------------------- UTILIDADES ----------------------------------------------

void printLog(const String &mensaje)
{
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char buffer[32];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
  Serial.print("[");
  Serial.print(buffer);
  Serial.print("] ");
  Serial.println(mensaje);
}

namespace Prefs
{
  void beginRead() { preferences.begin("riego", true); }
  void beginWrite() { preferences.begin("riego", false); }
  void end() { preferences.end(); }

  unsigned long getDuracion(unsigned long fallback = DEFAULT_RIEGO_MS)
  {
    beginRead();
    unsigned long v = fallback;
    if (preferences.isKey("duracion"))
      v = preferences.getULong("duracion", fallback);
    end();
    return v;
  }
  void setDuracion(unsigned long v)
  {
    beginWrite();
    preferences.putULong("duracion", v);
    end();
    printLog("Nueva duración guardada: " + String(v));
  }
  float getUmbral(float fallback = DEFAULT_UMBRAL)
  {
    beginRead();
    float v = fallback;
    if (preferences.isKey("umbral"))
      v = preferences.getFloat("umbral", fallback);
    end();
    return v;
  }
  void setUmbral(float v)
  {
    beginWrite();
    preferences.putFloat("umbral", v);
    end();
    printLog("Nuevo umbral guardado: " + String(v));
  }
}

// -------------------- MQTT ----------------------------------------------------

void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++)
    msg += (char)payload[i];
  printLog("MQTT Rx [" + String(topic) + "]: " + msg);

  if (String(topic) == MQTT_TOPIC_CONFIG)
  {
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, msg))
      return;
    if (doc.containsKey("umbral"))
    {
      float u = doc["umbral"].as<float>();
      state.humedadUmbral = u;
      Prefs::setUmbral(u);
    }
    if (doc.containsKey("duracion"))
    {
      unsigned long d = doc["duracion"].as<unsigned long>();
      state.duracionRiego = d;
      Prefs::setDuracion(d);
    }
  }
}

void mqttEnsureConnected()
{
  if (mqttClient.connected())
    return;
  while (!mqttClient.connected())
  {
    if (mqttClient.connect(MQTT_CLIENT_NAME.c_str(), MQTT_USER, MQTT_PASSWORD))
    {
      mqttClient.subscribe(MQTT_TOPIC_CONFIG.c_str());
      mqttClient.publish(MQTT_TOPIC_EVENTS.c_str(), "", true);
    }
    else
    {
      int rc = mqttClient.state();
      Serial.print("MQTT connect failed, code=");
      Serial.println(rc);
      printLog("Reintentando MQTT en 5s");
      delay(5000);
    }
  }
}

void publishState(float temp)
{
  DynamicJsonDocument doc(256);
  doc["humedad"] = lastHumedad;
  doc["temperatura"] = temp;
  doc["umbral"] = state.humedadUmbral;
  doc["duracion"] = state.duracionRiego;
  doc["nivel_agua"] = state.nivelAgua;
  char buf[256];
  size_t n = serializeJson(doc, buf);
  mqttClient.publish(MQTT_TOPIC_BASE.c_str(), buf, n);
}

void publishEvent(const char *json, bool retain = false)
{
  mqttClient.publish(MQTT_TOPIC_EVENTS.c_str(), json, retain);
}

void addEvent(const char *evento)
{
  // Obtener fecha actual en formato ISO
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char fecha[32];
  strftime(fecha, sizeof(fecha), "%Y-%m-%dT%H:%M:%S%z", &timeinfo);

  // Guardar evento como JSON string
  DynamicJsonDocument docEv(64);
  docEv["evento"] = evento;
  docEv["fecha"] = fecha;
  String evStr;
  serializeJson(docEv, evStr);

  eventosPendientes.push_back(evStr);

  // Publicar array de eventos
  DynamicJsonDocument doc(256);
  JsonArray arr = doc.to<JsonArray>();
  for (const auto &ev : eventosPendientes)
    arr.add(ev);
  char buf[256];
  size_t n = serializeJson(doc, buf);
  mqttClient.publish(MQTT_TOPIC_EVENTS.c_str(), buf, true);
}

void removeEvent(const char *evento)
{
  eventosPendientes.erase(
      std::remove_if(
          eventosPendientes.begin(),
          eventosPendientes.end(),
          [evento](const String &evStr)
          {
            DynamicJsonDocument docEv(64);
            DeserializationError err = deserializeJson(docEv, evStr);
            if (err)
              return false;
            return docEv["evento"] == evento;
          }),
      eventosPendientes.end());
}

// -------------------- NTP / ZONA HORARIA ------------------------------------

// Intenta sincronizar la hora usando NTP y establece TZ a España (CET/CEST).
// Devuelve true si la sincronización se completó dentro del timeout.
bool syncTimeSpain(unsigned long timeoutMs = NTP_TIMEOUT_MS)
{
  // España: UTC+1 (CET) / UTC+2 (CEST)
  setenv("TZ", "CET-1CEST-2,M3.5.0/02:00:00,M10.5.0/03:00:00", 1);
  tzset();

  // Configura servidores NTP
  configTime(0, 0, "pool.ntp.org", "time.google.com", "ntp.ubuntu.com");

  unsigned long start = millis();
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  // Esperar hasta que la hora sea válida o se acabe el timeout
  while ((timeinfo.tm_year < (2020 - 1900)) && (millis() - start) < timeoutMs)
  {
    delay(500);
    now = time(nullptr);
    localtime_r(&now, &timeinfo);
    Serial.print('.');
  }

  if (timeinfo.tm_year < (2020 - 1900))
  {
    printLog("No se pudo sincronizar hora NTP dentro del timeout.");
    return false;
  }

  char buf[64];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &timeinfo);
  printLog(String("Hora NTP sincronizada: ") + buf);
  return true;
}

// -------------------- TIEMPO / SLEEP ----------------------------------------

bool esHoraDeDormir()
{
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  return (t.tm_hour >= HORA_INACT_START || t.tm_hour < HORA_INACT_END);
}

uint64_t microsHastaLas10()
{
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  struct tm next = t;
  if (t.tm_hour >= HORA_INACT_START)
    next.tm_mday += 1;
  next.tm_hour = HORA_INACT_END;
  next.tm_min = 0;
  next.tm_sec = 0;
  double secs = difftime(mktime(&next), now);
  if (secs < 0)
    return 0;
  return (uint64_t)(secs * 1e6);
}

// -------------------- SENSORES Y RIEGO -------------------------------------

void checkWaterLevel()
{
  int nivel = analogRead(WATER_LEVEL_SENSOR_PIN);
  bool prev = state.nivelAgua;
  state.nivelAgua = nivel < 100;
  if (nivel > 4000)
    state.nivelAgua = false;
  if (!state.nivelAgua)
    state.bloqueoSinAgua = true;
  if (state.nivelAgua && state.bloqueoSinAgua)
  {
    state.bloqueoSinAgua = false;
    printLog("Agua detectada, desbloqueando riego.");
  }
  if (prev != state.nivelAgua)
  {
    printLog(String("Nivel agua: ") + (state.nivelAgua ? "SI" : "NO") + " (ADC=" + String(nivel) + ")");
  }
}

void startPump()
{
  digitalWrite(PUMP_PIN, HIGH);
  bombaEncendida = true;
  bombaStartMillis = millis();
  printLog("Bomba ENCENDIDA (humedad < " + String(state.humedadUmbral) + ")");
}

void stopPump(const String &reason)
{
  digitalWrite(PUMP_PIN, LOW);
  bombaEncendida = false;
  printLog("Bomba APAGADA: " + reason);
}

// -------------------- SETUP --------------------------------------------------

void setup()
{
  Serial.begin(115200);
  pinMode(HUMIDITY_SENSOR_PIN, INPUT);
  pinMode(WATER_LEVEL_SENSOR_PIN, INPUT);
  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, LOW);
  sensors.begin();

  state.humedadUmbral = Prefs::getUmbral(DEFAULT_UMBRAL);
  state.duracionRiego = Prefs::getDuracion(DEFAULT_RIEGO_MS);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Conectando a WiFi");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 10)
  {
    delay(500);
    Serial.print('.');
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    printLog("WiFi conectado.");
    // Intentar sincronizar hora solo si hay WiFi
    ntpSynced = syncTimeSpain(NTP_TIMEOUT_MS);
  }
  else
  {
    printLog("No se pudo conectar a WiFi tras 10 intentos. Se omite NTP.");
    ntpSynced = false;
  }

  // MQTT
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttEnsureConnected();

  // Publicar estado inicial / evento NTP si falló
  if (!ntpSynced)
  {
    addEvent("ntp_error");
  }
  else
  {
    eventosPendientes.clear();
    addEvent("init_ok");
  }
}

// -------------------- LOOP NO BLOQUEANTE ------------------------------------

void loop()
{
  checkWaterLevel();

  if (esHoraDeDormir())
  {
    // Elimina eventos de bomba y riego antes de dormir
    removeEvent("pump_on");
    removeEvent("pump_off_done");
    removeEvent("pump_off_no_water");
    removeEvent("pump_blocked_no_water");
    addEvent("sleep_nocturno");

    // Espera suficiente para asegurar envío MQTT
    unsigned long t0 = millis();
    while (millis() - t0 < 2000)
    { // 2 segundos
      mqttClient.loop();
      delay(10);
    }

    esp_sleep_enable_timer_wakeup(microsHastaLas10());
    esp_deep_sleep_start();
  }

  mqttEnsureConnected();
  mqttClient.loop();

  if (bombaEncendida)
  {
    int nivel = analogRead(WATER_LEVEL_SENSOR_PIN);
    if (nivel > 4000)
    {
      stopPump("SIN AGUA");
      state.bloqueoSinAgua = true;
      removeEvent("pump_on");
      removeEvent("pump_off_done");
      addEvent("pump_off_no_water");
      return;
    }
    if (millis() - bombaStartMillis >= state.duracionRiego)
    {
      stopPump("Duración completada");
      removeEvent("pump_on");
      removeEvent("pump_off_no_water");
      addEvent("pump_off_done");
    }
    return;
  }

  unsigned long now = millis();
  if (!samplingInProgress)
  {
    samplingInProgress = true;
    sampleIndex = 0;
    belowCount = 0;
    lastSampleMillis = now;
  }

  if (samplingInProgress && now - lastSampleMillis >= HUMEDAD_SAMPLE_INTERVAL_MS)
  {
    lastSampleMillis = now;
    int sensorValue = analogRead(HUMIDITY_SENSOR_PIN);
    float humedad = (float)(TIERRA_SECA - sensorValue) / (TIERRA_SECA - TIERRA_HUMEDA) * 100.0f;
    humedad = constrain(humedad, 0.0f, 100.0f);
    lastHumedad = humedad;
    if (humedad < state.humedadUmbral)
      belowCount++;
    sampleIndex++;
    if (sampleIndex >= HUMEDAD_SAMPLE_COUNT)
    {
      samplingInProgress = false;
      // Elimina eventos de bomba antes de iniciar nuevo riego
      removeEvent("pump_off_done");
      removeEvent("pump_off_no_water");
      removeEvent("pump_blocked_no_water");
      if (belowCount == HUMEDAD_SAMPLE_COUNT && !state.bloqueoSinAgua)
      {
        startPump();
        addEvent("pump_on");
      }
      else if (belowCount == HUMEDAD_SAMPLE_COUNT && state.bloqueoSinAgua)
      {
        printLog("Intento de riego bloqueado: sin agua");
        addEvent("pump_blocked_no_water");
      }
    }
  }

  if (now - lastPublishMillis >= LOOP_PUBLISH_INTERVAL_MS)
  {
    lastPublishMillis = now;
    sensors.requestTemperatures();
    float temp = sensors.getTempCByIndex(0);
    publishState(temp);
  }
}
