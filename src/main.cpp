#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include "config.h"
#include <time.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <vector>

// ----------------------------- CONFIGURACIÓN ---------------------------------

// Pines y hardware
static constexpr int HUMIDITY_SENSOR_PIN    = 34;
static constexpr int WATER_LEVEL_SENSOR_PIN = 35;
static constexpr int ONE_WIRE_BUS           = 4;
static constexpr int PUMP_PIN               = 25;

// ADC mapeo humedad suelo
static constexpr int TIERRA_SECA   = 4095;
static constexpr int TIERRA_HUMEDA = 1000;

// Sensor de nivel de agua (capacitivo externo, no sumergible):
//   ADC ~0    → hay agua en el depósito (sensor conduce)
//   ADC ~4095 → sin agua                (sensor no conduce)
//   ADC 500–3500 → indeterminado / pin flotante (ignorar, no cambiar estado)
static constexpr int WATER_PRESENT_THRESHOLD = 500;   // < 500  → posible agua
static constexpr int WATER_ABSENT_THRESHOLD  = 3500;  // > 3500 → posible sin agua

// Device / MQTT
static const String MQTT_TOPIC_BASE   = String("sensors/") + DEVICE_ID;
static const String MQTT_TOPIC_CONFIG = MQTT_TOPIC_BASE + "/config";
static const String MQTT_TOPIC_EVENTS = MQTT_TOPIC_BASE + "/events";
static const String MQTT_CLIENT_NAME  = String("ESP32Client_") + DEVICE_ID;

// Defaults
static constexpr unsigned long DEFAULT_RIEGO_MS = 60000UL;
static constexpr float         DEFAULT_UMBRAL   = 70.0f;

// Intervalos
static constexpr unsigned long HUMEDAD_SAMPLE_INTERVAL_MS = 2000UL;
static constexpr int           HUMEDAD_SAMPLE_COUNT        = 3;
static constexpr unsigned long LOOP_PUBLISH_INTERVAL_MS    = 5000UL;
static constexpr unsigned long WATER_CHECK_INTERVAL_MS     = 250UL;   // max 4 lecturas/s
static constexpr unsigned long MQTT_RETRY_INTERVAL_MS      = 5000UL;  // reintento no bloqueante
static constexpr unsigned long WIFI_RECONNECT_TIMEOUT_MS   = 30000UL; // timeout reconexión WiFi

// Lecturas consecutivas para confirmar cambio de estado del sensor de agua
static constexpr int WATER_LEVEL_CONFIRM_COUNT = 5;

// NTP
static constexpr unsigned long NTP_TIMEOUT_MS = 30000UL;

// ------------------------------------------------------------------------------

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
WiFiClient espClient;
PubSubClient mqttClient(espClient);
Preferences preferences;

typedef struct
{
    float         humedadUmbral  = DEFAULT_UMBRAL;
    unsigned long duracionRiego  = DEFAULT_RIEGO_MS;
    bool          nivelAgua      = false; // espera confirmación antes de permitir riego
    bool          bloqueoSinAgua = true;  // bloqueado hasta primera confirmación de agua
} DeviceState;

DeviceState state;

static bool          bombaEncendida   = false;
static unsigned long bombaStartMillis = 0;
static float         lastHumedad      = 0.0f;

// Muestreo humedad no bloqueante
static int           sampleIndex        = 0;
static int           belowCount         = 0;
static unsigned long lastSampleMillis   = 0;
static bool          samplingInProgress = false;
static unsigned long lastPublishMillis  = 0;

// Sensor agua no bloqueante
static unsigned long lastWaterCheckMs  = 0;
static int           waterConfirmCount = 0; // >0 confirma agua, <0 confirma ausencia

// MQTT no bloqueante
static unsigned long lastMqttAttemptMs = 0;

// WiFi reconexión no bloqueante
static bool          wifiReconnecting = false;
static unsigned long wifiReconnectMs  = 0;

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
    void beginRead()  { preferences.begin("riego", true);  }
    void beginWrite() { preferences.begin("riego", false); }
    void end()        { preferences.end(); }

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
        if (deserializeJson(doc, msg)) return;
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

