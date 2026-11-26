#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include "HX711.h"

// ==========================================
// 1. DEFINICIÓN DE PINES Y PARÁMETROS HARDWARE
// ==========================================

// --- Balanza HX711 ---
#define DT_PIN                16    // Pin de datos (DOUT) del modulo HX711
#define SCK_PIN               17    // Pin de reloj (PD_SCK) del modulo HX711

// --- Servo Dispensador (360° rotacion continua) ---
// NOTA: Movido de GPIO 18 a GPIO 25 porque GPIO 18 es el pin SPI SCK del MFRC522
#define SERVO_DISPENSADOR_PIN 25    // Pin PWM del servo que dosifica croquetas
#define SERVO_FEED_US         400   // Pulso PWM (us) de maxima velocidad hacia adelante
#define SERVO_REV_US          2500  // Pulso PWM (us) de maxima velocidad en reversa (bajar a 2400 si no gira con 'r')

// --- Lector RFID / NFC (MFRC522 por SPI: SCK=18, MISO=19, MOSI=23) ---
#define RFID_SS_PIN           5     // Pin Slave Select (SDA/SS) del modulo MFRC522
#define RFID_RST_PIN          22    // Pin Reset (RST) del modulo MFRC522

// --- Servos de Compuertas (SG92R derecha e izquierda) ---
#define SERVO_TAPA_R_PIN      13    // Pin PWM del servo de la compuerta derecha
#define SERVO_TAPA_L_PIN      14    // Pin PWM del servo de la compuerta izquierda

// ==========================================
// 2. CONSTANTES DE CALIBRACIÓN, SEGURIDAD Y TIEMPOS
// ==========================================

// --- Angulos y temporizacion de compuertas ---
const int ANGULO_CERRADO_R                     = 90;    // Angulo (grados) cerrado de la compuerta derecha
const int ANGULO_ABIERTO_R                     = 10;    // Angulo (grados) abierto de la compuerta derecha
const int ANGULO_CERRADO_L                     = 10;    // Angulo (grados) cerrado de la compuerta izquierda
const int ANGULO_ABIERTO_L                     = 100;   // Angulo (grados) abierto de la compuerta izquierda
const unsigned long MARGEN_APERTURA_COMPUERTA  = 1000;  // Margen (ms) tras abrir compuertas antes de servir comida
const unsigned long TIEMPO_CIERRE_MS           = 1500;  // Tiempo (ms) sin detectar tag para cerrar o liberar bloqueo
const unsigned long INTERVALO_LECTURA_RFID_MS  = 100;   // Periodo (ms) de consulta no bloqueante al lector NFC

// --- UID del tag autorizado (REEMPLAZA estos 4 bytes por el UID real de tu mascota) ---
const byte UID_AUTORIZADO_LEN                  = 4;     // Longitud en bytes del UID autorizado
const byte UID_AUTORIZADO[UID_AUTORIZADO_LEN] = { 0x21, 0x12, 0xD5, 0x1D };

// --- Protecciones de seguridad y deteccion de atasco ---
const unsigned long TIMEOUT_GLOBAL_MS          = 60000; // Tiempo maximo total permitido para una dispensa (60 s)
const unsigned long TIMEOUT_ARRANQUE_MS        = 2200;  // Tiempo sin flujo inicial antes de disparar desatasco
const unsigned long TIMEOUT_SIN_FLUJO_MS       = 2200;  // Tiempo sin flujo durante dispensa antes de desatasco
const float MIN_AVANCE_RAPIDO_G                = 1.0;   // Incremento minimo (g) para considerar flujo en fase rapida
const float MIN_AVANCE_FINO_G                  = 0.3;   // Incremento minimo (g) esperado por pulso en fase fina
const float AVANCE_RESET_DESATASCO_G           = 4.0;   // Gramos de flujo continuo para reiniciar intentos de desatasco
const int   MAX_PULSOS_SIN_AVANCE              = 3;     // Pulsos finos fallidos seguidos antes de intentar desatasco
const int   MAX_INTENTOS_DESATASCO             = 3;     // Reintentos maximos de reversa antes de parada de seguridad

