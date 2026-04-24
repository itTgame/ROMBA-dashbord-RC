/*
  Minimal Roomba startup test for ESP32.

  Use this sketch to verify:
  - the ESP32 UART pins are wired correctly
  - the Roomba accepts Open Interface commands
  - START + SAFE + CLEAN opcodes are reaching the robot
*/

#include <Arduino.h>

namespace {

constexpr int ROOMBA_RX = 16;
constexpr int ROOMBA_TX = 17;
constexpr uint32_t ROOMBA_BAUD = 115200;
constexpr uint16_t ROOMBA_BOOT_DELAY_MS = 2000;
constexpr uint16_t ROOMBA_MODE_DELAY_MS = 100;

constexpr uint8_t ROOMBA_START_OPCODE = 128;
constexpr uint8_t ROOMBA_SAFE_OPCODE = 131;
constexpr uint8_t ROOMBA_CLEAN_OPCODE = 135;

HardwareSerial roombaSerial(1);

void sendCommand(uint8_t opcode) {
  roombaSerial.write(opcode);
  roombaSerial.flush();
}

}  // namespace

void setup() {
  roombaSerial.begin(ROOMBA_BAUD, SERIAL_8N1, ROOMBA_RX, ROOMBA_TX);

  // Wait for the Roomba OI port to become ready after power-up.
  delay(ROOMBA_BOOT_DELAY_MS);

  sendCommand(ROOMBA_START_OPCODE);
  delay(ROOMBA_MODE_DELAY_MS);

  sendCommand(ROOMBA_SAFE_OPCODE);
  delay(ROOMBA_MODE_DELAY_MS);

  sendCommand(ROOMBA_CLEAN_OPCODE);
}

void loop() {
}
