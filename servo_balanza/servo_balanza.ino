#include <Arduino.h>
#include <ESP32Servo.h>
#include "HX711.h"

// ==========================================
// 1. PINES Y CONFIGURACIÓN
// ==========================================
#define DT_PIN         16
#define SCK_PIN        17
#define SERVO_PIN      18

#define SERVO_FEED_US  400    // Pulso maximo hacia adelante (dispensar)
#define SERVO_REV_US   2500   // Pulso maximo en reversa (baja a 2400 si tu servo no gira con 'r')

// Protecciones de seguridad y desatasco
const unsigned long TIMEOUT_GLOBAL_MS        = 60000;
const unsigned long TIMEOUT_ARRANQUE_MS      = 2200;
const unsigned long TIMEOUT_SIN_FLUJO_MS     = 2200;
const float MIN_AVANCE_RAPIDO_G              = 1.0;
const float MIN_AVANCE_FINO_G                = 0.3;
const float AVANCE_RESET_DESATASCO_G         = 4.0;
const int   MAX_PULSOS_SIN_AVANCE            = 3;
const int   MAX_INTENTOS_DESATASCO           = 3;

// Tiempos de pre-reversa inicial y desatasco progresivo
const unsigned long DURACION_PRE_REVERSA_MS  = 550;   // Retroceso inicial al empezar cada servida
const unsigned long PAUSA_PRE_ARRANQUE_MS    = 250;   // Tiempo muerto antes de girar hacia adelante
const unsigned long PAUSA_PRE_REVERSA_MS     = 800;
const unsigned long REVERSA_BASE_MS          = 750;   // Intento 1: 600 ms -> 2: 800 ms -> 3: 1000 ms
const unsigned long INCREMENTO_REVERSA_MS    = 200;
const unsigned long EXTRA_EMPUJE_RECUP_MS    = 250;   // Empuje neto hacia adelante tras retroceder
const unsigned long MAX_EMPUJE_RECUP_MS      = 1200;
const unsigned long PAUSA_POST_REVERSA_MS    = 350;

// Ajustes de balanza y dosificación precisa
float factorCalibracion = 467.1;
const float ZONA_MUERTA_PLATO  = 1.5;  // Oculta ruido/deriva del plato en reposo
const float ZONA_MUERTA_AVANCE = 0.4;  // Permite ver incrementos pequenos en fase fina

const float MARGEN_FRENADO_G   = 12.5;
const float TOLERANCIA_FINAL_G = 0.8;

// Pulsos adaptativos en fase fina
const unsigned long PULSO_MUY_LARGO_MS    = 650; // Faltan > 7.0 g
const unsigned long PULSO_LARGO_MS        = 480; // Faltan > 3.5 g
const unsigned long PULSO_MEDIO_MS        = 350; // Faltan > 1.5 g
const unsigned long PULSO_CORTO_MS        = 240; // Faltan <= 1.5 g
const unsigned long EXTRA_POR_FALLO_MS    = 120;
const unsigned long PULSO_MAX_MS          = 850;
const unsigned long PAUSA_ASENTAMIENTO_MS = 650;

// ==========================================
// 2. RUTINA DE 3 HORARIOS Y RELOJ SIMULADO
// ==========================================
struct HorarioComida {
  const char* nombre;
  uint8_t hora;        // Formato 24h (0 - 23)
  uint8_t minuto;      // (0 - 59)
  float gramosMeta;    // Peso total objetivo en el plato
  bool ejecutadoHoy;
};

const int NUM_HORARIOS = 3;
HorarioComida horarios[NUM_HORARIOS] = {
  { "Primera comida",  7, 0, 30.0, false }, // 07:00 (7:00 am)
  { "Segunda comida", 13, 0, 30.0, false }, // 13:00 (1:00 pm)
  { "Tercera comida", 19, 0, 30.0, false }  // 19:00 (7:00 pm)
};

uint8_t horaSim = 6;
uint8_t minutoSim = 55;
uint8_t segundoSim = 0;
unsigned long ultimoTickReloj = 0;

// ==========================================
// 3. OBJETOS Y VARIABLES DE ESTADO
// ==========================================
HX711 balanza;
Servo servoDispensador;

