#ifndef CCSDS_TELEMETRY_H
#define CCSDS_TELEMETRY_H

#include <Arduino.h>

// CCSDS Application Process IDs (APIDs)
#define APID_CDH_HEALTH   0x001
#define APID_ADCS_KINEMATICS 0x010
#define APID_TCS_THERMAL   0x020
#define APID_ECLSS_GAS     0x030
#define APID_EDGE_AI_ALERT 0x099

// Flight Operating Modes
enum FlightMode {
    MODE_BOOT_LAUNCH = 0,
    MODE_DETUMBLE    = 1,
    MODE_NOMINAL_OPS = 2,
    MODE_ECLIPSE     = 3,
    MODE_SAFEHOLD    = 4
};

// CCSDS Primary Header (6 bytes according to CCSDS 133.0-B-2)
struct __attribute__((packed)) CCSDS_PrimaryHeader {
    uint16_t packet_id;        // Version (3-bit), Type (1-bit), Sec Hdr Flag (1-bit), APID (11-bit)
    uint16_t packet_seq_ctrl; // Seq Flags (2-bit), Sequence Count (14-bit)
    uint16_t packet_length;   // Length of Packet Data Field - 1
};

// Subsystem Telemetry Structure
struct SubsystemTelemetry {
    uint32_t met_ms;           // Mission Elapsed Time in milliseconds
    uint8_t  flight_mode;      // FlightMode enum
    uint8_t  anomaly_flags;    // Anomaly & Fault flags
    float    accel_x, accel_y, accel_z; // ADCS g-force
    float    gyro_x, gyro_y, gyro_z;    // ADCS deg/sec
    float    quat_q0, quat_q1, quat_q2, quat_q3; // Orientation Quaternion
    float    lux_level;        // Coarse Sun Sensor (BH1750)
    float    ambient_temp_c;   // TCS / GSE Temp
    float    ir_object_temp_c; // TCS / Payload IR Temp
    float    relative_hum;     // GSE Humidity (DHT22)
    uint16_t gas_raw_adc;      // ECLSS Air Quality ADC
    float    edge_ai_score;    // TinyML Anomaly Score
};

// CRC16 Calculation for Space Packet Integrity
inline uint16_t calculate_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021; // CCITT polynomial
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

#endif // CCSDS_TELEMETRY_H
