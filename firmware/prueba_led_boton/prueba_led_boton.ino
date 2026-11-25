/*
  Viernes - Prueba 1: LED + boton que se queda hundido
  Placa: ESP32-S3

  Conexiones:
    GPIO 4  -> resistencia (220 o 330 ohm) -> pata larga del LED
    pata corta del LED -> GND
    GPIO 5  -> una pata del boton
    otra pata del boton -> GND   (sin resistencia, usamos la interna)

  Que hace:
    - Boton HUNDIDO  -> el LED se ENCIENDE  (modo "escuchando")
    - Boton SUELTO   -> el LED se APAGA     (modo "dormido")
    - Escribe en el Monitor Serie cada vez que cambia.
*/

const int PIN_LED   = 4;   // LED de prueba
const int PIN_BOTON = 5;   // Boton que se queda hundido

bool escuchando = false;           // estado actual
bool ultimaLectura = HIGH;         // ultima lectura cruda del boton
bool estadoEstable = HIGH;         // lectura ya filtrada
unsigned long ultimoCambio = 0;    // para filtrar rebotes
const unsigned long TIEMPO_REBOTE = 40;  // milisegundos

void aplicarEstado() {
  digitalWrite(PIN_LED, escuchando ? HIGH : LOW);
  Serial.println(escuchando ? "Boton HUNDIDO -> Viernes ESCUCHANDO (LED encendido)"
                            : "Boton SUELTO  -> Viernes DORMIDO (LED apagado)");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== Viernes: prueba de LED y boton ===");

  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BOTON, INPUT_PULLUP);   // reposo = HIGH, hundido = LOW

  // Leer el estado inicial del boton
  estadoEstable = digitalRead(PIN_BOTON);
  ultimaLectura = estadoEstable;
  escuchando = (estadoEstable == LOW);
  aplicarEstado();
}

void loop() {
  bool lectura = digitalRead(PIN_BOTON);

  // Si la lectura cambio, reiniciamos el cronometro anti-rebote
  if (lectura != ultimaLectura) {
    ultimoCambio = millis();
    ultimaLectura = lectura;
  }

  // Si lleva estable mas de TIEMPO_REBOTE, aceptamos el cambio
  if ((millis() - ultimoCambio) > TIEMPO_REBOTE && lectura != estadoEstable) {
    estadoEstable = lectura;
    escuchando = (estadoEstable == LOW);
    aplicarEstado();
  }
}