enum EstadoDispensa {
  ESTADO_INACTIVO,
  ESTADO_PRE_REVERSA,
  ESTADO_PRE_PAUSA,
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
unsigned long duracionReversaActual = REVERSA_BASE_MS;
unsigned long ultimaImpresion = 0;

int pulsosSinAvance = 0;
int intentosDesatasco = 0;
int lecturasCortePulso = 0;
bool veniaDePulsoFino = false;

// ==========================================
// 4. FUNCIONES AUXILIARES DE BALANZA Y MOTOR
// ==========================================
float aplicarZonaMuerta(float valor, float umbral = ZONA_MUERTA_PLATO) {
  return (fabs(valor) < umbral) ? 0.0f : valor;
}

float leerPeso(int muestras) {
  return aplicarZonaMuerta(balanza.get_units(muestras), ZONA_MUERTA_PLATO);
}

// Mediana de 3 muestras para descartar picos aislados
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

void arrancarMotor(int pulsoUs) {
  servoDispensador.attach(SERVO_PIN, 300, 2500);
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
  arrancarMotor(SERVO_FEED_US);
}

void iniciarFaseAdelante(float faltante, unsigned long reversaPreviaMs, unsigned long ahora) {
  if (faltante > MARGEN_FRENADO_G) {
    estadoDispensa = ESTADO_RAPIDO;
    veniaDePulsoFino = false;
    arrancarMotor(SERVO_FEED_US);
    Serial.printf("   -> Iniciando fase rapida (corte previo a %.1f g)...\n",
                  pesoObjetivo - MARGEN_FRENADO_G);
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
// 5. CONTROL DE DISPENSA Y DESATASCO
// ==========================================
void detenerServo(const char* motivo) {
  servoDispensador.detach();
  estadoDispensa = ESTADO_INACTIVO;
  Serial.printf(">> DETENIDO: %s\n", motivo);

  delay(650);
  if (balanza.is_ready()) {
    float pesoFinalCrudo = leerCrudoMediana3();
    float pesoAgregado = aplicarZonaMuerta(pesoFinalCrudo - pesoInicial, ZONA_MUERTA_AVANCE);
    float pesoTotalLimpio = aplicarZonaMuerta(pesoFinalCrudo, ZONA_MUERTA_PLATO);

    Serial.printf("   Agregado real: %.1f g | Total en plato: %.1f g\n\n",
                  pesoAgregado, pesoTotalLimpio);
  } else {
    Serial.println("   (Balanza no respondio al finalizar; revisa cableado/alimentacion)\n");
  }
}

void interrumpirSiActivo(const char* motivo) {
  if (estadoDispensa != ESTADO_INACTIVO) {
    detenerServo(motivo);
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
  duracionReversaActual = REVERSA_BASE_MS + (unsigned long)(intentosDesatasco - 1) * INCREMENTO_REVERSA_MS;
  estadoDispensa = ESTADO_DESATASCO_FRENO;
  marcaTiempoFase = millis();

  Serial.printf("   !! POSIBLE ATASCO en %s -> Desatasco fuerte (%d/%d, rev %lu ms a %d us)...\n",
                origen, intentosDesatasco, MAX_INTENTOS_DESATASCO, duracionReversaActual, SERVO_REV_US);
}

void iniciarDispensa(float gramosPorServir) {
  if (gramosPorServir <= 0.0f) {
    Serial.println("Ingresa un peso mayor a 0 g.");
    return;
  }

  pesoInicial = leerCrudoMediana3();
  pesoObjetivo = gramosPorServir;
  ultimoPesoAvance = 0.0;
  pesoAnteriorRapido = 0.0;
  pesoUltimoDesatasco = 0.0;
  pulsosSinAvance = 0;
  intentosDesatasco = 0;
  lecturasCortePulso = 0;
  veniaDePulsoFino = false;

  unsigned long ahora = millis();
  inicioDispensa = ahora;
  tiempoUltimoAvance = ahora;
  marcaTiempoFase = ahora;

  // Siempre arranca con pre-reversa energica para destrabar el mecanismo antes de servir
  estadoDispensa = ESTADO_PRE_REVERSA;
  arrancarMotor(SERVO_REV_US);
  Serial.printf(">> DISPENSANDO +%.1f g | Pre-reversa de arranque (%lu ms a %d us)...\n",
                pesoObjetivo, DURACION_PRE_REVERSA_MS, SERVO_REV_US);
}

void actualizarDispensa() {
  if (estadoDispensa == ESTADO_INACTIVO) return;

  unsigned long ahora = millis();

  if (ahora - inicioDispensa >= TIMEOUT_GLOBAL_MS) {
    detenerServo("SEGURIDAD: Timeout global alcanzado");
    return;
  }

  switch (estadoDispensa) {
    // --- PRE-REVERSA DE ARRANQUE ---
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

    // --- FASE RAPIDA ---
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

    // --- FASE FINA (PULSOS + ESPERA) ---
    case ESTADO_PULSO:
      // En pulsos largos de recuperacion/arranque vigila en vivo para evitar sobrepasarse
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
            detenerServo("SEGURIDAD: HX711 sin respuesta en fase fina");
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
          Serial.printf("   (Pulso sin avance %d/%d)\n", pulsosSinAvance, MAX_PULSOS_SIN_AVANCE);

          if (pulsosSinAvance >= MAX_PULSOS_SIN_AVANCE) {
            iniciarDesatasco(ESTADO_PULSO, "fase fina");
            return;
          }
        }

        float faltante = pesoObjetivo - pesoAgregado;
        iniciarPulsoFino(calcularDuracionPulso(faltante, pulsosSinAvance), millis());
      }
      break;

    // --- SECUENCIA DE DESATASCO ---
    case ESTADO_DESATASCO_FRENO:
      if (ahora - marcaTiempoFase >= PAUSA_PRE_REVERSA_MS) {
        Serial.println("   <- Girando en reversa a maxima potencia...");
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

        unsigned long instante = millis();
        tiempoUltimoAvance = instante;
        pulsosSinAvance = 0;
        pesoAnteriorRapido = pesoAgregado;

        float faltante = pesoObjetivo - pesoAgregado;
        if (estadoRetornoDesatasco == ESTADO_RAPIDO && faltante > MARGEN_FRENADO_G) {
          iniciarFaseAdelante(faltante, 0, instante);
        } else {
          iniciarFaseAdelante(min(faltante, MARGEN_FRENADO_G), duracionReversaActual, instante);
        }
      }
      break;

    default:
      break;
  }
}

// ==========================================
// 6. GESTIÓN DE HORARIOS Y SIMULACIÓN DE HORA
// ==========================================
void reiniciarEjecucionDiaria() {
  for (int i = 0; i < NUM_HORARIOS; i++) {
    horarios[i].ejecutadoHoy = false;
  }
}

void establecerHoraSimulada(uint8_t hora, uint8_t minuto) {
  interrumpirSiActivo("Cambio manual de hora simulada");
  horaSim = hora;
  minutoSim = minuto;
  segundoSim = 0;
  ultimoTickReloj = millis();

  reiniciarEjecucionDiaria();
  Serial.printf("\n>> Reloj simulado ajustado a %02u:%02u:00\n", horaSim, minutoSim);
}

void cambiarRacionRutina(float gramosMeta) {
  for (int i = 0; i < NUM_HORARIOS; i++) {
    horarios[i].gramosMeta = gramosMeta;
  }
  Serial.printf(">> Meta actualizada a %.1f g para los %d horarios.\n", gramosMeta, NUM_HORARIOS);
}

void mostrarEstadoHorarios() {
  float pesoPlato = balanza.is_ready() ? leerPesoPlato() : 0.0f;
  Serial.printf("\n--- ESTADO ACTUAL [%02u:%02u:%02u] | Plato: %.1f g ---\n",
                horaSim, minutoSim, segundoSim, pesoPlato);
  for (int i = 0; i < NUM_HORARIOS; i++) {
    Serial.printf("  %d) %-15s -> %02u:%02u | Meta: %.1f g | %s\n",
                  i + 1,
                  horarios[i].nombre,
                  horarios[i].hora,
                  horarios[i].minuto,
                  horarios[i].gramosMeta,
                  horarios[i].ejecutadoHoy ? "EJECUTADA" : "PENDIENTE");
  }
  Serial.println();
}

void actualizarRelojSimulado() {
  unsigned long ahora = millis();
  while (ahora - ultimoTickReloj >= 1000) {
    ultimoTickReloj += 1000;
    segundoSim++;
    if (segundoSim >= 60) {
      segundoSim = 0;
      minutoSim++;
      if (minutoSim >= 60) {
        minutoSim = 0;
        horaSim++;
        if (horaSim >= 24) {
          horaSim = 0;
          reiniciarEjecucionDiaria();
          Serial.println(">> Nuevo dia simulado (00:00): horarios reiniciados.");
        }
      }
    }
  }
}

void ejecutarComidaProgramada(HorarioComida& comida) {
  float pesoEnPlato = leerPesoPlato();
  float gramosFaltantes = comida.gramosMeta - pesoEnPlato;

  Serial.printf("\n=== HORARIO ACTIVADO: %s (%02u:%02u) ===\n",
                comida.nombre, comida.hora, comida.minuto);
  Serial.printf("    Meta: %.1f g | Restante en plato: %.1f g\n",
                comida.gramosMeta, pesoEnPlato);

  if (gramosFaltantes <= TOLERANCIA_FINAL_G) {
    Serial.println("    El plato ya cumple con la racion. No se activa el servo.\n");
    return;
  }

  Serial.printf("    Sirviendo diferencia para completar racion: +%.1f g\n", gramosFaltantes);
  iniciarDispensa(gramosFaltantes);
}

void verificarHorarios() {
  if (estadoDispensa != ESTADO_INACTIVO) return;

  for (int i = 0; i < NUM_HORARIOS; i++) {
    if (!horarios[i].ejecutadoHoy &&
        horarios[i].hora == horaSim &&
        horarios[i].minuto == minutoSim) {
      horarios[i].ejecutadoHoy = true;
      ejecutarComidaProgramada(horarios[i]);
      return;
    }
  }
}

// ==========================================
// 7. COMANDOS POR MONITOR SERIE
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

  if (cmd.equalsIgnoreCase("i")) {
    mostrarEstadoHorarios();
    return;
  }

  if (cmd.startsWith("h") || cmd.startsWith("H")) {
    int hh = -1, mm = -1;
    const char* args = cmd.c_str() + 1;
    if (sscanf(args, "%d:%d", &hh, &mm) != 2) {
      sscanf(args, "%d %d", &hh, &mm);
    }
    if (hh >= 0 && hh <= 23 && mm >= 0 && mm <= 59) {
      establecerHoraSimulada((uint8_t)hh, (uint8_t)mm);
    } else {
      Serial.println("Formato invalido. Ejemplos: h 07:00 | h 13:00 | h 19:00");
    }
    return;
  }

  if (cmd.startsWith("p") || cmd.startsWith("P")) {
    int idx = cmd.substring(1).toInt();
    if (idx >= 1 && idx <= NUM_HORARIOS) {
      HorarioComida& h = horarios[idx - 1];
      Serial.printf(">> Saltando a %s (%02u:%02u)...\n", h.nombre, h.hora, h.minuto);
      establecerHoraSimulada(h.hora, h.minuto);
    } else {
      Serial.printf("Horario invalido. Usa: p 1, p 2 o p %d\n", NUM_HORARIOS);
    }
    return;
  }

  if (cmd.startsWith("g") || cmd.startsWith("G")) {
    float nuevaRacion = cmd.substring(1).toFloat();
    if (nuevaRacion > 0.0f) cambiarRacionRutina(nuevaRacion);
    else Serial.println("Formato invalido. Ejemplo: g 30");
    return;
  }

  if (cmd.equalsIgnoreCase("r")) {
    if (estadoDispensa != ESTADO_INACTIVO) {
      Serial.println("Deten la dispensa primero con 's' antes de probar reversa.");
      return;
    }
    Serial.printf(">> Probando pre-reversa (%lu ms a %d us)...\n", DURACION_PRE_REVERSA_MS, SERVO_REV_US);
    arrancarMotor(SERVO_REV_US);
    delay(DURACION_PRE_REVERSA_MS);
    servoDispensador.detach();
    Serial.println(">> Reversa terminada.\n");
    return;
  }

  if (cmd.equalsIgnoreCase("t")) {
    interrumpirSiActivo("Interrumpido para tara");
    Serial.println("Haciendo tara (sin peso en el plato)...");
    balanza.tare(20);
    Serial.println("Cero listo.\n");
    return;
  }

  if (cmd.startsWith("c") || cmd.startsWith("C")) {
    interrumpirSiActivo("Interrumpido para calibracion");
    float pesoConocido = cmd.substring(1).toFloat();
    if (pesoConocido <= 0.0f) {
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
  if (gramos > 0.0f) {
    iniciarDispensa(gramos);
  } else {
    Serial.println("Comando no reconocido. Usa: [h HH:MM], [p 1..3], [g <g>], [i], [t], [c <g>], [<g>], [r] o [s]");
  }
}

// ==========================================
// 8. SETUP Y LOOP
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(300);

  ESP32PWM::allocateTimer(0);
  servoDispensador.setPeriodHertz(50);
  servoDispensador.attach(SERVO_PIN, 300, 2500);
  servoDispensador.detach();

  balanza.begin(DT_PIN, SCK_PIN);
  Serial.println("No pongas peso en la balanza (solo el plato vacio)...");
  delay(2000);
  balanza.set_scale(factorCalibracion);
  balanza.tare(20);

  ultimoTickReloj = millis();

  Serial.println("\n=== DISPENSADOR HX711 + SERVO (3 HORARIOS + PRE-REVERSA) LISTO ===");
  Serial.println("Comandos: [h HH:MM] | [p 1..3] | [g <g>] | [i] | [t] | [c <g>] | [<g>] | [r] | [s]");
  mostrarEstadoHorarios();
}

void loop() {
  actualizarRelojSimulado();
  leerComandosSerie();
  verificarHorarios();
  actualizarDispensa();

  if (estadoDispensa == ESTADO_INACTIVO) {
    if (millis() - ultimaImpresion >= 1000) {
      ultimaImpresion = millis();
      if (balanza.is_ready()) {
        Serial.printf("[%02u:%02u:%02u] Peso actual: %.1f g\n",
                      horaSim, minutoSim, segundoSim, leerPeso(5));
      }
    }
  }
}