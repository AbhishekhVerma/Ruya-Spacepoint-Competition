# Ru'ya SpacePoint Competition - Instructor & Judge Master Guide

Welcome to the **Ru'ya SpacePoint Competition** instructor manual! This document provides all hardware schematics, firmware architectures, Docker telemetry stack setups, 3D Digital Twin configurations, and judging rubrics for evaluating competitor entries.

---

## 1. Hardware Assembly & Wiring Blueprint

```
+-------------------------------------------------------------------------+
|                              ESP32-S3 OBC                               |
|                                                                         |
|   3.3V  GND  GPIO8(SDA)  GPIO9(SCL)   GPIO4(DHT)   GPIO1(Gas ADC)  5V   |
+----+-----+------+---------+------------+------------+---------------+---+
     |     |      |         |            |            |               |
     |     |      +----+----+            |            |               |
     |     |           |                 |            |               |
     |     |     +-----+-----+           |            |               |
     |     |     | I2C BUS   |           |            |               |
     |     |     | (3.3V)    |           |            |               |
     |     |     +-----+-----+           |            |               |
     |     |           |                 |            |               |
     +-----+-----------+---> BH1750      |            |               |
     |     |           |     (Addr 0x23) |            |               |
     |     |           |                 |            |               |
     +-----+-----------+---> MLX90614    |            |               |
     |     |           |     (Addr 0x5A) |            |               |
     |     |           |                 |            |               |
     +-----+-----------+---> MPU6050     |            |               |
     |     |                 (Addr 0x68) |            |               |
     |     |                             |            |               |
     +-----+-----------------------------+---> DHT22  |               |
     |     |                                   (10k pull-up)          |
     |     |                                          |               |
     +-----+------------------------------------------+---> Gas Sensor|
     |     |                                                (Signal)  |
     +-----+----------------------------------------------------------+---> (VCC 5V)
```

### Pin Assignment Table:
- **I2C Bus (SDA / SCL)**: `GPIO 8` (SDA) / `GPIO 9` (SCL) with 4.7kΩ pull-up resistors to 3.3V.
- **DHT22 Data**: `GPIO 4` with 10kΩ pull-up resistor to 3.3V.
- **Gas Sensor Signal**: `GPIO 1` (ADC1_CH0). *Note: Ensure gas module output voltage is scaled to 0–3.3V max!*
- **Gas Sensor VCC**: `5V` pin on ESP32-S3 (for internal heater coil).

---

## 2. Telemetry Pipeline Deployment Guide

### A. ESP32-S3 Firmware Deployment

#### Option 1: Arduino IDE Setup (Recommended for Students/Instructors)
1. Open **Arduino IDE** (v2.x recommended).
2. Go to **File -> Preferences** and add the ESP32 board manager URL:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. Go to **Tools -> Board -> Boards Manager...**, search for `esp32` by Espressif Systems, and install it.
4. Go to **Tools -> Board -> esp32 -> ESP32S3 Dev Module**.
5. Set Board Settings:
   - **PSRAM**: `Enabled` / `OPI PSRAM`
   - **Partition Scheme**: `Huge APP (3MB No OTA / 1MB SPIFFS)`
6. Install Required Libraries via **Tools -> Manage Libraries...**:
   - `ArduinoJson` (v6.x)
   - `PubSubClient` (v2.8)
   - `DHT sensor library` (v1.4.x)
   - `BH1750` (v1.3.x)
   - `Adafruit MLX90614 Library` (v2.1.x)
   - `MPU6050` (by Electronic Cats)
7. Open `Ruya_SpacePoint_CubeSat/Ruya_SpacePoint_CubeSat.ino`, update `WIFI_SSID`, `WIFI_PASSWORD`, and `MQTT_SERVER` IP.
8. Click **Upload** (or Ctrl+U).

#### Option 2: PlatformIO (VS Code)
1. Open terminal inside the repository folder.
2. Connect ESP32-S3 via USB cable.
3. Compile and flash:
   ```bash
   pio run --target upload
   pio device monitor
   ```

### B. Docker Ground Segment Launch
1. Open terminal in repository root.
2. Launch the Mosquitto + InfluxDB + Telegraf + Grafana + Orbit Simulator stack:
   ```bash
   docker compose up -d
   ```
3. Verify running containers:
   ```bash
   docker compose ps
   ```
4. Access Grafana Mission Control Dashboard at `http://localhost:3000` (User: `admin` / Password: `spacepoint`).
5. Open 3D Digital Twin in browser or embedded Grafana panel at `http://localhost:3000/public/threejs_digital_twin.html`.

---

## 3. Red Team Fault Injection Manual for Judges

Instructors and judges can inject live flight anomalies into student telemetry streams using standard MQTT commands to evaluate emergency response:

```bash
# Inject Fault 1: Satellite Tumbling State (High angular velocity spin)
mosquitto_pub -h localhost -t "spacepoint/commands/fault" -m "FAULT_01_TUMBLE"

# Inject Fault 2: Thermal Radiator Saturation (Spike MLX90614 reading to > 75°C)
mosquitto_pub -h localhost -t "spacepoint/commands/fault" -m "FAULT_02_THERMAL"

# Inject Fault 3: Avionics I2C Bus Arbitration Lock (Freeze I2C sensors)
mosquitto_pub -h localhost -t "spacepoint/commands/fault" -m "FAULT_03_I2C_LOCK"

# Inject Fault 4: ECLSS Cabin Gas Leak (Spike Gas sensor value to critical threshold)
mosquitto_pub -h localhost -t "spacepoint/commands/fault" -m "FAULT_04_ECLSS_LEAK"

# Reset All Faults to Nominal Mission State
mosquitto_pub -h localhost -t "spacepoint/commands/fault" -m "RESET_FAULTS"
```

---

## 4. Sample Competition Judging Rubric

```
+------------------------------------------------------------------------+
| Criteria                          | Weight | Evaluation Focus          |
+-----------------------------------+--------+---------------------------+
| 1. Space Systems Engineering      |  25%   | C&DH state machine, MET,  |
|                                   |        | CCSDS APID header structure|
+-----------------------------------+--------+---------------------------+
| 2. Firmware Integrity & Multi-Task|  20%   | FreeRTOS task separation, |
|                                   |        | I2C bus lockup recovery   |
+-----------------------------------+--------+---------------------------+
| 3. Mission Control UX & 3D Twin   |  20%   | Grafana panel layout,     |
|                                   |        | 3D Digital Twin sync      |
+-----------------------------------+--------+---------------------------+
| 4. Sensor Calibration & Edge AI   |  20%   | Engineering unit accuracy,|
|                                   |        | TinyML anomaly scoring    |
+-----------------------------------+--------+---------------------------+
| 5. Red Team Viva Defense          |  15%   | Explanation of vacuum     |
|                                   |        | limits, fault diagnosis   |
+-----------------------------------+--------+---------------------------+
```
