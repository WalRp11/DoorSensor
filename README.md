# Door Sensor — ESP32-C3 SuperMini

Arduino/PlatformIO firmware for the `esp32-c3-supermini-door` device.
Replaces the previous ESPHome configuration.

---

## Hardware

| Pin | Function |
|-----|----------|
| GPIO 3 | Reed contact (INPUT_PULLUP — HIGH = open, LOW = closed) |
| GPIO 4 | FSR pressure sensor (ADC, 0–3.3 V) |
| GPIO 8 | WS2812 status LED (built-in on C3 SuperMini) |

> **Note:** If the reed switch shows inverted readings, swap `HIGH`/`LOW` in `tickDoor()` or reverse the hardware polarity.

---

## First-time flash (USB)

The device is currently accessible via the USB-C port.

> **Static IP:** The device is configured to use **192.168.0.54** permanently. Web UI: `http://192.168.0.54/`

```bash
# Build and flash
pio run -t upload

# Open serial monitor (115200 baud)
pio device monitor
```

On first boot with no saved configuration the device starts in **AP setup mode** automatically — see the next section.

---

## WiFi setup (via device AP)

When no WiFi credentials are saved, the device creates its own access point:

| Setting | Value |
|---------|-------|
| SSID | `DoorSensor-Setup` |
| Password | `doorsensor` |
| Config page | `http://192.168.4.1/config` |

**Steps:**
1. Connect your phone or PC to the `DoorSensor-Setup` network.
2. A captive portal opens automatically, or navigate to `http://192.168.4.1/config`.
3. Enter your home WiFi SSID and password under **WiFi**.
4. Click **Save & Restart**.
5. The device reboots, connects to your network, and shows its IP on the serial monitor.

To re-enter AP mode at any time, clear the saved SSID via the web config page (erase the SSID field and save) — the device will restart into AP mode.

---

## MQTT configuration (via device web page)

Once the device is connected to WiFi, open its local IP in a browser and go to **Config** (`/config`).

The MQTT section lets you set:

| Field | Default |
|-------|---------|
| Host / IP | `192.168.0.40` |
| Port | `1883` |
| Username | `pi` |
| Password | *(stored in NVS — not shown)* |

All settings are stored in NVS (non-volatile flash) and survive reboots and re-flashing via OTA. They are only erased by a full flash erase (`pio run -t erase`).

---

## OTA firmware updates

There are two ways to update the firmware after the initial USB flash.

### Web upload (browser)

1. Build the firmware: `pio run` (or use the PlatformIO Build button in VS Code).
2. Find the binary at:
   ```
   .pio/build/doorsensor/firmware.bin
   ```
3. Open `http://<device-ip>/update` in your browser.
4. Select the `.bin` file and click **Flash firmware**.
5. A progress bar shows the upload. The device reboots automatically when done.

### ArduinoOTA (PlatformIO / Arduino IDE)

The device announces itself as hostname `door-sensor` on mDNS once WiFi is connected.
A dedicated OTA environment is already configured in `platformio.ini`:

```bash
pio run -e doorsensor-ota -t upload
```

If mDNS resolution fails on your network, replace `door-sensor.local` in `platformio.ini`
with the device's IP address directly (`upload_port = 192.168.0.x`).

---

## LED status guide

| Colour | Meaning |
|--------|---------|
| White (brief) | Booting |
| Dim blue (solid) | Connecting to WiFi |
| Green (2 s flash) | WiFi connected |
| Cyan (1 s flash) | MQTT connected |
| Amber (solid) | AP setup mode — waiting for configuration |
| Dim red (solid) | WiFi lost |
| Red flash (0.5 s) | Door opened |
| Green flash (0.5 s) | Door closed |
| Red (20 s) | FSR alarm — lamp turned ON |
| Green (3 s) | FSR medium — lamp turned OFF |
| Blue (solid) | OTA flashing in progress |

---

## Web endpoints

All endpoints are on port 80.

| Path | Method | Description |
|------|--------|-------------|
| `/` | GET | Live status page (auto-refreshes every 30 s) |
| `/data` | GET | JSON snapshot of all sensor values |
| `/config` | GET / POST | WiFi + MQTT configuration form |
| `/update` | GET | OTA firmware upload page |
| `/update` | POST | Multipart firmware upload (used by the page) |
| `/restart` | POST | Reboot device |

### `/data` JSON example
```json
{
  "door_open": false,
  "fsr_voltage": 0.012,
  "wifi_rssi": -67,
  "wifi_quality": 66,
  "ip": "192.168.0.54",   // static — always this address
  "mqtt": true,
  "uptime_s": 3820,
  "free_heap": 198432
}
```

---

## MQTT topics

Device ID: `esp32-c3-supermini-door`

| Topic | Retained | Payload |
|-------|----------|---------|
| `esp32-c3-supermini-door/online` | yes | `true` / `false` (LWT) |
| `esp32-c3-supermini-door/binary_sensor/door_contact__reed_/state` | yes | `ON` (open) / `OFF` (closed) |
| `esp32-c3-supermini-door/sensor/fsr_pressure_sensor/state` | no | voltage, e.g. `0.012` |
| `esp32-c3-supermini-door/sensor/wifi_signal_strength/state` | no | RSSI in dBm |
| `esp32-c3-supermini-door/sensor/wifi_signal_quality/state` | no | 0–100 % |
| `esp32-c3-supermini-door/sensor/device_ip_address/state` | no | IP string |
| `esp32-c3-supermini-door/sensor/device_uptime/state` | no | uptime in minutes |
| `door/alarm` | no | `Lamp on` / `Lamp off` (FSR threshold triggers) |

Sensor topics are published every 30 seconds. The door contact publishes immediately on state change.

---

## FSR threshold logic

The FSR sensor controls the Shelly lamp at `192.168.0.118`:

| Transition | Action |
|-----------|--------|
| Below → above 1.0 V | Lamp **ON**, MQTT `door/alarm: Lamp on`, red LED 20 s |
| Below → above 0.5 V (without crossing 1.0 V) | Lamp **OFF**, MQTT `door/alarm: Lamp off`, green LED 3 s |

---

## Build commands

```bash
pio run                        # compile
pio run -t upload              # compile + USB flash
pio run -e doorsensor-ota -t upload   # compile + OTA flash
pio device monitor             # open serial monitor (115200)
pio run -t erase               # full flash erase (resets NVS config)
```
