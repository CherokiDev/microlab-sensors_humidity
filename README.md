# Repaso punto por punto de la aplicación de riego automático con ESP32

## Cambios principales en esta versión

- Añadida sincronización de hora NTP.
- La sincronización espera hasta un timeout configurable y reporta por MQTT si falla.
- Mantiene el muestreo no bloqueante y la lógica de deep-sleep para ahorro energético.

**Notas:**

- La sincronización NTP se realiza sólo si hay conexión WiFi.
- Si no se sincroniza en el tiempo límite, se publica un evento retenido `ntp_error` en `sensors/<DEVICE_ID>/events` para que el frontend muestre aviso.

---

## 1. **Inicialización (`setup`)**

- Inicializa el puerto serie para logs.
- Configura los pines de sensores y bomba.
- Inicializa el sensor de temperatura Dallas.
- Carga el umbral de humedad y la duración de riego desde la memoria permanente.
- Conecta a la red WiFi.
- Sincroniza la hora con NTP (España, CET/CEST), esperando hasta un timeout configurable.
- Si la sincronización NTP falla, publica evento `ntp_error` por MQTT.
- Configura el cliente MQTT y se suscribe al tópico de configuración.

---

## 2. **Lectura de sensores (`loop`)**

- Lee el sensor de nivel de agua:
  - Si no hay agua, activa el bloqueo de riego.
  - Si vuelve a haber agua, desactiva el bloqueo y lo registra en el log.

---

## 3. **Control de horario nocturno**

- Si la hora está entre las 20:00 y las 10:00:
  - Publica el evento `sleep_nocturno` por MQTT.
  - Entra en deep sleep hasta las 10:00 del día siguiente.

---

## 4. **Conexión MQTT**

- Si no está conectado al broker MQTT, intenta reconectar.
- Procesa mensajes entrantes (por ejemplo, cambios de configuración enviados desde el dashboard).

---

## 5. **Lógica de riego**

- Si la bomba está encendida:
  - Verifica si se queda sin agua durante el riego y apaga la bomba si es necesario.
  - Apaga la bomba cuando se cumple el tiempo de riego.
- Si la bomba está apagada:
  - Realiza 3 lecturas de humedad con muestreo no bloqueante y un pequeño delay entre ellas.
  - Si las 3 lecturas están por debajo del umbral y no hay bloqueo, enciende la bomba y comienza el riego.
  - Si hay bloqueo por falta de agua, lo registra en el log.

---

## 6. **Lectura de temperatura**

- Lee la temperatura del sensor Dallas y la incluye en el log y en el mensaje MQTT.

---

## 7. **Publicación de datos**

- Publica por MQTT los datos de humedad, temperatura, umbral, duración de riego y nivel de agua.

---

## 8. **Recepción de configuración**

- Espera unos segundos tras publicar para recibir posibles mensajes de configuración (umbral y duración de riego) desde el dashboard.
- Si recibe un mensaje retained, actualiza la configuración y la guarda en memoria.

---

## 9. **Deep sleep**

- Apaga el WiFi y entra en deep sleep durante el tiempo configurado (`DEEP_SLEEP_SECONDS`).
- Al despertar, repite el ciclo desde el paso 1.