// --- Tiempos de pre-reversa y desatasco progresivo ---
const unsigned long DURACION_PRE_REVERSA_MS    = 550;   // Retroceso inicial (ms) al comenzar cada dispensa
const unsigned long PAUSA_PRE_ARRANQUE_MS      = 250;   // Pausa muerta (ms) entre pre-reversa y giro adelante
const unsigned long PAUSA_PRE_REVERSA_MS       = 800;   // Pausa muerta (ms) antes de invertir giro en un atasco
const unsigned long REVERSA_BASE_MS            = 750;   // Duracion base (ms) del 1er desatasco (750 -> 950 -> 1150 ms)
const unsigned long INCREMENTO_REVERSA_MS      = 200;   // Tiempo extra (ms) sumado en cada reintento de desatasco
const unsigned long EXTRA_EMPUJE_RECUP_MS      = 250;   // Empuje adicional (ms) adelante tras retroceder en fase fina
const unsigned long MAX_EMPUJE_RECUP_MS        = 1200;  // Limite maximo (ms) del pulso de recuperacion
const unsigned long PAUSA_POST_REVERSA_MS      = 350;   // Tiempo de asentamiento (ms) tras finalizar una reversa

// --- Balanza y umbrales de dosificacion ---
float factorCalibracion                        = 467.1; // Factor de escala calibrado de la celda de carga de 5 kg
const float ZONA_MUERTA_PLATO                  = 1.5;   // Margen (g) para ocultar ruido/deriva del plato en reposo
const float ZONA_MUERTA_AVANCE                 = 0.4;   // Margen (g) mas fino para reportar avance durante dispensa
const float MARGEN_FRENADO_G                   = 12.5;  // Gramos antes de la meta donde frena la fase rapida
const float TOLERANCIA_FINAL_G                 = 0.8;   // Margen (g) respecto a la meta para dar la racion por lista

// --- Pulsos adaptativos en fase fina ---
const unsigned long PULSO_MUY_LARGO_MS         = 650;   // Duracion de pulso (ms) cuando faltan > 7.0 g
const unsigned long PULSO_LARGO_MS             = 480;   // Duracion de pulso (ms) cuando faltan > 3.5 g
const unsigned long PULSO_MEDIO_MS             = 350;   // Duracion de pulso (ms) cuando faltan > 1.5 g
const unsigned long PULSO_CORTO_MS             = 240;   // Duracion de pulso (ms) cuando faltan <= 1.5 g
const unsigned long EXTRA_POR_FALLO_MS         = 120;   // Tiempo extra (ms) sumado tras cada pulso fino sin avance
const unsigned long PULSO_MAX_MS               = 850;   // Duracion maxima permitida para un pulso fino normal
const unsigned long PAUSA_ASENTAMIENTO_MS      = 650;   // Espera (ms) entre pulsos finos para leer peso estable

// ==========================================
// 3. ESTRUCTURAS, OBJETOS Y VARIABLES DE ESTADO
// ==========================================

// Representa una comida programada dentro de la rutina diaria
struct HorarioComida {
  const char* nombre;  // Etiqueta descriptiva del horario
  uint8_t hora;        // Hora de activacion en formato 24h (0 - 23)
  uint8_t minuto;      // Minuto de activacion (0 - 59)
  float gramosMeta;    // Peso total objetivo (g) que debe haber en el plato
  bool ejecutadoHoy;   // Bandera que evita disparar el mismo horario dos veces el mismo dia
};

const int NUM_HORARIOS = 3; // Cantidad total de comidas configuradas por dia
HorarioComida horarios[NUM_HORARIOS] = {
  { "Primera comida",  7, 0, 30.0, false }, // 07:00 (7:00 am)
  { "Segunda comida", 13, 0, 30.0, false }, // 13:00 (1:00 pm)
  { "Tercera comida", 19, 0, 30.0, false }  // 19:00 (7:00 pm)
};

