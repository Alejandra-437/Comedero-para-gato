#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include "HX711.h"
#include "time.h"

// ==========================================
// 1. CONFIGURACIÓN WI-FI Y FIREBASE
// ==========================================
const char* ssid = "TU_NOMBRE_WIFI";
const char* password = "TU_CONTRASEÑA_WIFI";
const String firebaseUrl = "https://test-9d590-default-rtdb.firebaseio.com"; // SIN / al final

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -6 * 3600; // GMT-6 El Salvador
const int daylightOffset_sec = 0;

// ==========================================
// 2. PINES Y CONFIGURACIÓN DE HARDWARE
// ==========================================
#define DT_PIN        16
#define SCK_PIN       17
#define SERVO_PIN     18

#define SERVO_FEED_US  400    // Pulso de giro de tu servo
#define TIMEOUT_MS     15000  // Seguridad: detiene el servo tras 15 s

float factorCalibracion = 467.1;
const float ZONA_MUERTA = 1.5;

HX711 balanza;
Servo servoDispensador;

// Variables de control de dispensación
bool dispensando = false;
float pesoInicial = 0.0;
float pesoObjetivo = 0.0;
String origenDispensa = "Manual";
unsigned long inicioDispensa = 0;

// Temporizadores
unsigned long ultimoChequeoManual = 0;
unsigned long ultimoChequeoHorarios = 0;
unsigned long ultimoEnvioPeso = 0;
String ultimoMinutoDispensado = "";

// ==========================================
// 3. LECTURA LIMPIA DE LA BALANZA
// ==========================================
float leerPeso(int muestras) {
  float peso = balanza.get_units(muestras);
  if (fabs(peso) < ZONA_MUERTA) return 0.0f;
  return peso;
}

// ==========================================
// 4. CONTROL DEL SERVO + REGISTRO EN FIREBASE
// ==========================================
void iniciarDispensa(float gramosPorServir, String origen) {
  if (gramosPorServir <= 0 || dispensando) return;

  pesoInicial = balanza.get_units(5);
  pesoObjetivo = gramosPorServir;
  origenDispensa = origen;
  dispensando = true;
  inicioDispensa = millis();

  servoDispensador.attach(SERVO_PIN, 300, 2400);
  servoDispensador.writeMicroseconds(SERVO_FEED_US);
  Serial.printf(">> DISPENSANDO +%.1f g [%s] (base: %.1f g)...\n", pesoObjetivo, origen.c_str(), pesoInicial);
}

void detenerServo(const char* motivo, bool esTimeout = false) {
  servoDispensador.detach();
  dispensando = false;
  Serial.printf(">> DETENIDO: %s\n", motivo);

  delay(400);
  float pesoFinalCrudo = balanza.get_units(10);
  float pesoAgregado = pesoFinalCrudo - pesoInicial;

  if (fabs(pesoAgregado) < ZONA_MUERTA) pesoAgregado = 0.0f;
  float pesoTotalLimpio = (fabs(pesoFinalCrudo) < ZONA_MUERTA) ? 0.0f : pesoFinalCrudo;

  Serial.printf("   Agregado real: %.1f g | Total en plato: %.1f g\n\n", pesoAgregado, pesoTotalLimpio);

  // Actualizar peso real en Firebase
  enviarDatoPUT("/dispositivo/peso_actual_plato.json", String((int)round(pesoTotalLimpio)));

  if (esTimeout) {
    enviarDatoPUT("/dispositivo/tolva_baja.json", "true");
  } else {
    enviarDatoPUT("/dispositivo/tolva_baja.json", "false");
    
    String fecha = obtenerFechaCompleta();
    String detalle = String((int)round(pesoAgregado)) + "g servidos (" + origenDispensa + ")";
    String logJson = "{\"tipo\":\"dispensacion\",\"detalle\":\"" + detalle + "\",\"timestamp\":\"" + fecha + "\"}";
    enviarDatoPOST("/logs.json", logJson);
  }
}

