#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include "config.h"
#include <time.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>

// ----------------------------- CONFIGURACIÓN ---------------------------------

// Pines y hardware
static constexpr int HUMIDITY_SENSOR_PIN    = 34;
static constexpr int WATER_LEVEL_SENSOR_PIN = 35;
static constexpr int ONE_WIRE_BUS           = 4;
static constexpr int PUMP_PIN               = 25;

// Pin alimentación sensor resistivo (encendido solo durante la lectura para reducir oxidación)
static constexpr int SENSOR_POWER_PIN = 26;

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
static constexpr unsigned long HUMEDAD_SAMPLE_INTERVAL_MS  = 2000UL;
static constexpr int           HUMEDAD_SAMPLE_COUNT         = 3;
static constexpr unsigned long SENSOR_WARMUP_MS             = 500UL;   // estabilización sensor tras encendido
static constexpr unsigned long HUMEDAD_CYCLE_INTERVAL_MS    = 30000UL; // ciclo de muestreo cada 30 s
static constexpr unsigned long DS18B20_CONVERSION_MS        = 750UL;   // tiempo de conversión 12-bit
static constexpr unsigned long WATER_CHECK_INTERVAL_MS      = 250UL;   // max 4 lecturas/s
static constexpr unsigned long MQTT_RETRY_INTERVAL_MS       = 5000UL;  // reintento no bloqueante
static constexpr unsigned long WIFI_RECONNECT_TIMEOUT_MS    = 30000UL; // timeout reconexión WiFi
static constexpr unsigned long EVENT_DEDUP_MS               = 60000UL; // cooldown entre eventos idénticos consecutivos
static constexpr unsigned long PUMP_WATER_CHECK_MS          = 250UL;   // intervalo check agua durante riego
static constexpr int           PUMP_NO_WATER_CONFIRM        = 3;       // lecturas consecutivas sin agua para parar bomba

// Lecturas consecutivas para confirmar cambio de estado del sensor de agua
static constexpr int WATER_LEVEL_CONFIRM_COUNT = 5;

// Cola de eventos pendientes cuando MQTT está desconectado
static constexpr int MAX_PENDING_EVENTS = 8;

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
static float         lastTemp         = -127.0f;

// Deduplicación de eventos: cooldown temporal por evento idéntico consecutivo
static char          lastEventSent[32] = "";
static unsigned long lastEventMs       = 0;

// Muestreo humedad no bloqueante
static int           sampleIndex        = 0;
static int           belowCount         = 0;
static float         humSum             = 0.0f;
static unsigned long lastSampleMillis   = 0;
static bool          samplingInProgress = false;
static bool          sensorPowered      = false;
static bool          sensorWarmingUp    = false;
static unsigned long sensorWarmupStart  = 0;
static unsigned long lastCycleMillis    = 0;
static bool          firstSample        = true;  // primera lectura al arrancar

// DS18B20 no bloqueante
static bool          tempRequested     = false;
static unsigned long tempRequestMillis = 0;

// Sensor agua no bloqueante
static unsigned long lastWaterCheckMs  = 0;
static int           waterConfirmCount = 0; // >0 confirma agua, <0 confirma ausencia

// MQTT no bloqueante
static unsigned long lastMqttAttemptMs = 0;

// Debounce sensor agua durante riego
static unsigned long pumpWaterCheckMs  = 0;
static int           pumpNoWaterCount  = 0;

// Cola de eventos pendientes (cuando MQTT está desconectado)
static char pendingEvents[MAX_PENDING_EVENTS][192];
static int  pendingEventCount = 0;

// WiFi reconexión no bloqueante
static bool          wifiReconnecting = false;
static unsigned long wifiReconnectMs  = 0;

static bool ntpSynced = false;



// Forward declarations (definidas en la sección NTP más abajo)
static int  getSpainOffset(time_t rawtime);
static void getLocalTime(time_t utc, struct tm *ti);

// -------------------- UTILIDADES ----------------------------------------------