// Variables del reloj interno simulado
uint8_t horaSim              = 6;  // Hora actual simulada (0 - 23)
uint8_t minutoSim            = 55; // Minuto actual simulado (0 - 59)
uint8_t segundoSim           = 0;  // Segundo actual simulado (0 - 59)
unsigned long ultimoTickReloj = 0; // Marca de millis() del ultimo segundo contado

// Instancias de perifericos
HX711 balanza;                     // Controlador del amplificador de celda de carga HX711
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN); // Controlador del lector RFID/NFC MFRC522
Servo servoDispensador;            // Servo continuo encargado de empujar el alimento
Servo servoTapaR;                  // Servo SG92R de la compuerta derecha
Servo servoTapaL;                  // Servo SG92R de la compuerta izquierda

// Estados de la maquina de estados no bloqueante de dosificacion
enum EstadoDispensa {
  ESTADO_INACTIVO,           // Dispensador en reposo
  ESTADO_ABRIENDO_COMPUERTA, // Esperando margen de tiempo mientras se abren las compuertas
  ESTADO_PRE_REVERSA,        // Ejecutando retroceso inicial para destrabar el rotor
  ESTADO_PRE_PAUSA,          // Pausa muerta tras la pre-reversa antes de avanzar
  ESTADO_RAPIDO,             // Giro continuo rapido hasta el margen de frenado
  ESTADO_PULSO,              // Pulso corto hacia adelante en fase fina
  ESTADO_ESPERA,             // Espera de caida en vuelo / asentamiento para medir peso
  ESTADO_DESATASCO_FRENO,    // Pausa muerta previa a la reversa de desatasco
  ESTADO_DESATASCO_REV,      // Giro en reversa para liberar croquetas trabadas
  ESTADO_DESATASCO_PAUSA     // Pausa posterior al desatasco antes de reanudar
};

EstadoDispensa estadoDispensa         = ESTADO_INACTIVO; // Estado actual del ciclo de dispensa
EstadoDispensa estadoRetornoDesatasco = ESTADO_INACTIVO; // Fase a la que se vuelve tras un desatasco

// Variables de control de peso y flujo
float pesoInicial         = 0.0; // Lectura cruda base (g) tomada al iniciar el movimiento del dispensador
float pesoObjetivo        = 0.0; // Gramos adicionales que faltan servir en la dispensa activa
float ultimoPesoAvance    = 0.0; // Mayor avance valido (g) registrado en la dispensa actual
float pesoAnteriorRapido  = 0.0; // Lectura anterior (g) en fase rapida para estimar velocidad de caida
float pesoUltimoDesatasco = 0.0; // Avance registrado (g) al momento del ultimo desatasco

// Temporizadores y contadores del ciclo de dispensa
unsigned long inicioDispensa        = 0;               // Marca millis() de inicio de la dispensa (timeout global)
unsigned long tiempoUltimoAvance    = 0;               // Marca millis() del ultimo incremento valido de peso
unsigned long marcaTiempoFase       = 0;               // Marca millis() de inicio del estado actual
unsigned long duracionPulsoActual   = PULSO_MEDIO_MS;  // Duracion (ms) asignada al pulso fino en curso
unsigned long duracionReversaActual = REVERSA_BASE_MS; // Duracion (ms) de la reversa de desatasco en curso
unsigned long ultimaImpresion       = 0;               // Marca millis() del ultimo reporte de peso en reposo

int  pulsosSinAvance    = 0;     // Contador de pulsos finos seguidos sin incremento de peso
int  intentosDesatasco  = 0;     // Contador de desatascos consecutivos ejecutados
int  lecturasCortePulso = 0;     // Confirmaciones consecutivas de meta en pulsos largos de recuperacion
bool veniaDePulsoFino   = false; // Indica si la espera actual proviene de un pulso fino o de fase rapida