// No bloqueante: intenta conectar solo si ha pasado MQTT_RETRY_INTERVAL_MS
void mqttEnsureConnected()
{
    if (mqttClient.connected()) return;
    unsigned long now = millis();
    if (now - lastMqttAttemptMs < MQTT_RETRY_INTERVAL_MS) return;
    lastMqttAttemptMs = now;

    if (mqttClient.connect(MQTT_CLIENT_NAME.c_str(), MQTT_USER, MQTT_PASSWORD))
    {
        mqttClient.subscribe(MQTT_TOPIC_CONFIG.c_str());
        mqttClient.publish(MQTT_TOPIC_EVENTS.c_str(), "", true);
        printLog("MQTT conectado.");
    }
    else
    {
        printLog("MQTT connect failed, code=" + String(mqttClient.state()) + ". Reintentando en 5s.");
    }
}

void publishState(float temp)
{
    DynamicJsonDocument doc(256);
    doc["humedad"]     = lastHumedad;
    doc["temperatura"] = temp;
    doc["umbral"]      = state.humedadUmbral;
    doc["duracion"]    = state.duracionRiego;
    doc["nivel_agua"]  = state.nivelAgua;

    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char fecha[40];
    strftime(fecha, sizeof(fecha), "%d %b %Y, %H:%M:%S", &timeinfo);
    doc["timestamp"] = fecha;

    char buf[256];
    size_t n = serializeJson(doc, buf);
    mqttClient.publish(MQTT_TOPIC_BASE.c_str(), buf, n);
}

void addEvent(const char *evento)
{
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char fecha[40];
    strftime(fecha, sizeof(fecha), "%d %b %Y, %H:%M:%S", &timeinfo);

    DynamicJsonDocument docEv(64);
    docEv["evento"] = evento;
    docEv["fecha"]  = fecha;
    String evStr;
    serializeJson(docEv, evStr);
    eventosPendientes.push_back(evStr);

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
                if (err) return false;
                return docEv["evento"] == evento;
            }),
        eventosPendientes.end());
}

// -------------------- NTP / ZONA HORARIA ------------------------------------

bool syncTimeSpain(unsigned long timeoutMs = NTP_TIMEOUT_MS)
{
    setenv("TZ", "CET-1CEST-2,M3.5.0/02:00:00,M10.5.0/03:00:00", 1);
    tzset();
    configTime(0, 0, "pool.ntp.org", "time.google.com", "ntp.ubuntu.com");

    unsigned long start = millis();
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

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

// -------------------- SENSOR NIVEL AGUA -------------------------------------

// Sensor capacitivo externo (no sumergible):
//   ADC ~0    = hay agua  → sensor conduce → LOW
//   ADC ~4095 = sin agua  → sensor no conduce → HIGH
//   ADC 500–3500 = zona indeterminada (pin flotante) → mantener estado anterior
//
// Se requieren WATER_LEVEL_CONFIRM_COUNT lecturas consecutivas en la misma
// dirección para confirmar un cambio de estado (fail-safe anti-ruido).
// El contador se resetea si la lectura entra en la zona contraria.
void checkWaterLevel()
{
    unsigned long now = millis();
    if (now - lastWaterCheckMs < WATER_CHECK_INTERVAL_MS) return;
    lastWaterCheckMs = now;

    int nivel = analogRead(WATER_LEVEL_SENSOR_PIN);
    bool prev = state.nivelAgua;

    if (nivel < WATER_PRESENT_THRESHOLD)
    {
        // Posible agua — acumular confirmaciones en dirección positiva
        if (waterConfirmCount < 0) waterConfirmCount = 0;
        waterConfirmCount++;
        if (waterConfirmCount >= WATER_LEVEL_CONFIRM_COUNT)
            state.nivelAgua = true;
    }
    else if (nivel > WATER_ABSENT_THRESHOLD)
    {
        // Posible ausencia de agua — acumular confirmaciones en dirección negativa
        if (waterConfirmCount > 0) waterConfirmCount = 0;
        waterConfirmCount--;
        if (waterConfirmCount <= -WATER_LEVEL_CONFIRM_COUNT)
            state.nivelAgua = false;
    }
    // else: zona indeterminada (pin flotante) → no tocar estado ni contador

    // Si no hay agua, bloquear riego
    if (!state.nivelAgua)
        state.bloqueoSinAgua = true;

    // Transición sin agua → con agua: desbloquear riego
    if (!prev && state.nivelAgua)
    {
        state.bloqueoSinAgua = false;
        printLog("Agua detectada, desbloqueando riego.");
    }

    // Loguear solo cuando cambia el estado
    if (prev != state.nivelAgua)
        printLog(String("Nivel agua: ") + (state.nivelAgua ? "SI" : "NO") + " (ADC=" + String(nivel) + ")");
}

// -------------------- RIEGO --------------------------------------------------

void startPump()
{
    digitalWrite(PUMP_PIN, HIGH);
    bombaEncendida   = true;
    bombaStartMillis = millis();
    printLog("Bomba ENCENDIDA (humedad < " + String(state.humedadUmbral) + "%)");
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
    pinMode(HUMIDITY_SENSOR_PIN,    INPUT);
    pinMode(WATER_LEVEL_SENSOR_PIN, INPUT);
    pinMode(PUMP_PIN,               OUTPUT);
    digitalWrite(PUMP_PIN, LOW);
    sensors.begin();

    state.humedadUmbral = Prefs::getUmbral(DEFAULT_UMBRAL);
    state.duracionRiego = Prefs::getDuracion(DEFAULT_RIEGO_MS);

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Conectando a WiFi");
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 20)
    {
        delay(500);
        Serial.print('.');
        tries++;
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        printLog("WiFi conectado.");
        ntpSynced = syncTimeSpain(NTP_TIMEOUT_MS);
    }
    else
    {
        printLog("No se pudo conectar a WiFi. Se omite NTP.");
        ntpSynced = false;
    }

    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    mqttEnsureConnected();

    if (!ntpSynced)
        addEvent("ntp_error");
    else
    {
        eventosPendientes.clear();
        addEvent("init_ok");
    }
}

