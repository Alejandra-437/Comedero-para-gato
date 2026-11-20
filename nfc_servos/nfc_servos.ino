#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>

#define SS_PIN      5
#define RST_PIN     22
#define SERVO_PIN_R 13
#define SERVO_PIN_L 14   // Pin PWM 

// Ajustes de ángulos 
const int ANGULO_CERRADO_R = 90;
const int ANGULO_ABIERTO_R = 10;

const int ANGULO_CERRADO_L = 10;
const int ANGULO_ABIERTO_L = 100;

const unsigned long TIEMPO_CIERRE_MS = 1500; // Espera 1.5 s sin detectar el tag antes de cerrar
const unsigned long INTERVALO_LECTURA_MS = 100;

MFRC522 rfid(SS_PIN, RST_PIN);
Servo servoTapaR;
Servo servoTapaL;

bool tapaAbierta = false;
unsigned long ultimaDeteccion = 0;
unsigned long ultimaLectura = 0;

void abrirCompuertas() {
  servoTapaR.write(ANGULO_ABIERTO_R);
  servoTapaL.write(ANGULO_ABIERTO_L);
}

void cerrarCompuertas() {
  servoTapaR.write(ANGULO_CERRADO_R);
  servoTapaL.write(ANGULO_CERRADO_L);
}

bool detectarTarjeta() {
  byte bufferATQA[2];
  byte bufferSize = sizeof(bufferATQA);

  // Reiniciar registros de modulación para lecturas estables
  rfid.PCD_WriteRegister(MFRC522::TxModeReg, 0x00);
  rfid.PCD_WriteRegister(MFRC522::RxModeReg, 0x00);
  rfid.PCD_WriteRegister(MFRC522::ModWidthReg, 0x26);

  // WUPA despierta tarjetas nuevas y también las que siguen sobre el lector en estado HALT
  MFRC522::StatusCode status = rfid.PICC_WakeupA(bufferATQA, &bufferSize);
  if (status != MFRC522::STATUS_OK && status != MFRC522::STATUS_COLLISION) {
    return false;
  }

  return rfid.PICC_ReadCardSerial();
}

void imprimirUID() {
  Serial.print("UID:");
  for (byte i = 0; i < rfid.uid.size; i++) {
    Serial.print(rfid.uid.uidByte[i] < 0x10 ? " 0" : " ");
    Serial.print(rfid.uid.uidByte[i], HEX);
  }
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  SPI.begin();            // ESP32 por defecto: SCK=18, MISO=19, MOSI=23
  rfid.PCD_Init();

  // Asignar timers PWM dedicados para manejar ambos servos sin conflictos
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);

  servoTapaR.setPeriodHertz(50);
  servoTapaL.setPeriodHertz(50);

  // El SG92R opera a 50 Hz con pulsos de 500 a 2400 us
  servoTapaR.attach(SERVO_PIN_R, 500, 2400);
  servoTapaL.attach(SERVO_PIN_L, 500, 2400);

  cerrarCompuertas();

  delay(50);
  Serial.println("Listo. Acerca una tarjeta o llavero al lector...");
}

void loop() {
  unsigned long ahora = millis();

  // Consultar el lector cada 100 ms sin bloquear el microcontrolador
  if (ahora - ultimaLectura >= INTERVALO_LECTURA_MS) {
    ultimaLectura = ahora;

    if (detectarTarjeta()) {
      ultimaDeteccion = ahora;

      if (!tapaAbierta) {
        imprimirUID();
        Serial.println("Tag detectado -> Abriendo compuertas R y L");
        abrirCompuertas();
        tapaAbierta = true;
      }

      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
    }
  }

  // Si pasó el tiempo sin detectar el tag, cerrar ambos servos
  if (tapaAbierta && (ahora - ultimaDeteccion >= TIEMPO_CIERRE_MS)) {
    Serial.println("Tag alejado -> Cerrando compuertas R y L");
    cerrarCompuertas();
    tapaAbierta = false;
  }
}