// Variables de control de compuertas y lector RFID/NFC
bool tapaAbierta                         = false; // Estado fisico actual de las compuertas (true = abiertas)
bool tagAutorizadoPresente               = false; // True mientras el tag valido este dentro de la ventana activa
bool tagNoAutorizadoPresente             = false; // True mientras un tag invalido este presente (bloquea dispensa)
unsigned long ultimaLecturaRFID          = 0;     // Marca millis() de la ultima consulta al MFRC522
unsigned long ultimaDeteccionAutorizada  = 0;     // Marca millis() de la ultima lectura del tag valido
unsigned long ultimaDeteccionNoAutorizada = 0;    // Marca millis() de la ultima lectura de un tag invalido

// ==========================================
// 4. FUNCIONES DE COMPUERTAS Y RFID / NFC
// ==========================================

// Mueve ambos servos de compuerta a su posicion de apertura y actualiza el estado
void abrirCompuertas() {
  if (tapaAbierta) return;
  servoTapaR.write(ANGULO_ABIERTO_R);
  servoTapaL.write(ANGULO_ABIERTO_L);
  tapaAbierta = true;
  Serial.println(">> Compuertas ABIERTAS (R y L).");
}

// Mueve ambos servos de compuerta a su posicion de cierre y actualiza el estado
void cerrarCompuertas() {
  if (!tapaAbierta) return;
  servoTapaR.write(ANGULO_CERRADO_R);
  servoTapaL.write(ANGULO_CERRADO_L);
  tapaAbierta = false;
  Serial.println(">> Compuertas CERRADAS (R y L).");
}

// Fuerza el cierre de las compuertas al arrancar el sistema sin depender de la bandera previa
void inicializarCompuertasCerradas() {
  servoTapaR.write(ANGULO_CERRADO_R);
  servoTapaL.write(ANGULO_CERRADO_L);
  tapaAbierta = false;
}

// Despierta tarjetas nuevas o en estado HALT sobre la antena y lee su UID
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

// Compara el UID leido por el MFRC522 contra el arreglo constante UID_AUTORIZADO
bool esTagAutorizado() {
  if (rfid.uid.size != UID_AUTORIZADO_LEN) return false;
  for (byte i = 0; i < UID_AUTORIZADO_LEN; i++) {
    if (rfid.uid.uidByte[i] != UID_AUTORIZADO[i]) return false;
  }
  return true;
}

// Imprime en el Monitor Serie el UID hexadecimal de la tarjeta detectada
void imprimirUID() {
  Serial.print("UID:");
  for (byte i = 0; i < rfid.uid.size; i++) {
    Serial.print(rfid.uid.uidByte[i] < 0x10 ? " 0" : " ");
    Serial.print(rfid.uid.uidByte[i], HEX);
  }
  Serial.println();
}

// Prototipo adelantado para que el modulo RFID pueda abortar una dispensa ante un tag no autorizado
void detenerServo(const char* motivo, bool cerrarInmediato = false);

// Sincroniza apertura y cierre de compuertas segun presencia de tags NFC y estado de dispensa
void actualizarCompuertas(unsigned long ahora) {
  if (tagAutorizadoPresente && (ahora - ultimaDeteccionAutorizada >= TIEMPO_CIERRE_MS)) {
    tagAutorizadoPresente = false;
    Serial.println(">> Tag autorizado alejado.");
  }

  if (tagNoAutorizadoPresente && (ahora - ultimaDeteccionNoAutorizada >= TIEMPO_CIERRE_MS)) {
    tagNoAutorizadoPresente = false;
    Serial.println(">> Tag no autorizado alejado. Bloqueo liberado.");
  }

  // Las compuertas solo deben estar abiertas si no hay intruso y (esta el tag valido o se esta sirviendo)
  bool debeEstarAbierta = !tagNoAutorizadoPresente &&
                          (tagAutorizadoPresente || estadoDispensa != ESTADO_INACTIVO);

  if (debeEstarAbierta && !tapaAbierta) {
    abrirCompuertas();
  } else if (!debeEstarAbierta && tapaAbierta) {
    cerrarCompuertas();
  }
}

