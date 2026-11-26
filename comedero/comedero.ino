#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include "HX711.h"
#include "time.h"

// ==========================================
// 1. CONFIGURACIÓN WI-FI, FIREBASE Y RELOJ
// ==========================================
const char* ssid = "Tallertest";
const char* password = "12345678";
const String firebaseUrl = "https://test-9d590-default-rtdb.firebaseio.com"; // SIN / al final

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -6 * 3600; // GMT-6 El Salvador
const int daylightOffset_sec = 0;

// ==========================================
// 2. DEFINICIÓN DE PINES Y PARÁMETROS HARDWARE
// ==========================================
// --- Balanza HX711 ---
#define DT_PIN                16
#define SCK_PIN               17

// --- Servo Dispensador (360° rotación continua) ---
#define SERVO_DISPENSADOR_PIN 25
#define SERVO_FEED_US         400
#define SERVO_REV_US          2500

// --- Lector RFID / NFC (SPI: SCK=18, MISO=19, MOSI=23) ---
#define RFID_SS_PIN           5
#define RFID_RST_PIN          22

// --- Servos de Compuertas (SG92R derecha e izquierda) ---
#define SERVO_TAPA_R_PIN      13
#define SERVO_TAPA_L_PIN      14

// --- Ángulos y temporización de compuertas ---
const int ANGULO_CERRADO_R                     = 90;
const int ANGULO_ABIERTO_R                     = 10;
const int ANGULO_CERRADO_L                     = 10;
const int ANGULO_ABIERTO_L                     = 100;
const unsigned long MARGEN_APERTURA_COMPUERTA  = 1000;
const unsigned long TIEMPO_CIERRE_MS           = 1500;
const unsigned long INTERVALO_LECTURA_RFID_MS  = 100;

// --- UID del tag autorizado ---
const byte UID_AUTORIZADO_LEN                  = 4;
const byte UID_AUTORIZADO[UID_AUTORIZADO_LEN]  = { 0x21, 0x12, 0xD5, 0x1D };

// --- Protecciones de seguridad y detección de atasco ---
const unsigned long TIMEOUT_GLOBAL_MS          = 60000;
const unsigned long TIMEOUT_ARRANQUE_MS        = 2200;
const unsigned long TIMEOUT_SIN_FLUJO_MS       = 2200;
const float MIN_AVANCE_RAPIDO_G                = 1.0;
const float MIN_AVANCE_FINO_G                  = 0.3;
const float AVANCE_RESET_DESATASCO_G           = 4.0;
const int   MAX_PULSOS_SIN_AVANCE              = 3;
const int   MAX_INTENTOS_DESATASCO             = 3;

// --- Tiempos de pre-reversa y desatasco progresivo ---
const unsigned long DURACION_PRE_REVERSA_MS    = 550;
const unsigned long PAUSA_PRE_ARRANQUE_MS      = 250;
const unsigned long PAUSA_PRE_REVERSA_MS       = 800;
const unsigned long REVERSA_BASE_MS            = 750;
const unsigned long INCREMENTO_REVERSA_MS      = 200;
const unsigned long EXTRA_EMPUJE_RECUP_MS      = 250;
const unsigned long MAX_EMPUJE_RECUP_MS        = 1200;
const unsigned long PAUSA_POST_REVERSA_MS      = 350;

// --- Balanza y umbrales de dosificación ---
float factorCalibracion                        = 467.1;
const float ZONA_MUERTA_PLATO                  = 1.5;
const float ZONA_MUERTA_AVANCE                 = 0.4;
const float MARGEN_FRENADO_G                   = 12.5;
const float TOLERANCIA_FINAL_G                 = 0.8;

// --- Pulsos adaptativos en fase fina ---
const unsigned long PULSO_MUY_LARGO_MS         = 650;
const unsigned long PULSO_LARGO_MS             = 480;
const unsigned long PULSO_MEDIO_MS             = 350;
const unsigned long PULSO_CORTO_MS             = 240;
const unsigned long EXTRA_POR_FALLO_MS         = 120;
const unsigned long PULSO_MAX_MS               = 850;
const unsigned long PAUSA_ASENTAMIENTO_MS      = 650;

