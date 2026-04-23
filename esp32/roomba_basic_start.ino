#define ROOMBA_RX 16
#define ROOMBA_TX 17

HardwareSerial RoombaSerial(1);

void setup() {
  RoombaSerial.begin(115200, SERIAL_8N1, ROOMBA_RX, ROOMBA_TX);

  delay(2000); // English comment

  RoombaSerial.write(128); // English comment
  delay(100);

  RoombaSerial.write(131); // English comment
  delay(100);

  RoombaSerial.write(135); // English comment
}

void loop() {
}
