#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "time.h"

// 1. CONFIGURA TUS DATOS AQUÍ
#define ssid "hitron 5G"
#define password "QUINTANILLA1995"
const String firebaseUrl = "https://test-9d590-default-rtdb.firebaseio.com"; // SIN la barra / al final

// Configuración de hora real (GMT-6 para El Salvador)
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -6 * 3600;
const int daylightOffset_sec = 0;

unsigned long ultimoChequeoManual = 0;
unsigned long ultimoChequeoHorarios = 0;
String ultimoMinutoDispensado = ""; // Evita que dispense varias veces en el mismo minuto

void setup() {
  Serial.begin(115200);
  
  Serial.print("Conectando a Wi-Fi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n¡Wi-Fi conectado!");

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  enviarDatoPUT("/dispositivo/estado_wifi.json", "true");
  
  Serial.println("\n--- SIMULADOR CON HORARIOS LISTO ---");
  Serial.println("Comandos: nfc | tolva | normal | peso 45 | hora");
}

void loop() {
  // 1. Cada 3 segundos revisa si presionaste "Dispensar ahora" en la app
  if (millis() - ultimoChequeoManual > 3000) {
    ultimoChequeoManual = millis();
    revisarOrdenDispensar();
  }

  // 2. Cada 10 segundos comprueba si toca alguna rutina programada
  if (millis() - ultimoChequeoHorarios > 10000) {
    ultimoChequeoHorarios = millis();
    comprobarHorarios();
  }

  // 3. Comandos manuales desde el Monitor Serie
  if (Serial.available()) {
    String comando = Serial.readStringUntil('\n');
    comando.trim();

    if (comando == "nfc") {
      String fecha = obtenerFechaCompleta();
      String json = "{\"tipo\":\"acceso_nfc\",\"detalle\":\"Tapa abierta - Gato detectado\",\"timestamp\":\"" + fecha + "\"}";
      enviarDatoPOST("/logs.json", json);
      Serial.println(">> Evento NFC enviado.");
    } 
    else if (comando == "tolva") {
      enviarDatoPUT("/dispositivo/tolva_baja.json", "true");
      Serial.println(">> Alerta de tolva baja activada.");
    } 
    else if (comando == "normal") {
      enviarDatoPUT("/dispositivo/tolva_baja.json", "false");
      Serial.println(">> Nivel de tolva restaurado.");
    } 
    else if (comando.startsWith("peso ")) {
      String gramos = comando.substring(5);
      enviarDatoPUT("/dispositivo/peso_actual_plato.json", gramos);
      Serial.println(">> Peso actualizado a " + gramos + "g.");
    }
    else if (comando == "hora") {
      Serial.println(">> Hora actual del ESP32: " + obtenerHoraMinuto());
    }
  }
}

// --- COMPROBACIÓN AUTOMÁTICA DE HORARIOS ---

void comprobarHorarios() {
  String horaActual = obtenerHoraMinuto(); // Formato "HH:MM"
  if (horaActual == "" || horaActual == ultimoMinutoDispensado) return;

  String jsonRutinas = leerDatoGET("/rutinas.json");
  if (jsonRutinas == "" || jsonRutinas == "null") return;

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, jsonRutinas);
  if (error) {
    Serial.println("Error al leer JSON de rutinas");
    return;
  }

  // Recorremos todas las rutinas guardadas en Firebase
  for (JsonPair kv : doc.as<JsonObject>()) {
    JsonObject rutina = kv.value().as<JsonObject>();
    
    bool activa = rutina["activa"] | false;
    String horaProgramada = rutina["hora"] | "";
    int gramos = rutina["gramos"] | 0;

    if (activa && horaProgramada == horaActual) {
      ultimoMinutoDispensado = horaActual; // Bloquea este minuto para no repetir
      
      Serial.println("\n[RUTINA] ¡Hora alcanzada (" + horaActual + ")! Dispensando " + String(gramos) + "g...");
      
      // 1. Actualizamos el peso simulado en el plato
      enviarDatoPUT("/dispositivo/peso_actual_plato.json", String(gramos));

      // 2. Guardamos el evento en el Registro Histórico (esto también dispara la notificación en tu celular)
      String fecha = obtenerFechaCompleta();
      String detalle = String(gramos) + "g servidos (Rutina " + horaActual + ")";
      String logJson = "{\"tipo\":\"dispensacion\",\"detalle\":\"" + detalle + "\",\"timestamp\":\"" + fecha + "\"}";
      enviarDatoPOST("/logs.json", logJson);
      
      Serial.println("[RUTINA] Dispensación completada y registrada en logs.\n");
      break; 
    }
  }
}

// --- FUNCIONES AUXILIARES ---

void revisarOrdenDispensar() {
  String estado = leerDatoGET("/dispositivo/dispensar_ahora.json");
  if (estado == "true") {
    Serial.println("\n[APP] ¡Orden manual recibida! Simulando dispensación...");
    enviarDatoPUT("/dispositivo/dispensar_ahora.json", "false");
    enviarDatoPUT("/dispositivo/peso_actual_plato.json", "50");
  }
}

String leerDatoGET(String ruta) {
  if (WiFi.status() != WL_CONNECTED) return "";
  HTTPClient http;
  http.begin(firebaseUrl + ruta);
  int httpCode = http.GET();
  String payload = (httpCode > 0) ? http.getString() : "";
  http.end();
  return payload;
}

void enviarDatoPUT(String ruta, String valorJson) {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(firebaseUrl + ruta);
  http.addHeader("Content-Type", "application/json");
  http.PUT(valorJson);
  http.end();
}

void enviarDatoPOST(String ruta, String valorJson) {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(firebaseUrl + ruta);
  http.addHeader("Content-Type", "application/json");
  http.POST(valorJson);
  http.end();
}

String obtenerHoraMinuto() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "";
  char buffer[6];
  strftime(buffer, sizeof(buffer), "%H:%M", &timeinfo);
  return String(buffer);
}

String obtenerFechaCompleta() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "2026-11-23 00:00:00";
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buffer);
}