// Consulta periodicamente el lector NFC, valida el tag y aplica apertura o bloqueo de seguridad
void actualizarRFID() {
  unsigned long ahora = millis();

  if (ahora - ultimaLecturaRFID >= INTERVALO_LECTURA_RFID_MS) {
    ultimaLecturaRFID = ahora;

    if (detectarTarjeta()) {
      if (esTagAutorizado()) {
        ultimaDeteccionAutorizada = ahora;
        if (!tagAutorizadoPresente) {
          imprimirUID();
          Serial.println(">> NFC AUTORIZADO -> Acceso permitido.");
        }
        tagAutorizadoPresente = true;
        tagNoAutorizadoPresente = false;
      } else {
        ultimaDeteccionNoAutorizada = ahora;
        if (!tagNoAutorizadoPresente) {
          imprimirUID();
          Serial.println("!! NFC NO AUTORIZADO -> Compuertas y dispensa BLOQUEADAS.");
        }
        tagNoAutorizadoPresente = true;
        tagAutorizadoPresente = false;

        // Si estaba dispensando cuando se acerco un tag invalido, corta de inmediato y cierra tapa
        if (estadoDispensa != ESTADO_INACTIVO) {
          detenerServo("SEGURIDAD: Tag NFC no autorizado detectado", true);
        }
      }

      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
    }
  }

  actualizarCompuertas(ahora);
}

// ==========================================
// 5. FUNCIONES AUXILIARES DE BALANZA Y MOTOR
// ==========================================

// Convierte a 0.0 g cualquier valor dentro de [-umbral, +umbral] para suprimir ruido
float aplicarZonaMuerta(float valor, float umbral = ZONA_MUERTA_PLATO) {
  return (fabs(valor) < umbral) ? 0.0f : valor;
}

// Obtiene un promedio de 'muestras' lecturas del HX711 filtrado con zona muerta de plato
float leerPeso(int muestras) {
  return aplicarZonaMuerta(balanza.get_units(muestras), ZONA_MUERTA_PLATO);
}

// Toma 3 lecturas individuales y retorna la mediana para eliminar picos aislados
float leerCrudoMediana3() {
  float a = balanza.get_units(1);
  float b = balanza.get_units(1);
  float c = balanza.get_units(1);

  if (a > b) { float tmp = a; a = b; b = tmp; }
  if (b > c) { float tmp = b; b = c; c = tmp; }
  if (a > b) { float tmp = a; a = b; b = tmp; }
  return b;
}

// Devuelve el peso disponible en el plato sin valores negativos (para calcular sobrantes)
float leerPesoPlato() {
  float peso = aplicarZonaMuerta(leerCrudoMediana3(), ZONA_MUERTA_PLATO);
  return (peso > 0.0f) ? peso : 0.0f;
}

// Activa el servo dispensador enviandole el pulso PWM indicado en microsegundos
void arrancarMotorDispensador(int pulsoUs) {
  servoDispensador.attach(SERVO_DISPENSADOR_PIN, 300, 2500);
  servoDispensador.writeMicroseconds(pulsoUs);
}

// Calcula la duracion del siguiente pulso fino segun gramos faltantes y fallos acumulados
unsigned long calcularDuracionPulso(float faltante, int sinAvance) {
  unsigned long base;
  if (faltante > 7.0f)      base = PULSO_MUY_LARGO_MS;
  else if (faltante > 3.5f) base = PULSO_LARGO_MS;
  else if (faltante > 1.5f) base = PULSO_MEDIO_MS;
  else                      base = PULSO_CORTO_MS;

  unsigned long total = base + (unsigned long)sinAvance * EXTRA_POR_FALLO_MS;
  return (total > PULSO_MAX_MS) ? PULSO_MAX_MS : total;
}

