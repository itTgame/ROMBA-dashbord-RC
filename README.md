# ROMBA-dashbord-RC

Simple local dashboard for a Roomba 886 connected through an ESP32 REST bridge.

## What it does
- Sends `Clean`, `Spot`, `Safe`, and `Stop` commands
- Polls `/api/sensors` and shows battery, charging state, current, and button data
- Renders the full sensor payload returned by the ESP32 API
- Saves the API base address in `localStorage`

## Start in 2 minutes
1. From the project folder, run `python3 -m http.server 8000`
2. Open `http://localhost:8000`
3. Enter your ESP32 address, for example `http://192.168.1.50`
4. Click **Save**
5. Click **Test**

If the header shows `Connected to Roomba`, the setup is working.

## Optional direct URL
- `http://localhost:8000/?apiBase=http://192.168.1.50`
- `?api=` is also supported as an alias

If you open the dashboard from another device on your network, replace `localhost` with your computer's local IP address.

## Expected ESP32 API
- `GET /api/status`
- `GET /api/sensors`
- `POST /api/clean`
- `POST /api/spot`
- `POST /api/safe`
- `POST /api/stop`

## Example sensor response
```json
{
  "battery_mV": 15600,
  "charging_state": 2,
  "current_mA": -320,
  "buttons": 0,
  "bump_left": false,
  "bump_right": false,
  "wheel_drop_left": false,
  "wheel_drop_right": false
}
```

## Wiring notes
1. Open `esp32/roomba_dashboard_api.ino`
2. Set `WIFI_SSID` and `WIFI_PASS`
3. Verify UART wiring: `RX=GPIO16`, `TX=GPIO17`
4. Upload the sketch with Arduino IDE or PlatformIO
5. Open Serial Monitor at `115200` baud and confirm the ESP32 prints its IP
6. In the dashboard, save the API base as `http://<ip>` and click **Test**

If `/api/sensors` returns `sensor_read_failed`, check:
- TX/RX wiring
- Shared `GND`
- Roomba is in `START` or `SAFE` mode

Recommended: test first with `esp32/roomba_basic_start.ino` before using the full API sketch.

## Roomba 886 Mini-DIN pinout

![Roomba 886 serial port Mini-DIN pinout](assets/roomba-886-mini-din-pinout.jpg)

- Pin 1: `Vpwr` - Roomba battery positive (unregulated)
- Pin 2: `Vpwr` - Roomba battery positive (unregulated)
- Pin 3: `RXD` - 0-5V serial input to Roomba
- Pin 4: `TXD` - 0-5V serial output from Roomba
- Pin 5: `BRC` - Baud rate change
- Pin 6: `GND` - Roomba battery ground
- Pin 7: `GND` - Roomba battery ground