void printLog(const String &mensaje)
{
    time_t now = time(nullptr);
    struct tm timeinfo;
    getLocalTime(now, &timeinfo);
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
        JsonDocument doc;
        if (deserializeJson(doc, msg)) return;
        if (doc.containsKey("umbral"))
        {
            float u = doc["umbral"].as<float>();
            if (u >= 0.0f && u <= 100.0f)
            {
                state.humedadUmbral = u;
                Prefs::setUmbral(u);
            }
            else
                printLog("Umbral rechazado (fuera de rango 0-100): " + String(u));
        }
        if (doc.containsKey("duracion"))
        {
            unsigned long d = doc["duracion"].as<unsigned long>();
            if (d >= 1000UL && d <= 600000UL)
            {
                state.duracionRiego = d;
                Prefs::setDuracion(d);
            }
            else
                printLog("Duración rechazada (fuera de rango 1000-600000ms): " + String(d));
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
        // Vaciar cola de eventos acumulados durante la desconexión
        for (int i = 0; i < pendingEventCount; i++)
            mqttClient.publish(MQTT_TOPIC_EVENTS.c_str(), pendingEvents[i]);
        pendingEventCount = 0;
        printLog("MQTT conectado.");
    }
    else
    {
        printLog("MQTT connect failed, code=" + String(mqttClient.state()) + ". Reintentando en 5s.");
    }
}

void publishState(float temp)
{
    JsonDocument doc;
    doc["humedad"]     = lastHumedad;
    if (temp == DEVICE_DISCONNECTED_C)
        doc["temperatura"] = nullptr;
    else
        doc["temperatura"] = temp;
    doc["umbral"]      = state.humedadUmbral;
    doc["duracion"]    = state.duracionRiego;
    doc["nivel_agua"]  = state.nivelAgua;

    time_t now = time(nullptr);
    struct tm timeinfo;
    getLocalTime(now, &timeinfo);
    char fecha[40];
    strftime(fecha, sizeof(fecha), "%d %b %Y, %H:%M:%S", &timeinfo);
    doc["timestamp"] = fecha;

    char buf[256];
    size_t n = serializeJson(doc, buf);
    mqttClient.publish(MQTT_TOPIC_BASE.c_str(), (uint8_t *)buf, n, true);
}

void addEvent(const char *evento)
{
    // Ignorar si es el mismo evento dentro del cooldown (evita spam en ráfaga)
    // pero sí lo registra si ha pasado más de EVENT_DEDUP_MS (permite histórico completo)
    unsigned long nowMs = millis();
    if (strncmp(evento, lastEventSent, sizeof(lastEventSent)) == 0 &&
        nowMs - lastEventMs < EVENT_DEDUP_MS) return;
    strncpy(lastEventSent, evento, sizeof(lastEventSent) - 1);
    lastEventSent[sizeof(lastEventSent) - 1] = '\0';
    lastEventMs = nowMs;

    time_t nowT = time(nullptr);
    struct tm timeinfo;
    getLocalTime(nowT, &timeinfo);
    char fecha[40];
    strftime(fecha, sizeof(fecha), "%d %b %Y, %H:%M:%S", &timeinfo);

    JsonDocument docEv;
    docEv["evento"]   = evento;
    docEv["fecha"]    = fecha;
    docEv["version"]  = APP_VERSION;
    char buf[192];
    size_t n = serializeJson(docEv, buf);

    if (mqttClient.connected())
    {
        mqttClient.publish(MQTT_TOPIC_EVENTS.c_str(), buf, n);
    }
    else if (pendingEventCount < MAX_PENDING_EVENTS)
    {
        memcpy(pendingEvents[pendingEventCount], buf, n + 1);
        pendingEventCount++;
        printLog(String("Evento encolado (MQTT offline): ") + evento);
    }
}



// -------------------- NTP / ZONA HORARIA ------------------------------------

// Calcula el offset UTC para España (CET = UTC+1, CEST = UTC+2).
// Transición: último domingo de marzo a las 01:00 UTC → verano
//             último domingo de octubre a las 01:00 UTC → invierno
static int getSpainOffset(time_t rawtime)
{
    struct tm ti_buf;
    gmtime_r(&rawtime, &ti_buf);
    int year = ti_buf.tm_year + 1900;

    struct tm lastMarch = {};
    lastMarch.tm_year = year - 1900;
    lastMarch.tm_mon  = 2;   // marzo
    lastMarch.tm_mday = 31;
    lastMarch.tm_hour = 1;   // 01:00 UTC
    time_t tMarch = mktime(&lastMarch);
    struct tm tMarch_buf;
    lastMarch.tm_mday -= gmtime_r(&tMarch, &tMarch_buf)->tm_wday; // retrocede al domingo
    tMarch = mktime(&lastMarch);

    struct tm lastOct = {};
    lastOct.tm_year = year - 1900;
    lastOct.tm_mon  = 9;     // octubre
    lastOct.tm_mday = 31;
    lastOct.tm_hour = 1;     // 01:00 UTC
    time_t tOct = mktime(&lastOct);
    struct tm tOct_buf;
    lastOct.tm_mday -= gmtime_r(&tOct, &tOct_buf)->tm_wday;
    tOct = mktime(&lastOct);

    return (rawtime >= tMarch && rawtime < tOct) ? 2 * 3600 : 3600;
}

