#include <ESP32Servo.h>

#define SERVO_PIN 14 // 14L 13R

Servo servoPrueba;

void probarMovilidadServo(Servo &servo, int anguloMin, int anguloMax, int paso, int pausaMs) {
  anguloMin = constrain(anguloMin, 0, 180);
  anguloMax = constrain(anguloMax, 0, 180);

  Serial.println("--- Barrido ascendente ---");
  for (int angulo = anguloMin; angulo <= anguloMax; angulo += paso) {
    servo.write(angulo);
    Serial.print("Angulo actual: ");
    Serial.print(angulo);
    Serial.println("°");
    delay(pausaMs);
  }

  delay(1000);

  Serial.println("--- Barrido descendente ---");
  for (int angulo = anguloMax; angulo >= anguloMin; angulo -= paso) {
    servo.write(angulo);
    Serial.print("Angulo actual: ");
    Serial.print(angulo);
    Serial.println("°");
    delay(pausaMs);
  }

  delay(1000);
}

//Posicionar el servo
void probarPorMonitorSerie(Servo &servo) {
  if (Serial.available() > 0) {
    int angulo = Serial.parseInt();

    // Limpiar saltos de línea pendientes en el buffer
    while (Serial.available() > 0) Serial.read();

    if (angulo >= 0 && angulo <= 180) {
      servo.write(angulo);
      Serial.print("Movido manualmente a: ");
      Serial.print(angulo);
      Serial.println("°");
    } else {
      Serial.println("Ingresa un valor entre 0 y 180.");
    }
  }
}

void setup() {
  Serial.begin(115200);

  ESP32PWM::allocateTimer(0);
  servoPrueba.setPeriodHertz(50);
  servoPrueba.attach(SERVO_PIN, 500, 2400);

  // Posición segura inicial antes de iniciar pruebas
  servoPrueba.write(90);
  delay(1000);

  // Ejecuta un barrido automático de 10° a 170° de 10 en 10 cada 500 ms
  probarMovilidadServo(servoPrueba, 10, 170, 10, 500);

  Serial.println("Barrido terminado. Escribe un angulo (0-180) en el Monitor Serie para ajustar fino:");
}

void loop() {
  // Modo calibración manual por Monitor Serie (115200 baud)
  probarPorMonitorSerie(servoPrueba);
}