// ==========================================
// 3. OBJETOS Y VARIABLES DE ESTADO
// ==========================================
HX711 balanza;
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
Servo servoDispensador;
Servo servoTapaR;
Servo servoTapaL;

enum EstadoDispensa {
  ESTADO_INACTIVO,
  ESTADO_ABRIENDO_COMPUERTA,
  ESTADO_PRE_REVERSA,
  ESTADO_PRE_PAUSA,
  ESTADO_RAPIDO,
  ESTADO_PULSO,
  ESTADO_ESPERA,
  ESTADO_DESATASCO_FRENO,
  ESTADO_DESATASCO_REV,
  ESTADO_DESATASCO_PAUSA
};

EstadoDispensa estadoDispensa         = ESTADO_INACTIVO;
EstadoDispensa estadoRetornoDesatasco = ESTADO_INACTIVO;

// Control de peso y flujo
float pesoInicial         = 0.0;
float pesoObjetivo        = 0.0;
float ultimoPesoAvance    = 0.0;
float pesoAnteriorRapido  = 0.0;
float pesoUltimoDesatasco = 0.0;
String origenDispensa     = "Manual";
bool pesoInicialTomado    = false;

// Variables para pausar y reanudar ante tag no autorizado
bool dispensaPausadaPorIntruso = false;
float gramosPendientes         = 0.0;
String origenPendiente         = "";

// Temporizadores de dispensa
unsigned long inicioDispensa        = 0;
unsigned long tiempoUltimoAvance    = 0;
unsigned long marcaTiempoFase       = 0;
unsigned long duracionPulsoActual   = PULSO_MEDIO_MS;
unsigned long duracionReversaActual = REVERSA_BASE_MS;

int  pulsosSinAvance    = 0;
int  intentosDesatasco  = 0;
int  lecturasCortePulso = 0;
bool veniaDePulsoFino   = false;

// Control de compuertas y RFID
bool tapaAbierta                          = false;
bool tagAutorizadoPresente                = false;
bool tagNoAutorizadoPresente              = false;
unsigned long ultimaLecturaRFID           = 0;
unsigned long ultimaDeteccionAutorizada   = 0;
unsigned long ultimaDeteccionNoAutorizada = 0;

// Temporizadores de Firebase
unsigned long ultimoChequeoManual   = 0;
unsigned long ultimoChequeoHorarios = 0;
unsigned long ultimoEnvioPeso       = 0;
String ultimoMinutoDispensado       = "";

// Prototipos adelantados
void iniciarDispensa(float gramosPorServir, String origen);
void pausarDispensaPorIntruso();
void detenerServo(const char* motivo, bool cerrarInmediato = false, bool esAlertaTolva = false, bool registrarLog = true);

// ==========================================
// 4. FUNCIONES DE FIREBASE Y RELOJ NTP
// ==========================================
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
  if (!getLocalTime(&timeinfo)) return "2026-11-25 00:00:00";
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buffer);
}

// ==========================================
// 5. FUNCIONES DE COMPUERTAS Y RFID / NFC
// ==========================================
void abrirCompuertas() {
  if (tapaAbierta) return;
  servoTapaR.write(ANGULO_ABIERTO_R);
  servoTapaL.write(ANGULO_ABIERTO_L);
  tapaAbierta = true;
  Serial.println(">> Compuertas ABIERTAS (R y L).");
}

void cerrarCompuertas() {
  if (!tapaAbierta) return;
  servoTapaR.write(ANGULO_CERRADO_R);
  servoTapaL.write(ANGULO_CERRADO_L);
  tapaAbierta = false;
  Serial.println(">> Compuertas CERRADAS (R y L).");
}

void inicializarCompuertasCerradas() {
  servoTapaR.write(ANGULO_CERRADO_R);
  servoTapaL.write(ANGULO_CERRADO_L);
  tapaAbierta = false;
}

