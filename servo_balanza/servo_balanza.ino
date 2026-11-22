#include <Arduino.h>
#include <ESP32Servo.h>
#include "HX711.h"

// ==========================================
// 1. PINES Y CONFIGURACIÓN
// ==========================================
#define DT_PIN        16
#define SCK_PIN       17
#define SERVO_PIN     18

#define SERVO_FEED_US  400    // Pulso de giro de tu servo
#define TIMEOUT_MS     15000  // Seguridad: detiene el servo tras 15 s si no alcanza el peso

// Ajustes de la balanza
float factorCalibracion = 467.1;
const float ZONA_MUERTA = 1.5; // Convierte a 0.0 g el ruido y la deriva dentro de -1.5 g a +1.5 g

HX711 balanza;
Servo servoDispensador;

bool dispensando = false;
float pesoInicial = 0.0;
float pesoObjetivo = 0.0;
unsigned long inicioDispensa = 0;
unsigned long ultimaImpresion = 0;

// ==========================================
// 2. LECTURA LIMPIA DE LA BALANZA
// ==========================================
float leerPeso(int muestras) {
  float peso = balanza.get_units(muestras);

  // Solo convierte a 0.0 el ruido/deriva pequeña entre -ZONA_MUERTA y +ZONA_MUERTA
  if (fabs(peso) < ZONA_MUERTA) {
    return 0.0f;
  }

  return peso;
}

// ==========================================
// 3. CONTROL DEL SERVO (POR PESO AGREGADO)
// ==========================================
void iniciarDispensa(float gramosPorServir) {
  if (gramosPorServir <= 0) {
    Serial.println("Ingresa un peso mayor a 0 g.");
    return;
  }

  // Guarda el peso real actual (sin zona muerta) para cancelar cualquier deriva de cero
  pesoInicial = balanza.get_units(5);
  pesoObjetivo = gramosPorServir;
  dispensando = true;
  inicioDispensa = millis();

  servoDispensador.attach(SERVO_PIN, 300, 2400);
  servoDispensador.writeMicroseconds(SERVO_FEED_US);
  Serial.printf(">> DISPENSANDO +%.1f g (base actual: %.1f g)...\n", pesoObjetivo, pesoInicial);
}

void detenerServo(const char* motivo) {
  servoDispensador.detach(); // Corta la señal PWM -> freno inmediato
  dispensando = false;
  Serial.printf(">> DETENIDO: %s\n", motivo);

  delay(400); // Pausa breve para estabilizar el plato tras detener el motor
  float pesoFinalCrudo = balanza.get_units(10);
  float pesoAgregado = pesoFinalCrudo - pesoInicial;

  if (fabs(pesoAgregado) < ZONA_MUERTA) pesoAgregado = 0.0f;
  float pesoTotalLimpio = (fabs(pesoFinalCrudo) < ZONA_MUERTA) ? 0.0f : pesoFinalCrudo;

  Serial.printf("   Agregado real: %.1f g | Total en plato: %.1f g\n\n", pesoAgregado, pesoTotalLimpio);
}

// ==========================================
// 4. COMANDOS POR MONITOR SERIE
// ==========================================
void leerComandosSerie() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd.length() == 0) return;

  // 's' -> Parada de emergencia
  if (cmd.equalsIgnoreCase("s")) {
    if (dispensando) detenerServo("Parada manual por usuario");
    else Serial.println("El servo ya esta detenido.");
    return;
  }

  // 't' -> Hacer tara (cero)
  if (cmd.equalsIgnoreCase("t")) {
    if (dispensando) detenerServo("Interrumpido para tara");
    Serial.println("Haciendo tara (sin peso en el plato)...");
    balanza.tare(20);
    Serial.println("Cero listo.\n");
    return;
  }

  // 'c 100' -> Calibrar colocando un peso conocido (usa lectura cruda sin zona muerta)
  if (cmd.startsWith("c") || cmd.startsWith("C")) {
    if (dispensando) detenerServo("Interrumpido para calibracion");
    float pesoConocido = cmd.substring(1).toFloat();
    if (pesoConocido <= 0) {
      Serial.println("Formato invalido. Ejemplo para calibrar con 100 g: c 100");
      return;
    }
    double lecturaNeta = balanza.get_value(15);
    factorCalibracion = lecturaNeta / pesoConocido;
    balanza.set_scale(factorCalibracion);
    Serial.printf("Calibrado! Tu factorCalibracion = %.3f\n", factorCalibracion);
    Serial.printf("Comprobacion -> Peso actual: %.1f g\n\n", leerPeso(10));
    return;
  }

  // Si es un numero (ej. '50'), dispensa esa cantidad de gramos adicionales
  float gramos = cmd.toFloat();
  if (gramos > 0) {
    iniciarDispensa(gramos);
  } else {
    Serial.println("Comando no reconocido. Usa: [t], [c <gramos>], [<gramos>] o [s]");
  }
}

// ==========================================
// 5. SETUP Y LOOP
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

  Serial.println("\n=== PRUEBA HX711 + SERVO LISTA ===");
  Serial.println("Comandos en Monitor Serie (115200 baud):");
  Serial.println("  t         -> Poner en cero (tara)");
  Serial.println("  c 100     -> Calibrar (pon primero un objeto de 100 g)");
  Serial.println("  50        -> Dispensar 50 g adicionales");
  Serial.println("  s         -> Parada de emergencia del servo\n");
}

void loop() {
  leerComandosSerie();

  if (dispensando) {
    // Usar solo 2 muestras mientras dispensa (~200 ms) para frenar a tiempo
    if (balanza.is_ready()) {
      float pesoAgregado = balanza.get_units(2) - pesoInicial;
      float pesoMostrar = (fabs(pesoAgregado) < ZONA_MUERTA) ? 0.0f : pesoAgregado;

      Serial.printf("Sirviendo... +%.1f / %.1f g\n", pesoMostrar, pesoObjetivo);

      if (pesoAgregado >= pesoObjetivo) {
        detenerServo("Peso objetivo alcanzado");
      }
    }

    // Proteccion contra tolva vacia o atasco
    if (dispensando && (millis() - inicioDispensa >= TIMEOUT_MS)) {
      detenerServo("Timeout de seguridad (posible tolva vacia o atasco)");
    }
  } else {
    // En reposo: mostrar peso cada 1 segundo
    if (millis() - ultimaImpresion >= 1000) {
      ultimaImpresion = millis();
      if (balanza.is_ready()) {
        float peso = leerPeso(5);
        Serial.printf("Peso actual: %.1f g\n", peso);
      }
    }
  }
}