/*
  סקיצה מינימלית לבדיקת תקשורת SCI בלבד (ללא REST API).
  מיועדת לבדוק שה-ESP32 שולח פקודות בסיסיות לרומבה דרך UART.
*/

#define ROOMBA_RX 16
#define ROOMBA_TX 17

const uint8_t ROOMBA_START = 128;
const uint8_t ROOMBA_SAFE  = 131;
const uint8_t ROOMBA_CLEAN = 135;

HardwareSerial RoombaSerial(1);

void setup() {
  // פתיחת UART1 לפיני RX/TX של רומבה
  RoombaSerial.begin(115200, SERIAL_8N1, ROOMBA_RX, ROOMBA_TX);

  delay(2000); // זמן עלייה ל-ESP32

  // רצף פקודות בסיסי ל-Roomba SCI
  RoombaSerial.write(ROOMBA_START); // START
  delay(100);

  RoombaSerial.write(ROOMBA_SAFE);  // SAFE MODE
  delay(100);

  RoombaSerial.write(ROOMBA_CLEAN); // CLEAN
}

void loop() {
  // אין לולאה פעילה בסקיצה זו
}