// Calcula el pulso de avance necesario para compensar una reversa previa y vencer el atasco
unsigned long calcularEmpujeRecuperacion(unsigned long reversaMs) {
  unsigned long empuje = reversaMs + EXTRA_EMPUJE_RECUP_MS;
  return (empuje > MAX_EMPUJE_RECUP_MS) ? MAX_EMPUJE_RECUP_MS : empuje;
}

// Detiene el motor dispensador y pasa al estado de espera de caida/asentamiento
void pasarAEspera(bool desdePulso, unsigned long ahora) {
  servoDispensador.detach();
  estadoDispensa = ESTADO_ESPERA;
  veniaDePulsoFino = desdePulso;
  marcaTiempoFase = ahora;
  lecturasCortePulso = 0;
}

// Inicia un pulso fino cronometrado hacia adelante
void iniciarPulsoFino(unsigned long duracionMs, unsigned long ahora) {
  duracionPulsoActual = duracionMs;
  estadoDispensa = ESTADO_PULSO;
  veniaDePulsoFino = true;
  marcaTiempoFase = ahora;
  lecturasCortePulso = 0;
  arrancarMotorDispensador(SERVO_FEED_US);
}

// Toma la referencia de peso con compuertas ya abiertas y dispara la pre-reversa de arranque
void iniciarPreReversa(unsigned long ahora) {
  pesoInicial = leerCrudoMediana3();
  ultimoPesoAvance = 0.0;
  pesoAnteriorRapido = 0.0;
  pesoUltimoDesatasco = 0.0;
  tiempoUltimoAvance = ahora;
  marcaTiempoFase = ahora;

  estadoDispensa = ESTADO_PRE_REVERSA;
  arrancarMotorDispensador(SERVO_REV_US);
  Serial.printf("   -> Compuertas listas. Pre-reversa de arranque (%lu ms a %d us)...\n",
                DURACION_PRE_REVERSA_MS, SERVO_REV_US);
}

