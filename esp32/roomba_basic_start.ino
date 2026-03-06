#define ROOMBA_RX 16
#define ROOMBA_TX 17

HardwareSerial RoombaSerial(1);

void setup() {
  RoombaSerial.begin(115200, SERIAL_8N1, ROOMBA_RX, ROOMBA_TX);

  delay(2000);        // נותן זמן ל-ESP לעלות

  RoombaSerial.write(128);   // START
  delay(100);

  RoombaSerial.write(131);   // SAFE MODE
  delay(100);

  RoombaSerial.write(135);   // CLEAN
}

void loop() {
}