bool detectarTarjeta() {
  byte bufferATQA[2];
  byte bufferSize = sizeof(bufferATQA);

  rfid.PCD_WriteRegister(MFRC522::TxModeReg, 0x00);
  rfid.PCD_WriteRegister(MFRC522::RxModeReg, 0x00);
  rfid.PCD_WriteRegister(MFRC522::ModWidthReg, 0x26);

  MFRC522::StatusCode status = rfid.PICC_WakeupA(bufferATQA, &bufferSize);
  if (status != MFRC522::STATUS_OK && status != MFRC522::STATUS_COLLISION) {
    return false;
  }
  return rfid.PICC_ReadCardSerial();
}

bool esTagAutorizado() {
  if (rfid.uid.size != UID_AUTORIZADO_LEN) return false;
  for (byte i = 0; i < UID_AUTORIZADO_LEN; i++) {
    if (rfid.uid.uidByte[i] != UID_AUTORIZADO[i]) return false;
  }
  return true;
}

void imprimirUID() {
  Serial.print("UID:");
  for (byte i = 0; i < rfid.uid.size; i++) {
    Serial.print(rfid.uid.uidByte[i] < 0x10 ? " 0" : " ");
    Serial.print(rfid.uid.uidByte[i], HEX);
  }
  Serial.println();
}

void actualizarCompuertas(unsigned long ahora) {
  if (tagAutorizadoPresente && (ahora - ultimaDeteccionAutorizada >= TIEMPO_CIERRE_MS)) {
    tagAutorizadoPresente = false;
    Serial.println(">> Tag autorizado alejado.");
  }

  if (tagNoAutorizadoPresente && (ahora - ultimaDeteccionNoAutorizada >= TIEMPO_CIERRE_MS)) {
    tagNoAutorizadoPresente = false;
    Serial.println(">> Tag no autorizado alejado. Bloqueo liberado.");

    // Reanudar automáticamente si había una dispensa pausada
    if (dispensaPausadaPorIntruso) {
      dispensaPausadaPorIntruso = false;
      if (gramosPendientes > TOLERANCIA_FINAL_G) {
        Serial.printf(">> Reanudando dispensa pausada (+%.1f g restantes)...\n", gramosPendientes);
        iniciarDispensa(gramosPendientes, origenPendiente);
      } else {
        Serial.println(">> La racion ya se habia completado antes de la pausa.");
      }
    }
  }

  bool debeEstarAbierta = !tagNoAutorizadoPresente &&
                          (tagAutorizadoPresente || estadoDispensa != ESTADO_INACTIVO);

  if (debeEstarAbierta && !tapaAbierta) {
    abrirCompuertas();
  } else if (!debeEstarAbierta && tapaAbierta) {
    cerrarCompuertas();
  }
}

void actualizarRFID() {
  unsigned long ahora = millis();

  if (ahora - ultimaLecturaRFID >= INTERVALO_LECTURA_RFID_MS) {
    ultimaLecturaRFID = ahora;

    if (detectarTarjeta()) {
      if (esTagAutorizado()) {
        ultimaDeteccionAutorizada = ahora;
        bool esNuevaVisita = !tagAutorizadoPresente;

        tagAutorizadoPresente = true;
        tagNoAutorizadoPresente = false;

        if (esNuevaVisita) {
          imprimirUID();
          Serial.println(">> NFC AUTORIZADO -> Acceso permitido.");
          abrirCompuertas();

          String fecha = obtenerFechaCompleta();
          String logJson = "{\"tipo\":\"acceso_nfc\",\"detalle\":\"Tapa abierta - Gato detectado\",\"timestamp\":\"" + fecha + "\"}";
          enviarDatoPOST("/logs.json", logJson);
          ultimaDeteccionAutorizada = millis();
        }
      } else {
        ultimaDeteccionNoAutorizada = ahora;
        if (!tagNoAutorizadoPresente) {
          imprimirUID();
          Serial.println("!! NFC NO AUTORIZADO -> Compuertas y dispensa BLOQUEADAS.");
        }
        tagNoAutorizadoPresente = true;
        tagAutorizadoPresente = false;

        // En lugar de cancelar, pausamos para retomar cuando se aleje
        if (estadoDispensa != ESTADO_INACTIVO) {
          pausarDispensaPorIntruso();
        }
      }

      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
    }
  }

  actualizarCompuertas(millis());
}

