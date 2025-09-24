#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include "config.h"
#include <time.h>
#include <PubSubClient.h>
#include <Preferences.h> // <-- Añadido para memoria permanente
#include <ArduinoJson.h>
#include "esp_sleep.h"

#define HUMIDITY_SENSOR_PIN 34
#define TIERRA_SECA 4095
#define TIERRA_HUMEDA 1000
#define ONE_WIRE_BUS 4
#define BOMB_PIN 25

// --- Identificador único del dispositivo ---
#define DEVICE_ID "riego_esp32_01" // Cambia este valor para cada dispositivo "riego_esp32_XX"

// --- Tópicos MQTT generados automáticamente ---
#define MQTT_TOPIC_BASE "sensors/" DEVICE_ID
#define MQTT_TOPIC_CONFIG MQTT_TOPIC_BASE "/config"

// --- Nombre de cliente MQTT único ---
#define MQTT_CLIENT_NAME "ESP32Client_" DEVICE_ID

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

WiFiClient espClient;
PubSubClient client(espClient);
Preferences preferences; // <-- Instancia de almacenamiento

float humedadUmbral = 75.0;           // valor por defecto si no hay nada en memoria
unsigned long duracionRiego = 120000; // 2 minutos por defecto (en ms)

// --- Prototipo de printLog ---
void printLog(const String &mensaje);

// --- NUEVAS FUNCIONES PARA DURACION ---
void saveDuracion(unsigned long duracion)
{
  preferences.begin("riego", false);
  preferences.putULong("duracion", duracion);
  preferences.end();
  printLog("Nueva duración guardada en memoria: " + String(duracion) + " ms");
}

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

void loadDuracion()
{
  preferences.begin("riego", true);
  if (preferences.isKey("duracion"))
  {
    duracionRiego = preferences.getULong("duracion", 120000);
  }
  preferences.end();
  printLog("Duración cargada: " + String(duracionRiego) + " ms");
}

void saveThreshold(float umbral)
{
  preferences.begin("riego", false); // namespace "riego"
  preferences.putFloat("umbral", umbral);
  preferences.end();
  printLog("Nuevo umbral guardado en memoria: " + String(umbral));
}

void loadThreshold()
{
  preferences.begin("riego", true);
  if (preferences.isKey("umbral"))
  {
    humedadUmbral = preferences.getFloat("umbral", 75.0);
  }
  preferences.end();
  printLog("Umbral cargado: " + String(humedadUmbral));
}

void callback(char *topic, byte *payload, unsigned int length)
{
  String msg;
  for (unsigned int i = 0; i < length; i++)
  {
    msg += (char)payload[i];
  }
  Serial.println("Mensaje recibido: " + msg);

  if (String(topic) == MQTT_TOPIC_CONFIG)
  {
    DynamicJsonDocument doc(128);
    DeserializationError error = deserializeJson(doc, msg);
    if (!error)
    {
      if (doc.containsKey("umbral"))
      {
        humedadUmbral = doc["umbral"].as<float>();
        saveThreshold(humedadUmbral);
      }
      if (doc.containsKey("duracion"))
      {
        duracionRiego = doc["duracion"].as<unsigned long>();
        saveDuracion(duracionRiego);
      }
    }
  }
}

void reconnect()
{
  while (!client.connected())
  {
    Serial.print("Conectando a MQTT...");
    if (client.connect(MQTT_CLIENT_NAME, MQTT_USER, MQTT_PASSWORD))
    {
      Serial.println("conectado!");
      client.subscribe(MQTT_TOPIC_CONFIG); // <-- Escuchar config
    }
    else
    {
      Serial.print("falló, rc=");
      Serial.print(client.state());
      Serial.println(" intentando de nuevo en 5 segundos");
      delay(5000);
    }
  }
}

// Devuelve true si la hora está entre las 20:00 y las 10:00
bool esHoraDeDormir()
{
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  int hora = timeinfo.tm_hour;
  return (hora >= 21 || hora < 10);
}

// Devuelve microsegundos hasta las 10:00 del día siguiente
uint64_t microsHastaLas10()
{
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  struct tm nextWake = timeinfo;
  if (timeinfo.tm_hour >= 10 && timeinfo.tm_hour < 20)
  {
    // Ya pasó las 10:00, no dormir
    return 0;
  }
  // Si es después de las 20:00, despierta mañana a las 10:00
  if (timeinfo.tm_hour >= 20)
  {
    nextWake.tm_mday += 1;
  }
  nextWake.tm_hour = 10;
  nextWake.tm_min = 0;
  nextWake.tm_sec = 0;

  time_t wakeTime = mktime(&nextWake);
  uint64_t segundos = difftime(wakeTime, now);
  return segundos * 1000000ULL;
}