// -------------------- LOOP NO BLOQUEANTE ------------------------------------

void loop()
{
    // — Reconexión WiFi no bloqueante —
    if (WiFi.status() != WL_CONNECTED)
    {
        unsigned long now = millis();
        if (!wifiReconnecting)
        {
            printLog("WiFi desconectado. Reintentando...");
            WiFi.disconnect();
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
            wifiReconnecting = true;
            wifiReconnectMs  = now;
        }
        else if (WiFi.status() == WL_CONNECTED)
        {
            wifiReconnecting = false;
            printLog("WiFi reconectado.");
            ntpSynced = syncTimeSpain(NTP_TIMEOUT_MS);
            mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
            mqttClient.setCallback(mqttCallback);
        }
        else if (now - wifiReconnectMs > WIFI_RECONNECT_TIMEOUT_MS)
        {
            wifiReconnecting = false; // reinicia el ciclo de reconexión
            printLog("Timeout reconexión WiFi. Reintentando...");
        }
        // Sin WiFi seguimos monitorizando el sensor de agua
        checkWaterLevel();
        return;
    }

    // — Sensor nivel agua —
    checkWaterLevel();

    // — MQTT no bloqueante —
    mqttEnsureConnected();
    mqttClient.loop();

    // — Control bomba (prioridad máxima dentro del loop) —
    if (bombaEncendida)
    {
        int nivel = analogRead(WATER_LEVEL_SENSOR_PIN);
        if (nivel > WATER_ABSENT_THRESHOLD)
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
            stopPump("Duracion completada");
            removeEvent("pump_on");
            removeEvent("pump_off_no_water");
            addEvent("pump_off_done");
        }
        return;
    }

    // — Muestreo humedad no bloqueante —
    unsigned long now = millis();
    if (!samplingInProgress)
    {
        samplingInProgress = true;
        sampleIndex        = 0;
        belowCount         = 0;
        lastSampleMillis   = now;
    }

    if (now - lastSampleMillis >= HUMEDAD_SAMPLE_INTERVAL_MS)
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

    // — Publicar estado MQTT cada LOOP_PUBLISH_INTERVAL_MS —
    if (now - lastPublishMillis >= LOOP_PUBLISH_INTERVAL_MS)
    {
        lastPublishMillis = now;
        sensors.requestTemperatures();
        float temp = sensors.getTempCByIndex(0);
        publishState(temp);
    }
}
