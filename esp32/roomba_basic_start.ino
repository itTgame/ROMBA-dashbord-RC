#define ROOMBA_RX 16
#define ROOMBA_TX 17

HardwareSerial RoombaSerial(1);

void setup() {
  RoombaSerial.begin(115200, SERIAL_8N1, ROOMBA_RX, ROOMBA_TX);

  delay(2000);  // מאפשר ל-ESP32 לסיים אתחול לפני תקשורת עם הרובוט.

  RoombaSerial.write(128);  // פקודת התחלה לממשק הפקודות של הרומבה.
  delay(100);

  RoombaSerial.write(131);  // מעבר למצב בטוח להפעלת פקודות תנועה/ניקוי.
  delay(100);

  RoombaSerial.write(135);  // התחלת ניקוי מלא.
}

void loop() {
}