// ==========================================
// 5. COMUNICACIÓN CON FIREBASE
// ==========================================
void revisarOrdenDispensar() {
  String estado = leerDatoGET("/dispositivo/dispensar_ahora.json");
  if (estado == "true") {
    enviarDatoPUT("/dispositivo/dispensar_ahora.json", "false");
    iniciarDispensa(50.0, "App Manual");
  }
}

void comprobarHorarios() {
  String horaActual = obtenerHoraMinuto();
  if (horaActual == "" || horaActual == ultimoMinutoDispensado) return;

  String jsonRutinas = leerDatoGET("/rutinas.json");
  if (jsonRutinas == "" || jsonRutinas == "null") return;

  JsonDocument doc;
  if (deserializeJson(doc, jsonRutinas)) return;

  for (JsonPair kv : doc.as<JsonObject>()) {
    JsonObject rutina = kv.value().as<JsonObject>();
    bool activa = rutina["activa"] | false;
    String horaProgramada = rutina["hora"] | "";
    float gramos = rutina["gramos"] | 0.0;

    if (activa && horaProgramada == horaActual && gramos > 0) {
      ultimoMinutoDispensado = horaActual;
      iniciarDispensa(gramos, "Rutina " + horaActual);
      break;
    }
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
  if (!getLocalTime(&timeinfo)) return "2026-11-24 00:00:00";
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buffer);
}

// ==========================================
// 6. SETUP Y LOOP
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(300);

  // Configurar servo detenido al arrancar
  ESP32PWM::allocateTimer(0);
  servoDispensador.setPeriodHertz(50);
  servoDispensador.attach(SERVO_PIN, 300, 2400);
  servoDispensador.detach();

  // Inicializar balanza
  balanza.begin(DT_PIN, SCK_PIN);
  Serial.println("No pongas peso en la balanza (solo el plato vacio)...");
  delay(2000);
  balanza.set_scale(factorCalibracion);
  balanza.tare(20);

  // Conectar Wi-Fi
  Serial.print("Conectando a Wi-Fi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n¡Wi-Fi conectado!");

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  enviarDatoPUT("/dispositivo/estado_wifi.json", "true");
  enviarDatoPUT("/dispositivo/peso_actual_plato.json", "0");

  Serial.println("\n=== SISTEMA COMPLETO LISTO (HX711 + SERVO + FIREBASE) ===\n");
}

void loop() {
  if (dispensando) {
    // MODO DISPENSACIÓN: máxima prioridad a la balanza (sin peticiones HTTP)
    if (balanza.is_ready()) {
      float pesoAgregado = balanza.get_units(2) - pesoInicial;
      float pesoMostrar = (fabs(pesoAgregado) < ZONA_MUERTA) ? 0.0f : pesoAgregado;

      Serial.printf("Sirviendo... +%.1f / %.1f g\n", pesoMostrar, pesoObjetivo);

      if (pesoAgregado >= pesoObjetivo) {
        detenerServo("Peso objetivo alcanzado", false);
      }
    }

    if (dispensando && (millis() - inicioDispensa >= TIMEOUT_MS)) {
      detenerServo("Timeout de seguridad (posible tolva vacia o atasco)", true);
    }
  } else {
    // MODO REPOSO: escuchar Firebase y actualizar peso
    if (millis() - ultimoChequeoManual > 3000) {
      ultimoChequeoManual = millis();
      revisarOrdenDispensar();
    }

    if (millis() - ultimoChequeoHorarios > 10000) {
      ultimoChequeoHorarios = millis();
      comprobarHorarios();
    }

    if (millis() - ultimoEnvioPeso > 5000) {
      ultimoEnvioPeso = millis();
      if (balanza.is_ready()) {
        float peso = leerPeso(5);
        Serial.printf("Peso actual: %.1f g\n", peso);
        enviarDatoPUT("/dispositivo/peso_actual_plato.json", String((int)round(peso)));
      }
    }
  }
}