// Devuelve la hora local de España como struct tm a partir de un timestamp UTC.
static void getLocalTime(time_t utc, struct tm *ti)
{
    time_t local = utc + getSpainOffset(utc);
    gmtime_r(&local, ti);
}

// Inicia la sincronización NTP en background (no bloqueante).
// Sincroniza en UTC puro; el offset horario lo aplica getSpainOffset().
void beginNtpSync()
{
    configTime(0, 0, "pool.ntp.org", "time.google.com", "ntp.ubuntu.com");
    printLog("Sincronización NTP iniciada...");
}

bool checkNtpSynced()
{
    return time(nullptr) > 1577836800UL; // > 2020-01-01 00:00:00 UTC
}

// -------------------- SENSOR NIVEL AGUA -------------------------------------

// Sensor capacitivo externo (no sumergible):
//   ADC ~0    = hay agua  → sensor conduce → LOW
//   ADC ~4095 = sin agua  → sensor no conduce → HIGH
//   ADC 500–3500 = zona indeterminada (pin flotante) → mantener estado anterior
//
// Se requieren WATER_LEVEL_CONFIRM_COUNT lecturas consecutivas en la misma
// dirección para confirmar un cambio de estado (fail-safe anti-ruido).
// El contador se resetea si la lectura entra en la zona contraria y se
// limita a ±WATER_LEVEL_CONFIRM_COUNT para evitar crecimiento ilimitado.
void checkWaterLevel()
{
    unsigned long now = millis();
    if (now - lastWaterCheckMs < WATER_CHECK_INTERVAL_MS) return;
    lastWaterCheckMs = now;

    int nivel = analogRead(WATER_LEVEL_SENSOR_PIN);
    bool prev = state.nivelAgua;

    if (nivel < WATER_PRESENT_THRESHOLD)
    {
        if (waterConfirmCount < 0) waterConfirmCount = 0;
        waterConfirmCount++;
        if (waterConfirmCount > WATER_LEVEL_CONFIRM_COUNT)
            waterConfirmCount = WATER_LEVEL_CONFIRM_COUNT;
        if (waterConfirmCount >= WATER_LEVEL_CONFIRM_COUNT)
            state.nivelAgua = true;
    }
    else if (nivel > WATER_ABSENT_THRESHOLD)
    {
        if (waterConfirmCount > 0) waterConfirmCount = 0;
        waterConfirmCount--;
        if (waterConfirmCount < -WATER_LEVEL_CONFIRM_COUNT)
            waterConfirmCount = -WATER_LEVEL_CONFIRM_COUNT;
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
    // Guardia de seguridad: no encender si no hay agua confirmada
    if (!state.nivelAgua || state.bloqueoSinAgua)
    {
        printLog("startPump bloqueado: sin agua");
        return;
    }
    // Apagar sensor si estaba activo (riego interrumpe ciclo de muestreo)
    if (sensorPowered)
    {
        digitalWrite(SENSOR_POWER_PIN, LOW);
        sensorPowered      = false;
        sensorWarmingUp    = false;
        samplingInProgress = false;
    }
    pumpNoWaterCount = 0;
    pumpWaterCheckMs = millis();
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
    pinMode(SENSOR_POWER_PIN,       OUTPUT);
    digitalWrite(PUMP_PIN,         LOW);
    digitalWrite(SENSOR_POWER_PIN, LOW);
    sensors.begin();
    sensors.setWaitForConversion(false); // DS18B20 no bloqueante

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
        beginNtpSync();
        unsigned long ntpStart = millis();
        while (!checkNtpSynced() && millis() - ntpStart < NTP_TIMEOUT_MS)
            delay(200);
        ntpSynced = checkNtpSynced();
        if (ntpSynced)
        {
            time_t t = time(nullptr);
            struct tm ti;
            getLocalTime(t, &ti);
            char buf[64];
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &ti);
            printLog(String("Hora NTP sincronizada: ") + buf);
        }
        else
            printLog("No se pudo sincronizar hora NTP.");
    }
    else
    {
        printLog("No se pudo conectar a WiFi. Se omite NTP.");
    }

    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    mqttEnsureConnected();

    if (!ntpSynced)
        addEvent("ntp_error");
    else
        addEvent("init_ok");
}

// -------------------- LOOP NO BLOQUEANTE ------------------------------------

