/*
 * Ru'ya SpacePoint Competition - CubeSat OBC Telemetry Firmware
 * Target Microcontroller: ESP32-S3
 * IDE: Arduino IDE (v2.x or v1.8.x)
 * 
 * Sensors:
 * Sensors:
 * - DHT22: GPIO 5 (Humidity & Ambient Temp)
 * - BH1750: I2C GPIO 6 (SDA) / GPIO 7 (SCL), Addr 0x23 (Coarse Sun Sensor)
 * - MLX90614: I2C GPIO 6 (SDA) / GPIO 7 (SCL), Addr 0x5A (IR Thermal Payload)
 * - MPU6050: I2C GPIO 6 (SDA) / GPIO 7 (SCL), Addr 0x68 (ADCS 6-DOF IMU)
 * - Gas Sensor: GPIO 4 (ADC1_CH3, ECLSS Air Quality)
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <BH1750.h>
#include <Adafruit_MLX90614.h>
#include <MPU6050.h>

#include "ccsds_telemetry.h"

// --- Pin Assignments ---
#define GAS_ADC_PIN    4   // Gas Sensor Analog Out (Must be ADC1 pin on ESP32-S3)
#define DHT_PIN        5   // DHT22 Data Pin
#define I2C_SDA_PIN    6   // I2C SDA (for MPU6050, BH1750, MLX90614)
#define I2C_SCL_PIN    7   // I2C SCL (for MPU6050, BH1750, MLX90614)
#define DHT_TYPE       DHT22

// --- Network & MQTT Settings ---
const char* WIFI_SSID     = "TinyGS-Network";
const char* WIFI_PASSWORD = "groundst";
const char* MQTT_SERVER   = "192.168.137.1"; // Replace with your computer/server IP address
const int   MQTT_PORT     = 1883;

// --- Sensor Objects ---
DHT dht(DHT_PIN, DHT_TYPE);
BH1750 lightMeter;
Adafruit_MLX90614 mlx = Adafruit_MLX90614();
MPU6050 mpu;

// --- Network Clients ---
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// --- Global Telemetry State & Synchronization ---
SubsystemTelemetry current_telemetry;
SemaphoreHandle_t telemetryMutex;
uint32_t sequence_counter = 0;

// --- Red Team Simulated Fault Flags ---
bool fault_tumble_active     = false;
bool fault_thermal_active    = false;
bool fault_i2c_lock_active   = false;
bool fault_eclss_leak_active = false;

// --- Function Prototypes ---
void setupWiFi();
void reconnectMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
float computeTinyMLAnomalyScore(const SubsystemTelemetry& telem);
void updateQuaternion(float gx, float gy, float gz, float ax, float ay, float az, float dt);

// --- FreeRTOS Task: Sensor Reading & Quaternion Kinematics (Core 1) ---
void vTaskSensorRead(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(100); // 10 Hz loop

    int16_t ax, ay, az, gx, gy, gz;

    for (;;) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);

        // Sample DHT22
        float h = dht.readHumidity();
        float t = dht.readTemperature();

        // Sample BH1750
        float lux = 0.0;
        if (!fault_i2c_lock_active) {
            lux = lightMeter.readLightLevel();
        }

        // Sample MLX90614
        float amb_temp = 0.0, obj_temp = 0.0;
        if (!fault_i2c_lock_active) {
            amb_temp = mlx.readAmbientTempC();
            obj_temp = mlx.readObjectTempC();
        }
        if (fault_thermal_active) {
            obj_temp += 55.0; // Simulate radiator heat saturation
        }

        // Sample MPU6050
        if (!fault_i2c_lock_active) {
            mpu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);
        }
        
        float g_ax = ax / 16384.0f;
        float g_ay = ay / 16384.0f;
        float g_az = az / 16384.0f;
        float deg_gx = gx / 131.0f;
        float deg_gy = gy / 131.0f;
        float deg_gz = gz / 131.0f;

        if (fault_tumble_active) {
            deg_gx += 145.0f;
            deg_gy -= 120.0f;
            deg_gz += 180.0f;
        }

        // Sample Analog Gas Sensor
        uint16_t gas_raw = analogRead(GAS_ADC_PIN);
        if (fault_eclss_leak_active) {
            gas_raw += 2200; // Simulate gas leak spike
        }

        // Update Quaternion Orientation
        updateQuaternion(deg_gx, deg_gy, deg_gz, g_ax, g_ay, g_az, 0.1f);

        // Lock Telemetry State for Thread-Safe Update
        if (xSemaphoreTake(telemetryMutex, portMAX_DELAY) == pdTRUE) {
            current_telemetry.met_ms           = millis();
            current_telemetry.accel_x          = g_ax;
            current_telemetry.accel_y          = g_ay;
            current_telemetry.accel_z          = g_az;
            current_telemetry.gyro_x           = deg_gx;
            current_telemetry.gyro_y           = deg_gy;
            current_telemetry.gyro_z           = deg_gz;
            current_telemetry.lux_level        = (lux < 0.0) ? 0.0 : lux;
            current_telemetry.ambient_temp_c   = (isnan(amb_temp)) ? t : amb_temp;
            current_telemetry.ir_object_temp_c = obj_temp;
            current_telemetry.relative_hum     = isnan(h) ? 0.0 : h;
            current_telemetry.gas_raw_adc      = gas_raw;

            // Flight Operating Mode State Machine
            if (current_telemetry.lux_level < 5.0) {
                current_telemetry.flight_mode = MODE_ECLIPSE;
            } else if (abs(deg_gx) > 45.0 || abs(deg_gy) > 45.0 || abs(deg_gz) > 45.0) {
                current_telemetry.flight_mode = MODE_DETUMBLE;
            } else {
                current_telemetry.flight_mode = MODE_NOMINAL_OPS;
            }

            xSemaphoreGive(telemetryMutex);
        }
    }
}

// --- FreeRTOS Task: Edge AI TinyML Anomaly Detection (Core 1) ---
void vTaskEdgeAIAnomaly(void *pvParameters) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200)); // 5 Hz loop

        SubsystemTelemetry telem_copy;
        if (xSemaphoreTake(telemetryMutex, portMAX_DELAY) == pdTRUE) {
            telem_copy = current_telemetry;
            xSemaphoreGive(telemetryMutex);
        }

        float anomaly_score = computeTinyMLAnomalyScore(telem_copy);

        if (xSemaphoreTake(telemetryMutex, portMAX_DELAY) == pdTRUE) {
            current_telemetry.edge_ai_score = anomaly_score;
            if (anomaly_score > 2.5f) { // Anomaly Threshold Trigger
                current_telemetry.flight_mode = MODE_SAFEHOLD;
                current_telemetry.anomaly_flags |= 0x01; // Set Anomaly Flag
            } else {
                current_telemetry.anomaly_flags &= ~0x01;
            }
            xSemaphoreGive(telemetryMutex);
        }
    }
}

// --- FreeRTOS Task: Telemetry Downlink over MQTT (Core 0) ---
void vTaskTelemetryDownlink(void *pvParameters) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000)); // 1 Hz Downlink

        if (!mqttClient.connected()) {
            reconnectMQTT();
        }
        mqttClient.loop();

        SubsystemTelemetry telem;
        if (xSemaphoreTake(telemetryMutex, portMAX_DELAY) == pdTRUE) {
            telem = current_telemetry;
            xSemaphoreGive(telemetryMutex);
        }

        sequence_counter++;

        // 1. Publish JSON Telemetry for Grafana & Telegraf
        StaticJsonDocument<512> doc;
        doc["sync_word"]     = "0x1ACFFC1D";
        doc["seq_num"]       = sequence_counter;
        doc["met_ms"]        = telem.met_ms;
        doc["flight_mode"]   = telem.flight_mode;
        doc["anomaly_flags"] = telem.anomaly_flags;
        doc["edge_ai_score"] = telem.edge_ai_score;

        JsonObject adcs = doc.createNestedObject("adcs");
        adcs["gyro_x"]  = telem.gyro_x;
        adcs["gyro_y"]  = telem.gyro_y;
        adcs["gyro_z"]  = telem.gyro_z;
        adcs["accel_x"] = telem.accel_x;
        adcs["accel_y"] = telem.accel_y;
        adcs["accel_z"] = telem.accel_z;
        adcs["q0"]      = telem.quat_q0;
        adcs["q1"]      = telem.quat_q1;
        adcs["q2"]      = telem.quat_q2;
        adcs["q3"]      = telem.quat_q3;
        adcs["lux"]     = telem.lux_level;

        JsonObject tcs = doc.createNestedObject("tcs");
        tcs["amb_c"] = telem.ambient_temp_c;
        tcs["ir_c"]  = telem.ir_object_temp_c;

        JsonObject eclss = doc.createNestedObject("eclss");
        eclss["gas_adc"] = telem.gas_raw_adc;

        JsonObject gse = doc.createNestedObject("gse");
        gse["hum"]  = telem.relative_hum;

        char buffer[512];
        serializeJson(doc, buffer);
        Serial.println("--- LIVE TELEMETRY ---");
        Serial.println(buffer);
        mqttClient.publish("spacepoint/telemetry/json", buffer);

        // 2. Publish CCSDS Binary Space Packet Telemetry
        CCSDS_PrimaryHeader header;
        header.packet_id       = htons((0x0800) | APID_CDH_HEALTH); // Telemetry Packet + APID
        header.packet_seq_ctrl = htons((0xC000) | (sequence_counter & 0x3FFF));
        header.packet_length   = htons(sizeof(SubsystemTelemetry) + 2 - 1);

        uint8_t ccsds_buf[sizeof(CCSDS_PrimaryHeader) + sizeof(SubsystemTelemetry) + 2];
        memcpy(ccsds_buf, &header, sizeof(header));
        memcpy(ccsds_buf + sizeof(header), &telem, sizeof(telem));

        uint16_t crc = calculate_crc16(ccsds_buf, sizeof(header) + sizeof(telem));
        ccsds_buf[sizeof(ccsds_buf) - 2] = (crc >> 8) & 0xFF;
        ccsds_buf[sizeof(ccsds_buf) - 1] = crc & 0xFF;

        mqttClient.publish("spacepoint/telemetry/ccsds", ccsds_buf, sizeof(ccsds_buf));
    }
}

// --- Arduino Setup ---
void setup() {
    Serial.begin(115200);
    telemetryMutex = xSemaphoreCreateMutex();

    // Initialize I2C Bus on ESP32-S3 (GPIO 8 SDA, GPIO 9 SCL)
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, 400000); // 400 kHz Fast-mode

    dht.begin();
    lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
    mlx.begin();
    mpu.initialize();

    // Default Quaternion Initializer
    current_telemetry.quat_q0 = 1.0f;
    current_telemetry.quat_q1 = 0.0f;
    current_telemetry.quat_q2 = 0.0f;
    current_telemetry.quat_q3 = 0.0f;

    setupWiFi();
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setBufferSize(1024); // Increase buffer size for large telemetry JSON payload

    // Launch FreeRTOS Tasks across Dual Cores
    xTaskCreatePinnedToCore(vTaskSensorRead, "SensorReadTask", 4096, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(vTaskEdgeAIAnomaly, "TinyMLEngineTask", 4096, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(vTaskTelemetryDownlink, "MQTTDownlinkTask", 8192, NULL, 1, NULL, 0);

    Serial.println("[ESP32-S3 OBC] Systems Initialized & Flight State Machine Active.");
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}

// --- TinyML Reconstruction Error Mockup ---
float computeTinyMLAnomalyScore(const SubsystemTelemetry& telem) {
    float norm_gyro  = (abs(telem.gyro_x) + abs(telem.gyro_y) + abs(telem.gyro_z)) / 300.0f;
    float norm_temp  = (telem.ir_object_temp_c > 50.0f) ? (telem.ir_object_temp_c - 50.0f) / 10.0f : 0.0f;
    float norm_gas   = (telem.gas_raw_adc > 2000) ? (telem.gas_raw_adc - 2000) / 500.0f : 0.0f;
    
    return norm_gyro + norm_temp + norm_gas;
}

// --- Quaternion Kinematics Integration ---
void updateQuaternion(float gx, float gy, float gz, float ax, float ay, float az, float dt) {
    float rad_x = gx * 0.0174533f;
    float rad_y = gy * 0.0174533f;
    float rad_z = gz * 0.0174533f;

    float q0 = current_telemetry.quat_q0;
    float q1 = current_telemetry.quat_q1;
    float q2 = current_telemetry.quat_q2;
    float q3 = current_telemetry.quat_q3;

    q0 += 0.5f * (-q1 * rad_x - q2 * rad_y - q3 * rad_z) * dt;
    q1 += 0.5f * ( q0 * rad_x + q2 * rad_z - q3 * rad_y) * dt;
    q2 += 0.5f * ( q0 * rad_y - q1 * rad_z + q3 * rad_x) * dt;
    q3 += 0.5f * ( q0 * rad_z + q1 * rad_y - q2 * rad_x) * dt;

    float norm = sqrt(q0 * q0 + q1 * q1 + q2 * q2 + q3 * q3);
    if (norm > 0.0f) {
        current_telemetry.quat_q0 = q0 / norm;
        current_telemetry.quat_q1 = q1 / norm;
        current_telemetry.quat_q2 = q2 / norm;
        current_telemetry.quat_q3 = q3 / norm;
    }
}

// --- Network Setup & MQTT Reconnect ---
void setupWiFi() {
    delay(10);
    Serial.printf("[Wi-Fi] Connecting to %s...\n", WIFI_SSID);
    WiFi.disconnect();
    delay(100);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    // Wait for connection to finish so we don't spam the router
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\n[Wi-Fi] Connected!");
}

void reconnectMQTT() {
    while (!mqttClient.connected()) {
        if (WiFi.status() != WL_CONNECTED) {
            setupWiFi();
            delay(1000);
            continue;
        }
        if (mqttClient.connect("ESP32S3_CubeSat_OBC")) {
            Serial.println("[MQTT] Downlink Connected.");
            mqttClient.subscribe("spacepoint/commands/fault");
        } else {
            delay(2000);
        }
    }
}

// --- Red Team Fault Injection Handler ---
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    char message[64];
    size_t copy_len = (length < 63) ? length : 63;
    memcpy(message, payload, copy_len);
    message[copy_len] = '\0';

    Serial.printf("[RED TEAM COMMAND] Injected: %s\n", message);

    if (strcmp(message, "FAULT_01_TUMBLE") == 0) {
        fault_tumble_active = true;
    } else if (strcmp(message, "FAULT_02_THERMAL") == 0) {
        fault_thermal_active = true;
    } else if (strcmp(message, "FAULT_03_I2C_LOCK") == 0) {
        fault_i2c_lock_active = true;
    } else if (strcmp(message, "FAULT_04_ECLSS_LEAK") == 0) {
        fault_eclss_leak_active = true;
    } else if (strcmp(message, "RESET_FAULTS") == 0) {
        fault_tumble_active     = false;
        fault_thermal_active    = false;
        fault_i2c_lock_active   = false;
        fault_eclss_leak_active = false;
    }
}