// ==========================================
// 6. FUNCIONES AUXILIARES DE BALANZA Y MOTOR
// ==========================================
float aplicarZonaMuerta(float valor, float umbral = ZONA_MUERTA_PLATO) {
  return (fabs(valor) < umbral) ? 0.0f : valor;
}

float leerPeso(int muestras) {
  return aplicarZonaMuerta(balanza.get_units(muestras), ZONA_MUERTA_PLATO);
}

float leerCrudoMediana3() {
  float a = balanza.get_units(1);
  float b = balanza.get_units(1);
  float c = balanza.get_units(1);

  if (a > b) { float tmp = a; a = b; b = tmp; }
  if (b > c) { float tmp = b; b = c; c = tmp; }
  if (a > b) { float tmp = a; a = b; b = tmp; }
  return b;
}

float leerPesoPlato() {
  float peso = aplicarZonaMuerta(leerCrudoMediana3(), ZONA_MUERTA_PLATO);
  return (peso > 0.0f) ? peso : 0.0f;
}

void arrancarMotorDispensador(int pulsoUs) {
  servoDispensador.attach(SERVO_DISPENSADOR_PIN, 300, 2500);
  servoDispensador.writeMicroseconds(pulsoUs);
}

unsigned long calcularDuracionPulso(float faltante, int sinAvance) {
  unsigned long base;
  if (faltante > 7.0f)      base = PULSO_MUY_LARGO_MS;
  else if (faltante > 3.5f) base = PULSO_LARGO_MS;
  else if (faltante > 1.5f) base = PULSO_MEDIO_MS;
  else                      base = PULSO_CORTO_MS;

  unsigned long total = base + (unsigned long)sinAvance * EXTRA_POR_FALLO_MS;
  return (total > PULSO_MAX_MS) ? PULSO_MAX_MS : total;
}

unsigned long calcularEmpujeRecuperacion(unsigned long reversaMs) {
  unsigned long empuje = reversaMs + EXTRA_EMPUJE_RECUP_MS;
  return (empuje > MAX_EMPUJE_RECUP_MS) ? MAX_EMPUJE_RECUP_MS : empuje;
}

void pasarAEspera(bool desdePulso, unsigned long ahora) {
  servoDispensador.detach();
  estadoDispensa = ESTADO_ESPERA;
  veniaDePulsoFino = desdePulso;
  marcaTiempoFase = ahora;
  lecturasCortePulso = 0;
}

void iniciarPulsoFino(unsigned long duracionMs, unsigned long ahora) {
  duracionPulsoActual = duracionMs;
  estadoDispensa = ESTADO_PULSO;
  veniaDePulsoFino = true;
  marcaTiempoFase = ahora;
  lecturasCortePulso = 0;
  arrancarMotorDispensador(SERVO_FEED_US);
}

void iniciarPreReversa(unsigned long ahora) {
  pesoInicial = leerCrudoMediana3();
  pesoInicialTomado = true;
  ultimoPesoAvance = 0.0;
  pesoAnteriorRapido = 0.0;
  pesoUltimoDesatasco = 0.0;
  tiempoUltimoAvance = ahora;
  marcaTiempoFase = ahora;

  estadoDispensa = ESTADO_PRE_REVERSA;
  arrancarMotorDispensador(SERVO_REV_US);
  Serial.printf("   -> Compuertas listas. Pre-reversa de arranque (%lu ms)...\n", DURACION_PRE_REVERSA_MS);
}

void iniciarFaseAdelante(float faltante, unsigned long reversaPreviaMs, unsigned long ahora) {
  if (faltante > MARGEN_FRENADO_G) {
    estadoDispensa = ESTADO_RAPIDO;
    veniaDePulsoFino = false;
    arrancarMotorDispensador(SERVO_FEED_US);
    Serial.printf("   -> Iniciando fase rapida (corte previo a %.1f g)...\n", pesoObjetivo - MARGEN_FRENADO_G);
  } else {
    unsigned long pulsoInicial = (reversaPreviaMs > 0)
                                 ? calcularEmpujeRecuperacion(reversaPreviaMs)
                                 : calcularDuracionPulso(faltante, 0);
    Serial.printf("   -> Iniciando pulsos finos (empuje inicial %lu ms)...\n", pulsoInicial);
    iniciarPulsoFino(pulsoInicial, ahora);
  }
}

