#include <Arduino.h>
#include <ESP32Servo.h>
#include "HX711.h"

// ==========================================
// 1. PINES Y CONFIGURACIÓN
// ==========================================
#define DT_PIN         16
#define SCK_PIN        17
#define SERVO_PIN      18

#define SERVO_FEED_US  400    // Pulso de giro normal (dispensar)
#define SERVO_REV_US   2400   // Pulso de giro en reversa (desatasco)

// Protecciones de seguridad y desatasco
const unsigned long TIMEOUT_GLOBAL_MS          = 60000; // Limite maximo total ampliado (60 s)
const unsigned long TIMEOUT_ARRANQUE_MS        = 3500;  // Destraba antes si el arranque sale atascado
const unsigned long TIMEOUT_SIN_FLUJO_MS       = 3500;  // Limite sin flujo una vez que ya cayo comida
const float MIN_AVANCE_RAPIDO_G                = 1.0;
const float MIN_AVANCE_FINO_G                  = 0.3;
const float AVANCE_RESET_DESATASCO_G           = 4.0;
const int   MAX_PULSOS_SIN_AVANCE              = 4;     // Pulsos finos sin avance antes de desatascar
const int   MAX_INTENTOS_DESATASCO             = 3;

const unsigned long PAUSA_PRE_REVERSA_MS       = 200;
const unsigned long DURACION_REVERSA_RAPIDA_MS = 450;   // Reversa aumentada en fase rapida
const unsigned long DURACION_REVERSA_FINA_MS   = 450;   // Reversa aumentada en fase fina (antes 200 ms)
const unsigned long PULSO_RECUPERACION_MS      = 700;   // Empuje tras reversa fina (> 450 ms para avanzar)
const unsigned long PAUSA_POST_REVERSA_MS      = 350;

// Ajustes de balanza y dosificación precisa
float factorCalibracion = 467.1;
const float ZONA_MUERTA = 1.5;

const float MARGEN_FRENADO_G   = 12.5;
const float TOLERANCIA_FINAL_G = 0.8;

// Pulsos adaptativos en fase fina (tiempos aumentados)
const unsigned long PULSO_MUY_LARGO_MS    = 650; // Faltan > 7.0 g
const unsigned long PULSO_LARGO_MS        = 480; // Faltan > 3.5 g
const unsigned long PULSO_MEDIO_MS        = 350; // Faltan > 1.5 g
const unsigned long PULSO_CORTO_MS        = 240; // Faltan <= 1.5 g
const unsigned long EXTRA_POR_FALLO_MS    = 120; // Incremento automatico por cada pulso sin caida
const unsigned long PULSO_MAX_MS          = 850; // Tope maximo de un pulso fino
const unsigned long PAUSA_ASENTAMIENTO_MS = 650;

HX711 balanza;
Servo servoDispensador;

enum EstadoDispensa {
  ESTADO_INACTIVO,
  ESTADO_RAPIDO,
  ESTADO_PULSO,
  ESTADO_ESPERA,
  ESTADO_DESATASCO_FRENO,
  ESTADO_DESATASCO_REV,
  ESTADO_DESATASCO_PAUSA
};

EstadoDispensa estadoDispensa = ESTADO_INACTIVO;
EstadoDispensa estadoRetornoDesatasco = ESTADO_INACTIVO;

float pesoInicial = 0.0;
float pesoObjetivo = 0.0;
float ultimoPesoAvance = 0.0;
float pesoAnteriorRapido = 0.0;
float pesoUltimoDesatasco = 0.0;

unsigned long inicioDispensa = 0;
unsigned long tiempoUltimoAvance = 0;
unsigned long marcaTiempoFase = 0;
unsigned long duracionPulsoActual = PULSO_MEDIO_MS;
unsigned long duracionReversaActual = DURACION_REVERSA_RAPIDA_MS;
unsigned long ultimaImpresion = 0;

int pulsosSinAvance = 0;
int intentosDesatasco = 0;
bool veniaDePulsoFino = false;

// ==========================================
// 2. LECTURA LIMPIA Y CONTROL DE MOTOR
// ==========================================
float leerPeso(int muestras) {
  float peso = balanza.get_units(muestras);
  if (fabs(peso) < ZONA_MUERTA) return 0.0f;
  return peso;
}

