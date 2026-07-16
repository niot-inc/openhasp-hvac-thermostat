// sht30.cpp
#include <Wire.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "sht30.h"
#include "state.h"   // G.temp_c, G.rh_pct 사용

// --- Board-fixed I2C pins (WT32S3/ZX3D95CE01S-TR-4848)
static const int I2C_SDA = 15;
static const int I2C_SCL = 6;

// --- SHT30 I2C/commands ---
#define SHT30_ADDR            0x44
#define CMD_SOFT_RESET        0x30A2
#define CMD_CLEAR_STATUS      0x3041
#define CMD_SINGLE_NS_LOW     0x2416   // No-stretch, Low repeatability (ESP32-S3에서 PASS)
#define CMD_SINGLE_CS_LOW     0x2C10   // Clock-stretch, Low (Fallback)

// 보정값 (기본 0)
static float s_temp_offset = 0.0f;
static float s_rh_offset   = 0.0f;

// ----- 유틸 -----
static uint8_t crc8_sht(const uint8_t* d, int n) {
    uint8_t c = 0xFF;
    for (int i=0; i<n; i++) {
        c ^= d[i];
        for (int b=0; b<8; b++) {
            c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x31) : (uint8_t)(c << 1);
        }
    }
    return c;
}

static bool write16(uint16_t cmd) {
    Wire.beginTransmission(SHT30_ADDR);
    Wire.write(uint8_t(cmd >> 8));
    Wire.write(uint8_t(cmd & 0xFF));
    return Wire.endTransmission() == 0;
}

static bool read6(uint8_t b[6]) {
    if (Wire.requestFrom((int)SHT30_ADDR, 6) != 6) return false;
    for (int i=0; i<6; i++) b[i] = Wire.read();
    return true;
}

static bool read_once(uint16_t cmd, uint32_t wait_ms, float &T, float &H) {
    if (!write16(cmd)) return false;
    delay(wait_ms);

    uint8_t b[6];
    if (!read6(b)) return false;
    if (crc8_sht(b,2) != b[2]) return false;
    if (crc8_sht(b+3,2) != b[5]) return false;

    uint16_t rawT = (uint16_t(b[0]) << 8) | b[1];
    uint16_t rawH = (uint16_t(b[3]) << 8) | b[4];

    T = -45.0f + 175.0f * (rawT / 65535.0f);
    H = 100.0f * (rawH / 65535.0f);

    // 보정 적용
    T += s_temp_offset;
    H += s_rh_offset;

    if (H < 0) H = 0;
    if (H > 100) H = 100;
    return true;
}

static void save_calibration() {
    if (!LittleFS.begin()) return;
    DynamicJsonDocument doc(256);
    doc["temp_offset"] = s_temp_offset;
    doc["rh_offset"]   = s_rh_offset;
    File f = LittleFS.open("/superb.json", "w");
    if (f) { serializeJsonPretty(doc, f); f.close(); }
}

static void load_calibration() {
    if (!LittleFS.begin()) return;
    if (!LittleFS.exists("/superb.json")) {
        save_calibration();
        return;
    }
    File f = LittleFS.open("/superb.json", "r");
    if (!f) return;

    DynamicJsonDocument doc(256);
    if (!deserializeJson(doc, f)) {
        s_temp_offset = doc["temp_offset"] | 0.0f;
        s_rh_offset   = doc["rh_offset"]   | 0.0f;
    }
    f.close();
}

// ----- Namespace sht30 -----
namespace sht30 {

    void begin() {
        Wire.begin(I2C_SDA, I2C_SCL);
        Wire.setClock(50000);   // ESP32-S3에서 안정적
        #if ARDUINO_ESP32_MAJOR_VERSION >= 2
        Wire.setTimeOut(300);
        #else
        Wire.setTimeout(300);
        #endif

        write16(CMD_SOFT_RESET);   delay(10);
        write16(CMD_CLEAR_STATUS); delay(2);

        load_calibration();

        float t,h;
        if (read(t,h)) {
            G.temp_c = t;
            G.rh_pct = h;
        }
    }

    bool read(float &T, float &H) {
        // 1차: no-stretch, Low repeatability
        if (read_once(CMD_SINGLE_NS_LOW, 30, T, H)) return true;
        // 2차: clock-stretch, Low
        if (read_once(CMD_SINGLE_CS_LOW, 50, T, H)) return true;
        return false;
    }

    void set_offsets(float temp_offset, float rh_offset, bool persist) {
        s_temp_offset = temp_offset;
        s_rh_offset   = rh_offset;
        if (persist) save_calibration();
    }

    void get_offsets(float &temp_offset, float &rh_offset) {
        temp_offset = s_temp_offset;
        rh_offset   = s_rh_offset;
    }
}