bool registrarAvance(float pesoAgregado, float minAvance, unsigned long ahora) {
  if (pesoAgregado < ultimoPesoAvance + minAvance) return false;

  ultimoPesoAvance = pesoAgregado;
  tiempoUltimoAvance = ahora;

  if (intentosDesatasco > 0 && (pesoAgregado >= pesoUltimoDesatasco + AVANCE_RESET_DESATASCO_G)) {
    intentosDesatasco = 0;
  }
  return true;
}

// ==========================================
// 7. CONTROL DE DISPENSA, PAUSA, DESATASCO Y FIREBASE
// ==========================================
void pausarDispensaPorIntruso() {
  servoDispensador.detach();
  estadoDispensa = ESTADO_INACTIVO;
  cerrarCompuertas();

  float servidoHastaAhora = 0.0f;
  if (pesoInicialTomado && balanza.is_ready()) {
    float lectura = leerCrudoMediana3() - pesoInicial;
    if (lectura > 0.0f) servidoHastaAhora = lectura;
  }

  gramosPendientes = pesoObjetivo - servidoHastaAhora;
  if (gramosPendientes < 0.0f) gramosPendientes = 0.0f;
  origenPendiente = origenDispensa;
  dispensaPausadaPorIntruso = true;

  Serial.printf(">> PAUSADO POR SEGURIDAD: Servido %.1f g | Pendiente %.1f g (se reanudara al alejarse)\n",
                servidoHastaAhora, gramosPendientes);
}

void detenerServo(const char* motivo, bool cerrarInmediato, bool esAlertaTolva, bool registrarLog) {
  servoDispensador.detach();
  estadoDispensa = ESTADO_INACTIVO;
  pesoInicialTomado = false;
  Serial.printf(">> DETENIDO: %s\n", motivo);

  if (cerrarInmediato) {
    cerrarCompuertas();
  }

  delay(650);
  float pesoAgregado = 0.0f;
  float pesoTotalLimpio = 0.0f;

  if (balanza.is_ready()) {
    float pesoFinalCrudo = leerCrudoMediana3();
    pesoAgregado = aplicarZonaMuerta(pesoFinalCrudo - pesoInicial, ZONA_MUERTA_AVANCE);
    pesoTotalLimpio = aplicarZonaMuerta(pesoFinalCrudo, ZONA_MUERTA_PLATO);
    Serial.printf("   Agregado real: %.1f g | Total en plato: %.1f g\n\n", pesoAgregado, pesoTotalLimpio);

    enviarDatoPUT("/dispositivo/peso_actual_plato.json", String((int)round(pesoTotalLimpio)));
  }

  if (!tagAutorizadoPresente) {
    cerrarCompuertas();
  }

  if (esAlertaTolva) {
    enviarDatoPUT("/dispositivo/tolva_baja.json", "true");
  } else if (registrarLog) {
    enviarDatoPUT("/dispositivo/tolva_baja.json", "false");
    String fecha = obtenerFechaCompleta();
    String detalle = String((int)round(pesoAgregado)) + "g servidos (" + origenDispensa + ")";
    String logJson = "{\"tipo\":\"dispensacion\",\"detalle\":\"" + detalle + "\",\"timestamp\":\"" + fecha + "\"}";
    enviarDatoPOST("/logs.json", logJson);
  }
}

void iniciarDesatasco(EstadoDispensa estadoRetorno, const char* origen) {
  servoDispensador.detach();

  if (intentosDesatasco >= MAX_INTENTOS_DESATASCO) {
    detenerServo("SEGURIDAD: Atasco persistente o tolva vacia", false, true, false);
    return;
  }

  intentosDesatasco++;
  pesoUltimoDesatasco = ultimoPesoAvance;
  estadoRetornoDesatasco = estadoRetorno;
  duracionReversaActual = REVERSA_BASE_MS + (unsigned long)(intentosDesatasco - 1) * INCREMENTO_REVERSA_MS;
  estadoDispensa = ESTADO_DESATASCO_FRENO;
  marcaTiempoFase = millis();

  Serial.printf("   !! POSIBLE ATASCO en %s -> Desatasco (%d/%d, rev %lu ms)...\n",
                origen, intentosDesatasco, MAX_INTENTOS_DESATASCO, duracionReversaActual);
}