// Toma 3 muestras y devuelve la mediana para descartar picos aislados (ej. +63.3 g)
float leerCrudoMediana3() {
  float a = balanza.get_units(1);
  float b = balanza.get_units(1);
  float c = balanza.get_units(1);

  if (a > b) { float tmp = a; a = b; b = tmp; }
  if (b > c) { float tmp = b; b = c; c = tmp; }
  if (a > b) { float tmp = a; a = b; b = tmp; }
  return b;
}

void arrancarMotor(int pulsoUs) {
  servoDispensador.attach(SERVO_PIN, 300, 2400);
  servoDispensador.writeMicroseconds(pulsoUs);
}

unsigned long calcularDuracionPulso(float faltante, int sinAvance) {
  unsigned long base;
  if (faltante > 7.0f)      base = PULSO_MUY_LARGO_MS;
  else if (faltante > 3.5f) base = PULSO_LARGO_MS;
  else if (faltante > 1.5f) base = PULSO_MEDIO_MS;
  else                      base = PULSO_CORTO_MS;

  unsigned long total = base + (unsigned long)sinAvance * EXTRA_POR_FALLO_MS;
  if (total > PULSO_MAX_MS) total = PULSO_MAX_MS;
  return total;
}

// ==========================================
// 3. CONTROL DEL SERVO (2 FASES + DESATASCO MEJORADO)
// ==========================================
void detenerServo(const char* motivo) {
  servoDispensador.detach();
  estadoDispensa = ESTADO_INACTIVO;
  Serial.printf(">> DETENIDO: %s\n", motivo);

  delay(650);
  if (balanza.is_ready()) {
    float pesoFinalCrudo = leerCrudoMediana3();
    float pesoAgregado = pesoFinalCrudo - pesoInicial;

    if (fabs(pesoAgregado) < ZONA_MUERTA) pesoAgregado = 0.0f;
    float pesoTotalLimpio = (fabs(pesoFinalCrudo) < ZONA_MUERTA) ? 0.0f : pesoFinalCrudo;

    Serial.printf("   Agregado real: %.1f g | Total en plato: %.1f g\n\n", pesoAgregado, pesoTotalLimpio);
  } else {
    Serial.println("   (Balanza no respondio al finalizar; revisa cableado/alimentacion)\n");
  }
}

void iniciarDesatasco(EstadoDispensa estadoRetorno, const char* origen) {
  servoDispensador.detach();

  if (intentosDesatasco >= MAX_INTENTOS_DESATASCO) {
    detenerServo("SEGURIDAD: Atasco persistente o tolva vacia (sin mas reintentos)");
    return;
  }

  intentosDesatasco++;
  pesoUltimoDesatasco = ultimoPesoAvance;
  estadoRetornoDesatasco = estadoRetorno;
  duracionReversaActual = (estadoRetorno == ESTADO_RAPIDO)
                          ? DURACION_REVERSA_RAPIDA_MS
                          : DURACION_REVERSA_FINA_MS;
  estadoDispensa = ESTADO_DESATASCO_FRENO;
  marcaTiempoFase = millis();

  Serial.printf("   !! POSIBLE ATASCO en %s -> Desatasco (%d/%d, rev %lu ms)...\n",
                origen, intentosDesatasco, MAX_INTENTOS_DESATASCO, duracionReversaActual);
}

void iniciarDispensa(float gramosPorServir) {
  if (gramosPorServir <= 0) {
    Serial.println("Ingresa un peso mayor a 0 g.");
    return;
  }

  pesoInicial = balanza.get_units(10);
  pesoObjetivo = gramosPorServir;
  ultimoPesoAvance = 0.0;
  pesoAnteriorRapido = 0.0;
  pesoUltimoDesatasco = 0.0;
  pulsosSinAvance = 0;
  intentosDesatasco = 0;
  veniaDePulsoFino = false;

  unsigned long ahora = millis();
  inicioDispensa = ahora;
  tiempoUltimoAvance = ahora;

  if (pesoObjetivo > MARGEN_FRENADO_G) {
    estadoDispensa = ESTADO_RAPIDO;
    arrancarMotor(SERVO_FEED_US);
    Serial.printf(">> DISPENSANDO +%.1f g (fase rapida hasta %.1f g)...\n",
                  pesoObjetivo, pesoObjetivo - MARGEN_FRENADO_G);
  } else {
    estadoDispensa = ESTADO_PULSO;
    veniaDePulsoFino = true;
    duracionPulsoActual = calcularDuracionPulso(pesoObjetivo, 0);
    marcaTiempoFase = ahora;
    arrancarMotor(SERVO_FEED_US);
    Serial.printf(">> DISPENSANDO +%.1f g (directo en pulsos finos)...\n", pesoObjetivo);
  }
}