// Decide si avanzar en fase rapida o en pulsos finos segun los gramos faltantes
void iniciarFaseAdelante(float faltante, unsigned long reversaPreviaMs, unsigned long ahora) {
  if (faltante > MARGEN_FRENADO_G) {
    estadoDispensa = ESTADO_RAPIDO;
    veniaDePulsoFino = false;
    arrancarMotorDispensador(SERVO_FEED_US);
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

// Registra un incremento valido de peso y reinicia el contador de desatascos si hay flujo sostenido
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
// 6. CONTROL DE DISPENSA Y DESATASCO
// ==========================================

// Detiene el dispensador, reporta el peso final estabilizado y cierra compuertas si corresponde
void detenerServo(const char* motivo, bool cerrarInmediato) {
  servoDispensador.detach();
  estadoDispensa = ESTADO_INACTIVO;
  Serial.printf(">> DETENIDO: %s\n", motivo);

  // Si fue una parada de seguridad por tag no autorizado, cierra antes de estabilizar peso
  if (cerrarInmediato) {
    cerrarCompuertas();
  }

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

  // Si no hay un tag autorizado manteniendo abiertas las compuertas, las cierra al finalizar
  if (!tagAutorizadoPresente) {
    cerrarCompuertas();
  }
}

// Interrumpe una dispensa en curso de forma segura antes de ejecutar comandos manuales
void interrumpirSiActivo(const char* motivo) {
  if (estadoDispensa != ESTADO_INACTIVO) {
    detenerServo(motivo);
  }
}

// Ejecuta una secuencia de reversa progresiva ante falta de flujo o corta por seguridad
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

// Inicia una dispensa verificando que no exista un tag no autorizado y abriendo compuertas con margen
void iniciarDispensa(float gramosPorServir) {
  if (gramosPorServir <= 0.0f) {
    Serial.println("Ingresa un peso mayor a 0 g.");
    return;
  }

  if (tagNoAutorizadoPresente) {
    Serial.println("!! DISPENSA BLOQUEADA: Tag NFC no autorizado presente.\n");
    cerrarCompuertas();
    return;
  }

  pesoObjetivo = gramosPorServir;
  pulsosSinAvance = 0;
  intentosDesatasco = 0;
  lecturasCortePulso = 0;
  veniaDePulsoFino = false;

  unsigned long ahora = millis();
  inicioDispensa = ahora;
  tiempoUltimoAvance = ahora;
  marcaTiempoFase = ahora;

  Serial.printf(">> DISPENSANDO +%.1f g...\n", pesoObjetivo);

  if (!tapaAbierta) {
    // Abre compuertas y espera el margen antes de accionar el motor dispensador
    abrirCompuertas();
    estadoDispensa = ESTADO_ABRIENDO_COMPUERTA;
    Serial.printf("   -> Esperando margen de apertura de compuertas (%lu ms)...\n",
                  MARGEN_APERTURA_COMPUERTA);
  } else {
    // Si las compuertas ya estaban abiertas por el tag valido, arranca directo
    iniciarPreReversa(ahora);
  }
}

// Avanza la maquina de estados de dosificacion sin bloquear el bucle principal
void actualizarDispensa() {
  if (estadoDispensa == ESTADO_INACTIVO) return;

  unsigned long ahora = millis();

  if (ahora - inicioDispensa >= TIMEOUT_GLOBAL_MS) {
    detenerServo("SEGURIDAD: Timeout global alcanzado");
    return;
  }

  switch (estadoDispensa) {
    // --- MARGEN DE APERTURA DE COMPUERTAS ---
    case ESTADO_ABRIENDO_COMPUERTA:
      if (ahora - marcaTiempoFase >= MARGEN_APERTURA_COMPUERTA) {
        if (!balanza.is_ready()) return;
        iniciarPreReversa(millis());
      }
      break;

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
// 7. GESTIÓN DE HORARIOS Y RELOJ SIMULADO
// ==========================================

// Marca todos los horarios del dia como pendientes (usado al cambiar de dia o simular hora)
void reiniciarEjecucionDiaria() {
  for (int i = 0; i < NUM_HORARIOS; i++) {
    horarios[i].ejecutadoHoy = false;
  }
}

// Fija una hora y minuto simulados para probar cualquier horario de la rutina
void establecerHoraSimulada(uint8_t hora, uint8_t minuto) {
  interrumpirSiActivo("Cambio manual de hora simulada");
  horaSim = hora;
  minutoSim = minuto;
  segundoSim = 0;
  ultimoTickReloj = millis();

  reiniciarEjecucionDiaria();
  Serial.printf("\n>> Reloj simulado ajustado a %02u:%02u:00\n", horaSim, minutoSim);
}

// Actualiza la meta de gramos para las 3 comidas de la rutina
void cambiarRacionRutina(float gramosMeta) {
  for (int i = 0; i < NUM_HORARIOS; i++) {
    horarios[i].gramosMeta = gramosMeta;
  }
  Serial.printf(">> Meta actualizada a %.1f g para los %d horarios.\n", gramosMeta, NUM_HORARIOS);
}

// Muestra por Monitor Serie la hora simulada, peso actual, estado de compuertas y los 3 horarios
void mostrarEstadoHorarios() {
  float pesoPlato = balanza.is_ready() ? leerPesoPlato() : 0.0f;
  Serial.printf("\n--- ESTADO ACTUAL [%02u:%02u:%02u] | Plato: %.1f g | Compuertas: %s ---\n",
                horaSim, minutoSim, segundoSim, pesoPlato, tapaAbierta ? "ABIERTAS" : "CERRADAS");
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

// Incrementa el reloj simulado segundo a segundo usando millis() sin bloqueos
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

// Evalua seguridad NFC y sobrante en el plato antes de disparar una comida programada
void ejecutarComidaProgramada(HorarioComida& comida) {
  Serial.printf("\n=== HORARIO ACTIVADO: %s (%02u:%02u) ===\n",
                comida.nombre, comida.hora, comida.minuto);

  if (tagNoAutorizadoPresente) {
    Serial.println("    !! BLOQUEADO: Tag NFC no autorizado presente. No se sirve la comida.\n");
    cerrarCompuertas();
    return;
  }

  float pesoEnPlato = leerPesoPlato();
  float gramosFaltantes = comida.gramosMeta - pesoEnPlato;

  Serial.printf("    Meta: %.1f g | Restante en plato: %.1f g\n",
                comida.gramosMeta, pesoEnPlato);

  if (gramosFaltantes <= TOLERANCIA_FINAL_G) {
    Serial.println("    El plato ya cumple con la racion. No se activan compuertas ni dispensador.\n");
    return;
  }

  Serial.printf("    Sirviendo diferencia para completar racion: +%.1f g\n", gramosFaltantes);
  iniciarDispensa(gramosFaltantes);
}

// Revisa los 3 horarios configurados y ejecuta el que coincida con la hora simulada actual
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
// 8. INTERFAZ DE COMANDOS POR MONITOR SERIE
// ==========================================

// Lee y procesa los comandos de prueba y configuracion enviados por Serial
void leerComandosSerie() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd.length() == 0) return;

  if (cmd.equalsIgnoreCase("s")) {
    if (estadoDispensa != ESTADO_INACTIVO) detenerServo("Parada manual por usuario");
    else Serial.println("El dispensador ya esta detenido.");
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
    arrancarMotorDispensador(SERVO_REV_US);
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
// 9. SETUP Y LOOP PRINCIPAL
// ==========================================

// Inicializa Serial, SPI/MFRC522, los 3 servos y la tara de la balanza HX711
void setup() {
  Serial.begin(115200);
  delay(300);

  // Inicializar bus SPI y lector NFC MFRC522 (SCK=18, MISO=19, MOSI=23)
  SPI.begin();
  rfid.PCD_Init();

  // Asignar 3 timers PWM dedicados para operar los 3 servos sin conflictos
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);

  // Configurar servo dispensador detenido al arranque
  servoDispensador.setPeriodHertz(50);
  servoDispensador.attach(SERVO_DISPENSADOR_PIN, 300, 2500);
  servoDispensador.detach();

  // Configurar servos de compuertas (SG92R a 50 Hz, 500-2400 us) en posicion cerrada
  servoTapaR.setPeriodHertz(50);
  servoTapaL.setPeriodHertz(50);
  servoTapaR.attach(SERVO_TAPA_R_PIN, 500, 2400);
  servoTapaL.attach(SERVO_TAPA_L_PIN, 500, 2400);
  inicializarCompuertasCerradas();

  // Inicializar balanza HX711 y hacer tara inicial
  balanza.begin(DT_PIN, SCK_PIN);
  Serial.println("No pongas peso en la balanza (solo el plato vacio)...");
  delay(2000);
  balanza.set_scale(factorCalibracion);
  balanza.tare(20);

  ultimoTickReloj = millis();

  Serial.println("\n=== DISPENSADOR INTEGRADO (HX711 + 3 SERVOS + RFID + HORARIOS) LISTO ===");
  Serial.println("Comandos: [h HH:MM] | [p 1..3] | [g <g>] | [i] | [t] | [c <g>] | [<g>] | [r] | [s]");
  mostrarEstadoHorarios();
}

// Bucle principal no bloqueante: actualiza reloj, RFID, comandos, horarios, dispensa y telemetria
void loop() {
  actualizarRelojSimulado();
  actualizarRFID();
  leerComandosSerie();
  verificarHorarios();
  actualizarDispensa();

  if (estadoDispensa == ESTADO_INACTIVO) {
    if (millis() - ultimaImpresion >= 1000) {
      ultimaImpresion = millis();
      if (balanza.is_ready()) {
        Serial.printf("[%02u:%02u:%02u] Peso: %.1f g | Compuertas: %s\n",
                      horaSim, minutoSim, segundoSim,
                      leerPeso(5),
                      tapaAbierta ? "ABIERTAS" : "CERRADAS");
      }
    }
  }
}