void iniciarDispensa(float gramosPorServir, String origen) {
  if (gramosPorServir <= 0.0f || estadoDispensa != ESTADO_INACTIVO) return;

  if (tagNoAutorizadoPresente) {
    Serial.println("!! DISPENSA EN ESPERA: Tag NFC no autorizado presente. Se servira al alejarse.\n");
    gramosPendientes = gramosPorServir;
    origenPendiente = origen;
    dispensaPausadaPorIntruso = true;
    cerrarCompuertas();
    return;
  }

  pesoObjetivo = gramosPorServir;
  origenDispensa = origen;
  pesoInicialTomado = false;
  pulsosSinAvance = 0;
  intentosDesatasco = 0;
  lecturasCortePulso = 0;
  veniaDePulsoFino = false;

  unsigned long ahora = millis();
  inicioDispensa = ahora;
  tiempoUltimoAvance = ahora;
  marcaTiempoFase = ahora;

  Serial.printf(">> DISPENSANDO +%.1f g [%s]...\n", pesoObjetivo, origen.c_str());

  if (!tapaAbierta) {
    abrirCompuertas();
    estadoDispensa = ESTADO_ABRIENDO_COMPUERTA;
  } else {
    iniciarPreReversa(ahora);
  }
}

void actualizarDispensa() {
  if (estadoDispensa == ESTADO_INACTIVO) return;

  unsigned long ahora = millis();

  if (ahora - inicioDispensa >= TIMEOUT_GLOBAL_MS) {
    detenerServo("SEGURIDAD: Timeout global alcanzado", false, true, false);
    return;
  }

  switch (estadoDispensa) {
    case ESTADO_ABRIENDO_COMPUERTA:
      if (ahora - marcaTiempoFase >= MARGEN_APERTURA_COMPUERTA) {
        if (!balanza.is_ready()) return;
        iniciarPreReversa(millis());
      }
      break;

    case ESTADO_PRE_REVERSA:
      if (ahora - marcaTiempoFase >= DURACION_PRE_REVERSA_MS) {
        servoDispensador.detach();
        estadoDispensa = ESTADO_PRE_PAUSA;
        marcaTiempoFase = ahora;
      }
      break;

    case ESTADO_PRE_PAUSA:
      if (ahora - marcaTiempoFase >= PAUSA_PRE_ARRANQUE_MS) {
        if (!balanza.is_ready()) return;

        float pesoAgregado = leerCrudoMediana3() - pesoInicial;
        if (pesoAgregado >= (pesoObjetivo - TOLERANCIA_FINAL_G)) {
          detenerServo("Peso objetivo alcanzado tras pre-reversa");
          return;
        }
        if (pesoAgregado > 0.0f) {
          ultimoPesoAvance = pesoAgregado;
          pesoAnteriorRapido = pesoAgregado;
        }

        unsigned long instante = millis();
        tiempoUltimoAvance = instante;
        iniciarFaseAdelante(pesoObjetivo - pesoAgregado, DURACION_PRE_REVERSA_MS, instante);
      }
      break;

    case ESTADO_RAPIDO:
      if (balanza.is_ready()) {
        float pesoAgregado = balanza.get_units(1) - pesoInicial;
        float delta = pesoAgregado - pesoAnteriorRapido;
        pesoAnteriorRapido = pesoAgregado;

        Serial.printf("[Rapido] %+.1f / %.1f g\n",
                      aplicarZonaMuerta(pesoAgregado, ZONA_MUERTA_AVANCE), pesoObjetivo);
        registrarAvance(pesoAgregado, MIN_AVANCE_RAPIDO_G, ahora);

        float avanceEstimado = constrain(delta, 0.0f, 4.0f);
        float pesoProyectado = (pesoAgregado > 3.0f) ? (pesoAgregado + avanceEstimado) : pesoAgregado;

        if (pesoProyectado >= (pesoObjetivo - MARGEN_FRENADO_G)) {
          pasarAEspera(false, ahora);
          tiempoUltimoAvance = ahora;
          return;
        }
      }

      {
        unsigned long limiteSinFlujo = (intentosDesatasco == 0 && ultimoPesoAvance < MIN_AVANCE_RAPIDO_G)
                                       ? TIMEOUT_ARRANQUE_MS
                                       : TIMEOUT_SIN_FLUJO_MS;
        if (ahora - tiempoUltimoAvance >= limiteSinFlujo) {
          iniciarDesatasco(ESTADO_RAPIDO, "fase rapida");
        }
      }
      break;

    case ESTADO_PULSO:
      if (duracionPulsoActual >= 600 && balanza.is_ready()) {
        float pesoVivo = balanza.get_units(1) - pesoInicial;
        if (pesoVivo >= (pesoObjetivo - TOLERANCIA_FINAL_G)) {
          lecturasCortePulso++;
          if (lecturasCortePulso >= 2) {
            pasarAEspera(true, ahora);
            return;
          }
        } else {
          lecturasCortePulso = 0;
        }
      }

      if (ahora - marcaTiempoFase >= duracionPulsoActual) {
        pasarAEspera(true, ahora);
      }
      break;

    case ESTADO_ESPERA:
      if (ahora - marcaTiempoFase >= PAUSA_ASENTAMIENTO_MS) {
        if (!balanza.is_ready()) {
          if (ahora - tiempoUltimoAvance >= TIMEOUT_SIN_FLUJO_MS) {
            detenerServo("SEGURIDAD: HX711 sin respuesta en fase fina", false, false, false);
          }
          return;
        }

        float pesoAgregado = leerCrudoMediana3() - pesoInicial;
        Serial.printf("[Fino]   %+.1f / %.1f g\n",
                      aplicarZonaMuerta(pesoAgregado, ZONA_MUERTA_AVANCE), pesoObjetivo);

        if (pesoAgregado >= (pesoObjetivo - TOLERANCIA_FINAL_G)) {
          detenerServo("Peso objetivo alcanzado");
          return;
        }

        if (registrarAvance(pesoAgregado, MIN_AVANCE_FINO_G, ahora)) {
          pulsosSinAvance = 0;
        } else if (veniaDePulsoFino) {
          pulsosSinAvance++;
          if (pulsosSinAvance >= MAX_PULSOS_SIN_AVANCE) {
            iniciarDesatasco(ESTADO_PULSO, "fase fina");
            return;
          }
        }

        float faltante = pesoObjetivo - pesoAgregado;
        iniciarPulsoFino(calcularDuracionPulso(faltante, pulsosSinAvance), millis());
      }
      break;

    case ESTADO_DESATASCO_FRENO:
      if (ahora - marcaTiempoFase >= PAUSA_PRE_REVERSA_MS) {
        arrancarMotorDispensador(SERVO_REV_US);
        estadoDispensa = ESTADO_DESATASCO_REV;
        marcaTiempoFase = ahora;
      }
      break;

    case ESTADO_DESATASCO_REV:
      if (ahora - marcaTiempoFase >= duracionReversaActual) {
        servoDispensador.detach();
        estadoDispensa = ESTADO_DESATASCO_PAUSA;
        marcaTiempoFase = ahora;
      }
      break;

    case ESTADO_DESATASCO_PAUSA:
      if (ahora - marcaTiempoFase >= PAUSA_POST_REVERSA_MS) {
        float pesoAgregado = ultimoPesoAvance;
        if (balanza.is_ready()) {
          pesoAgregado = leerCrudoMediana3() - pesoInicial;
          if (pesoAgregado >= (pesoObjetivo - TOLERANCIA_FINAL_G)) {
            detenerServo("Peso objetivo alcanzado tras desatasco");
            return;
          }
          if (pesoAgregado > ultimoPesoAvance) {
            ultimoPesoAvance = pesoAgregado;
          }
        }

        unsigned long instante = millis();
        tiempoUltimoAvance = instante;
        pulsosSinAvance = 0;
        pesoAnteriorRapido = pesoAgregado;

        float faltante = pesoObjetivo - pesoAgregado;
        if (estadoRetornoDesatasco == ESTADO_RAPIDO && faltante > MARGEN_FRENADO_G) {
          iniciarFaseAdelante(faltante, 0, instante);
        } else {
          float faltanteAcotado = (faltante < MARGEN_FRENADO_G) ? faltante : MARGEN_FRENADO_G;
          iniciarFaseAdelante(faltanteAcotado, duracionReversaActual, instante);
        }
      }
      break;

    default:
      break;
  }
}