void actualizarDispensa() {
  if (estadoDispensa == ESTADO_INACTIVO) return;

  unsigned long ahora = millis();

  if (ahora - inicioDispensa >= TIMEOUT_GLOBAL_MS) {
    detenerServo("SEGURIDAD: Timeout global alcanzado");
    return;
  }

  switch (estadoDispensa) {
    case ESTADO_RAPIDO:
      if (balanza.is_ready()) {
        float pesoAgregado = balanza.get_units(1) - pesoInicial;
        float delta = pesoAgregado - pesoAnteriorRapido;
        pesoAnteriorRapido = pesoAgregado;

        float pesoMostrar = (fabs(pesoAgregado) < ZONA_MUERTA) ? 0.0f : pesoAgregado;
        Serial.printf("[Rapido] %+.1f / %.1f g\n", pesoMostrar, pesoObjetivo);

        if (pesoAgregado >= ultimoPesoAvance + MIN_AVANCE_RAPIDO_G) {
          ultimoPesoAvance = pesoAgregado;
          tiempoUltimoAvance = ahora;

          if (intentosDesatasco > 0 && (pesoAgregado >= pesoUltimoDesatasco + AVANCE_RESET_DESATASCO_G)) {
            intentosDesatasco = 0;
          }
        }

        float avanceEstimado = constrain(delta, 0.0f, 4.0f);
        float pesoProyectado = (pesoAgregado > 3.0f) ? (pesoAgregado + avanceEstimado) : pesoAgregado;

        if (pesoProyectado >= (pesoObjetivo - MARGEN_FRENADO_G)) {
          servoDispensador.detach();
          estadoDispensa = ESTADO_ESPERA;
          veniaDePulsoFino = false;
          marcaTiempoFase = ahora;
          tiempoUltimoAvance = ahora;
          Serial.println("   -> Frenado previo, esperando caida en vuelo...");
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
      if (ahora - marcaTiempoFase >= duracionPulsoActual) {
        servoDispensador.detach();
        estadoDispensa = ESTADO_ESPERA;
        veniaDePulsoFino = true;
        marcaTiempoFase = ahora;
      }
      break;

    case ESTADO_ESPERA:
      if (ahora - marcaTiempoFase >= PAUSA_ASENTAMIENTO_MS) {
        if (!balanza.is_ready()) {
          if (ahora - tiempoUltimoAvance >= TIMEOUT_SIN_FLUJO_MS) {
            detenerServo("SEGURIDAD: HX711 sin respuesta en fase fina");
          }
          return;
        }

        float pesoAgregado = leerCrudoMediana3() - pesoInicial;
        float pesoMostrar = (fabs(pesoAgregado) < ZONA_MUERTA) ? 0.0f : pesoAgregado;
        Serial.printf("[Fino]   %+.1f / %.1f g\n", pesoMostrar, pesoObjetivo);

        if (pesoAgregado >= (pesoObjetivo - TOLERANCIA_FINAL_G)) {
          detenerServo("Peso objetivo alcanzado");
          return;
        }

        if (pesoAgregado >= ultimoPesoAvance + MIN_AVANCE_FINO_G) {
          ultimoPesoAvance = pesoAgregado;
          tiempoUltimoAvance = ahora;
          pulsosSinAvance = 0;

          if (intentosDesatasco > 0 && (pesoAgregado >= pesoUltimoDesatasco + AVANCE_RESET_DESATASCO_G)) {
            intentosDesatasco = 0;
          }
        } else if (veniaDePulsoFino) {
          pulsosSinAvance++;
          Serial.printf("   (Pulso sin avance %d/%d)\n", pulsosSinAvance, MAX_PULSOS_SIN_AVANCE);

          if (pulsosSinAvance >= MAX_PULSOS_SIN_AVANCE) {
            iniciarDesatasco(ESTADO_PULSO, "fase fina");
            return;
          }
        }

        float faltante = pesoObjetivo - pesoAgregado;
        duracionPulsoActual = calcularDuracionPulso(faltante, pulsosSinAvance);

        estadoDispensa = ESTADO_PULSO;
        marcaTiempoFase = millis();
        arrancarMotor(SERVO_FEED_US);
      }
      break;

    // --- SECUENCIA DE DESATASCO ---
    case ESTADO_DESATASCO_FRENO:
      if (ahora - marcaTiempoFase >= PAUSA_PRE_REVERSA_MS) {
        Serial.println("   <- Girando en reversa...");
        arrancarMotor(SERVO_REV_US);
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

        tiempoUltimoAvance = millis();
        pulsosSinAvance = 0;
        pesoAnteriorRapido = pesoAgregado;

        if (estadoRetornoDesatasco == ESTADO_RAPIDO && (pesoAgregado < (pesoObjetivo - MARGEN_FRENADO_G))) {
          Serial.println("   -> Retomando fase rapida...");
          estadoDispensa = ESTADO_RAPIDO;
          arrancarMotor(SERVO_FEED_US);
        } else {
          duracionPulsoActual = PULSO_RECUPERACION_MS;
          Serial.printf("   -> Retomando fase fina (pulso recuperacion %lu ms)...\n", duracionPulsoActual);
          estadoDispensa = ESTADO_PULSO;
          veniaDePulsoFino = true;
          marcaTiempoFase = millis();
          arrancarMotor(SERVO_FEED_US);
        }
      }
      break;

    default:
      break;
  }
}

// ==========================================
// 4. COMANDOS POR MONITOR SERIE
// ==========================================
void leerComandosSerie() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd.length() == 0) return;

  if (cmd.equalsIgnoreCase("s")) {
    if (estadoDispensa != ESTADO_INACTIVO) detenerServo("Parada manual por usuario");
    else Serial.println("El servo ya esta detenido.");
    return;
  }

  if (cmd.equalsIgnoreCase("r")) {
    if (estadoDispensa != ESTADO_INACTIVO) {
      Serial.println("Deten la dispensa primero con 's' antes de probar reversa.");
      return;
    }
    Serial.printf(">> Probando reversa (%lu ms)...\n", DURACION_REVERSA_FINA_MS);
    arrancarMotor(SERVO_REV_US);
    delay(DURACION_REVERSA_FINA_MS);
    servoDispensador.detach();
    Serial.println(">> Reversa terminada.\n");
    return;
  }

  if (cmd.equalsIgnoreCase("t")) {
    if (estadoDispensa != ESTADO_INACTIVO) detenerServo("Interrumpido para tara");
    Serial.println("Haciendo tara (sin peso en el plato)...");
    balanza.tare(20);
    Serial.println("Cero listo.\n");
    return;
  }

  if (cmd.startsWith("c") || cmd.startsWith("C")) {
    if (estadoDispensa != ESTADO_INACTIVO) detenerServo("Interrumpido para calibracion");
    float pesoConocido = cmd.substring(1).toFloat();
    if (pesoConocido <= 0) {
      Serial.println("Formato invalido. Ejemplo: c 100");
      return;
    }
    double lecturaNeta = balanza.get_value(15);
    factorCalibracion = lecturaNeta / pesoConocido;
    balanza.set_scale(factorCalibracion);
    Serial.printf("Calibrado! Tu factorCalibracion = %.3f\n", factorCalibracion);
    Serial.printf("Comprobacion -> Peso actual: %.1f g\n\n", leerPeso(10));
    return;
  }

  float gramos = cmd.toFloat();
  if (gramos > 0) {
    iniciarDispensa(gramos);
  } else {
    Serial.println("Comando no reconocido. Usa: [t], [c <g>], [<g>], [r] o [s]");
  }
}

// ==========================================
// 5. SETUP Y LOOP
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(300);

  ESP32PWM::allocateTimer(0);
  servoDispensador.setPeriodHertz(50);
  servoDispensador.attach(SERVO_PIN, 300, 2400);
  servoDispensador.detach();

  balanza.begin(DT_PIN, SCK_PIN);
  Serial.println("No pongas peso en la balanza (solo el plato vacio)...");
  delay(2000);
  balanza.set_scale(factorCalibracion);
  balanza.tare(20);

  Serial.println("\n=== PRUEBA HX711 + SERVO (2 FASES + RECUPERACION V2) LISTA ===");
  Serial.println("Comandos: [t] tara | [c <g>] calibrar | [<g>] dispensar | [r] probar reversa | [s] parar\n");
}

void loop() {
  leerComandosSerie();
  actualizarDispensa();

  if (estadoDispensa == ESTADO_INACTIVO) {
    if (millis() - ultimaImpresion >= 1000) {
      ultimaImpresion = millis();
      if (balanza.is_ready()) {
        float peso = leerPeso(5);
        Serial.printf("Peso actual: %.1f g\n", peso);
      }
    }
  }
}