void setup()
{
  Serial.begin(115200);
  pinMode(HUMIDITY_SENSOR_PIN, INPUT);
  sensors.begin();
  pinMode(BOMB_PIN, OUTPUT);
  digitalWrite(BOMB_PIN, LOW);

  loadThreshold(); // <-- cargar umbral de memoria al inicio
  loadDuracion();  // <-- cargar duración de memoria al inicio

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Conectando a WiFi");
  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 10)
  {
    delay(500);
    Serial.print(".");
    intentos++;
  }
  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("\nWiFi conectado!");
  }
  else
  {
    Serial.println("\nNo se pudo conectar a WiFi tras 10 intentos.");
  }

  configTime(7200, 0, "pool.ntp.org");
  Serial.print("Esperando hora NTP");
  time_t now = time(nullptr);
  unsigned long ntpStart = millis();
  const unsigned long ntpTimeout = 30000;

  while (now < 8 * 3600 * 2 && (millis() - ntpStart) < ntpTimeout)
  {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  if (now < 8 * 3600 * 2)
  {
    Serial.println("\nNo se pudo sincronizar hora NTP.");
  }
  else
  {
    Serial.println("\nHora NTP sincronizada.");
  }

  client.setServer(MQTT_SERVER, MQTT_PORT);
  client.setCallback(callback); // <-- Escucha mensajes entrantes
}

void loop()
{
  if (esHoraDeDormir())
  {
    printLog("Horario nocturno (20h-10h). Entrando en sueño profundo.");
    uint64_t microsSleep = microsHastaLas10();
    if (microsSleep > 0)
    {
      client.publish(MQTT_TOPIC_BASE, "{\"evento\":\"sleep_nocturno\"}");
      delay(100);
      esp_sleep_enable_timer_wakeup(microsSleep);
      delay(100);
      esp_deep_sleep_start();
    }
  }

  if (!client.connected())
  {
    reconnect();
  }
  client.loop();

  static bool bombaEncendida = false;
  static unsigned long bombaStartMillis = 0;
  static float lastHumedad = 0;

  float humedad = lastHumedad;

  if (bombaEncendida)
  {
    if (millis() - bombaStartMillis >= duracionRiego) // <-- usar variable
    {
      digitalWrite(BOMB_PIN, LOW);
      bombaEncendida = false;
      printLog("Bomba APAGADA (" + String(duracionRiego / 1000) + " segundos completados)");
    }
  }
  else
  {
    int consecutiveBelow = 0;
    for (int i = 0; i < 3; i++)
    {
      int sensorValue = analogRead(HUMIDITY_SENSOR_PIN);
      humedad = (float)(TIERRA_SECA - sensorValue) / (TIERRA_SECA - TIERRA_HUMEDA) * 100.0;
      humedad = constrain(humedad, 0, 100);

      if (humedad < humedadUmbral)
      {
        consecutiveBelow++;
      }
      delay(2000);
    }

    lastHumedad = humedad;

    if (consecutiveBelow == 3 && !bombaEncendida)
    {
      digitalWrite(BOMB_PIN, HIGH);
      bombaEncendida = true;
      bombaStartMillis = millis();
      printLog("Bomba ENCENDIDA (humedad < " + String(humedadUmbral) + "%)");
    }
  }

  sensors.requestTemperatures();
  float tempC = sensors.getTempCByIndex(0);

  char logMsg[96];
  snprintf(logMsg, sizeof(logMsg), "Humedad: %.1f %% | Temp: %.1f °C | Umbral: %.1f %% | Riego: %lus",
           lastHumedad, tempC, humedadUmbral, duracionRiego / 1000);
  printLog(logMsg);

  char payload[160];
  snprintf(payload, sizeof(payload), "{\"humedad\":%.1f,\"temperatura\":%.1f,\"umbral\":%.1f,\"duracion\":%lu}",
           lastHumedad, tempC, humedadUmbral, duracionRiego);
  client.publish(MQTT_TOPIC_BASE, payload);

  delay(5000);
}