// ==========================================
// 8. CONSULTAS DE RUTINAS Y ÓRDENES DESDE FIREBASE
// ==========================================
void revisarOrdenDispensar() {
  String estado = leerDatoGET("/dispositivo/dispensar_ahora.json");
  if (estado == "true") {
    enviarDatoPUT("/dispositivo/dispensar_ahora.json", "false");
    iniciarDispensa(50.0, "App Manual");
  }
}

void comprobarHorariosFirebase() {
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
    float gramosMeta = rutina["gramos"] | 0.0;

    if (activa && horaProgramada == horaActual && gramosMeta > 0.0f) {
      ultimoMinutoDispensado = horaActual;

      float pesoEnPlato = balanza.is_ready() ? leerPesoPlato() : 0.0f;
      float gramosFaltantes = gramosMeta - pesoEnPlato;

      Serial.printf("\n=== RUTINA %s | Meta: %.1f g | En plato: %.1f g ===\n",
                    horaActual.c_str(), gramosMeta, pesoEnPlato);

      if (gramosFaltantes <= TOLERANCIA_FINAL_G) {
        Serial.println("    El plato ya cumple con la racion. No se dispensa.\n");
      } else {
        iniciarDispensa(gramosFaltantes, "Rutina " + horaActual);
      }
      break;
    }
  }
}