void loop()
{
    // — Reset flag de reconexión si WiFi volvió —
    if (wifiReconnecting && WiFi.status() == WL_CONNECTED)
    {
        wifiReconnecting = false;
        printLog("WiFi reconectado.");
        mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
        mqttClient.setCallback(mqttCallback);
        beginNtpSync(); // no bloqueante: sincroniza en background
    }

    // — Sensor nivel agua (siempre, independiente de WiFi) —
    checkWaterLevel();

    // — Control bomba (prioridad máxima, independiente de WiFi) —
    if (bombaEncendida)
    {
        unsigned long pNow = millis();
        if (pNow - pumpWaterCheckMs >= PUMP_WATER_CHECK_MS)
        {
            pumpWaterCheckMs = pNow;
            int nivel = analogRead(WATER_LEVEL_SENSOR_PIN);
            if (nivel > WATER_ABSENT_THRESHOLD)
            {
                pumpNoWaterCount++;
                if (pumpNoWaterCount >= PUMP_NO_WATER_CONFIRM)
                {
                    pumpNoWaterCount = 0;
                    stopPump("SIN AGUA");
                    state.bloqueoSinAgua = true;
                    addEvent("pump_off_no_water");
                    return;
                }
            }
            else
            {
                pumpNoWaterCount = 0;
            }
        }
        if (millis() - bombaStartMillis >= state.duracionRiego)
        {
            pumpNoWaterCount = 0;
            stopPump("Duracion completada");
            addEvent("pump_off_done");
        }
        return;
    }

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
        else if (now - wifiReconnectMs > WIFI_RECONNECT_TIMEOUT_MS)
        {
            wifiReconnecting = false; // reinicia el ciclo de reconexión
            printLog("Timeout reconexión WiFi. Reintentando...");
        }
        return;
    }

    // — NTP: detectar sincronización completada en background —
    if (!ntpSynced && checkNtpSynced())
    {
        ntpSynced = true;
        time_t t = time(nullptr);
        struct tm ti;
        getLocalTime(t, &ti);
        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &ti);
        printLog(String("NTP sincronizado: ") + buf);
    }

    // — MQTT no bloqueante —
    mqttEnsureConnected();
    mqttClient.loop();

    // — Muestreo humedad no bloqueante (sensor encendido solo durante la lectura) —
    unsigned long now = millis();

    // Fase 1: iniciar ciclo si toca (primera vez o cada HUMEDAD_CYCLE_INTERVAL_MS)
    if (!samplingInProgress && !sensorWarmingUp)
    {
        if (firstSample || now - lastCycleMillis >= HUMEDAD_CYCLE_INTERVAL_MS)
        {
            firstSample = false;
            digitalWrite(SENSOR_POWER_PIN, HIGH);
            sensorPowered     = true;
            sensorWarmingUp   = true;
            sensorWarmupStart = now;
            sampleIndex       = 0;
            belowCount        = 0;
            humSum            = 0.0f;
        }
    }

    // Fase 2: esperar estabilización del sensor
    if (sensorWarmingUp && now - sensorWarmupStart >= SENSOR_WARMUP_MS)
    {
        sensorWarmingUp    = false;
        samplingInProgress = true;
        lastSampleMillis   = now;
    }

    // Fase 3: tomar muestras
    if (samplingInProgress && now - lastSampleMillis >= HUMEDAD_SAMPLE_INTERVAL_MS)
    {
        lastSampleMillis = now;
        int sensorValue = analogRead(HUMIDITY_SENSOR_PIN);
        float humedad = (float)(TIERRA_SECA - sensorValue) / (TIERRA_SECA - TIERRA_HUMEDA) * 100.0f;
        humedad = constrain(humedad, 0.0f, 100.0f);
        humSum += humedad;
        if (humedad < state.humedadUmbral)
            belowCount++;
        sampleIndex++;

        if (sampleIndex >= HUMEDAD_SAMPLE_COUNT)
        {
            lastHumedad        = humSum / HUMEDAD_SAMPLE_COUNT; // media del ciclo
            samplingInProgress = false;
            // Apagar sensor: ya no hace falta hasta el siguiente ciclo
            digitalWrite(SENSOR_POWER_PIN, LOW);
            sensorPowered   = false;
            lastCycleMillis = now;

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

            // Solicitar temperatura al final del ciclo; se publica 750 ms después
            sensors.requestTemperatures();
            tempRequested     = true;
            tempRequestMillis = now;
        }
    }

    // — Publicación MQTT: leer temperatura y publicar tras conversión DS18B20 —
    if (tempRequested && now - tempRequestMillis >= DS18B20_CONVERSION_MS)
    {
        tempRequested = false;
        float t = sensors.getTempCByIndex(0);
        if (t == DEVICE_DISCONNECTED_C)
            addEvent("sensor_temp_error");
        else
            lastTemp = t;
        publishState(lastTemp);
    }
}