// ==========================================
// 9. SETUP Y LOOP PRINCIPAL
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(300);

  SPI.begin();
  rfid.PCD_Init();

  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);

  servoDispensador.setPeriodHertz(50);
  servoDispensador.attach(SERVO_DISPENSADOR_PIN, 300, 2500);
  servoDispensador.detach();

  servoTapaR.setPeriodHertz(50);
  servoTapaL.setPeriodHertz(50);
  servoTapaR.attach(SERVO_TAPA_R_PIN, 500, 2400);
  servoTapaL.attach(SERVO_TAPA_L_PIN, 500, 2400);
  inicializarCompuertasCerradas();

  balanza.begin(DT_PIN, SCK_PIN);
  Serial.println("No pongas peso en la balanza (solo el plato vacio)...");
  delay(2000);
  balanza.set_scale(factorCalibracion);
  balanza.tare(20);

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
  enviarDatoPUT("/dispositivo/tolva_baja.json", "false");

  Serial.println("\n=== COMEDERO COMPLETO LISTO (HARDWARE AVANZADO + FIREBASE) ===\n");
}

void loop() {
  actualizarRFID();
  actualizarDispensa();

  // Solo consultar Firebase cuando está en reposo y no hay una pausa pendiente
  if (estadoDispensa == ESTADO_INACTIVO && !dispensaPausadaPorIntruso) {
    unsigned long ahora = millis();

    if (ahora - ultimoChequeoManual >= 3000) {
      ultimoChequeoManual = ahora;
      revisarOrdenDispensar();
    }

    if (ahora - ultimoChequeoHorarios >= 10000) {
      ultimoChequeoHorarios = ahora;
      comprobarHorariosFirebase();
    }

    if (ahora - ultimoEnvioPeso >= 5000) {
      ultimoEnvioPeso = ahora;
      if (balanza.is_ready()) {
        float peso = leerPeso(5);
        Serial.printf("[%s] Peso: %.1f g | Compuertas: %s\n",
                      obtenerHoraMinuto().c_str(), peso, tapaAbierta ? "ABIERTAS" : "CERRADAS");
        enviarDatoPUT("/dispositivo/peso_actual_plato.json", String((int)round(peso)));
      }
    }
  }
}