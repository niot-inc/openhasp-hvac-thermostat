// sht30.cpp
#include <Wire.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "hasplib.h"   // LOG_* 사용
#include "sht30.h"
#include "state.h"   // G.temp_c, G.rh_pct 사용

// --- Board-fixed I2C pins (WT32S3/ZX3D95CE01S-TR-4848)
static const int I2C_SDA = 15;
static const int I2C_SCL = 6;

// --- SHT30 I2C/commands ---
#define SHT30_ADDR            0x44
#define CMD_SOFT_RESET        0x30A2
#define CMD_CLEAR_STATUS      0x3041
#define CMD_HEATER_OFF        0x3066
#define CMD_READ_STATUS       0xF32D
#define CMD_SINGLE_NS_LOW     0x2416   // No-stretch, Low repeatability (ESP32-S3에서 PASS)
#define CMD_SINGLE_CS_LOW     0x2C10   // Clock-stretch, Low (Fallback)

// 보정값 (기본 0)
static float s_temp_offset = 0.0f;
static float s_rh_offset   = 0.0f;

// --- 이상값 감지 / 자동 복구 ---
// 증상: 드물게 40~42°C가 지속되고 재부팅해야 복구됨.
// 유력 원인: 공유 I2C 버스(터치)에서 트랜잭션이 깨진 뒤 3바이트 어긋난 정렬이
// 고착되어 습도 워드가 온도로 읽히는 경우 (블록 단위 스왑은 CRC를 통과함),
// 또는 센서 내부 상태/히터 latch. 어느 쪽이든 버스 재초기화 + soft reset으로 풀림.
static const float TEMP_ABS_MIN  = -10.0f; // 실내 온도조절기 타당 범위
static const float TEMP_ABS_MAX  =  50.0f;
static const float TEMP_MAX_JUMP =   5.0f; // 5초 샘플 간 이 이상 변화는 비정상
static const int   RECOVER_EVERY =   3;    // 연속 이상 N회마다 버스/센서 복구
static const int   RESEED_AFTER  =   8;    // 복구 후에도 같은 값이 지속되면 새 기준으로 수용

static float    s_last_t    = NAN;  // 마지막으로 수용한 온도 (급변 판정 기준)
static int      s_bad_count = 0;    // 연속 이상(통신 실패 + 값 거부) 횟수
static uint16_t s_rawT = 0, s_rawH = 0;  // 최근 읽기의 원시 워드 (진단 로그용)

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
    s_rawT = rawT; s_rawH = rawH;

    T = -45.0f + 175.0f * (rawT / 65535.0f);
    H = 100.0f * (rawH / 65535.0f);

    // 보정 적용
    T += s_temp_offset;
    H += s_rh_offset;

    if (H < 0) H = 0;
    if (H > 100) H = 100;
    return true;
}

static bool read_status(uint16_t &st) {
    if (!write16(CMD_READ_STATUS)) return false;
    uint8_t b[3];
    if (Wire.requestFrom((int)SHT30_ADDR, 3) != 3) return false;
    for (int i=0; i<3; i++) b[i] = Wire.read();
    if (crc8_sht(b,2) != b[2]) return false;
    st = (uint16_t(b[0]) << 8) | b[1];
    return true;
}

// 슬레이브가 SDA를 물고 있을 때 SCL을 최대 9회 토글해 버스를 풀어준다
static void bus_unstick() {
    pinMode(I2C_SDA, INPUT_PULLUP);
    pinMode(I2C_SCL, OUTPUT_OPEN_DRAIN);
    digitalWrite(I2C_SCL, HIGH);
    for (int i=0; i<9 && digitalRead(I2C_SDA) == LOW; i++) {
        digitalWrite(I2C_SCL, LOW);  delayMicroseconds(10);
        digitalWrite(I2C_SCL, HIGH); delayMicroseconds(10);
    }
    // STOP 조건: SCL high 상태에서 SDA low -> high
    pinMode(I2C_SDA, OUTPUT_OPEN_DRAIN);
    digitalWrite(I2C_SDA, LOW);  delayMicroseconds(10);
    digitalWrite(I2C_SCL, HIGH); delayMicroseconds(10);
    digitalWrite(I2C_SDA, HIGH); delayMicroseconds(10);
}

// 재부팅 없이 부팅 시퀀스와 동일한 상태로 되돌린다:
// I2C 드라이버 재초기화(밀린 FIFO 정리) + 센서 heater off/soft reset(내부 latch 해제)
static void recover_bus_and_sensor() {
    LOG_INFO(TAG_CUSTOM, "SHT30 recovery: reinit I2C bus + sensor soft reset");
    Wire.end();
    bus_unstick();
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(50000);
    #if ARDUINO_ESP32_MAJOR_VERSION >= 2
    Wire.setTimeOut(300);
    #else
    Wire.setTimeout(300);
    #endif
    write16(CMD_HEATER_OFF);   delay(2);
    write16(CMD_SOFT_RESET);   delay(10);
    write16(CMD_CLEAR_STATUS); delay(2);
}

static bool plausible(float t) {
    if (t < TEMP_ABS_MIN || t > TEMP_ABS_MAX) return false;
    if (!isnan(s_last_t) && fabsf(t - s_last_t) > TEMP_MAX_JUMP) return false;
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

        write16(CMD_HEATER_OFF);   delay(2);
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
        float t, h;
        // 1차: no-stretch, Low repeatability / 2차: clock-stretch, Low
        bool ok = read_once(CMD_SINGLE_NS_LOW, 30, t, h)
               || read_once(CMD_SINGLE_CS_LOW, 50, t, h);

        if (ok && plausible(t)) {
            s_last_t = t; s_bad_count = 0;
            T = t; H = h;
            return true;
        }

        s_bad_count++;

        if (ok) {
            // 통신/CRC는 정상인데 값이 비정상 → 정렬 어긋남 또는 히터/내부 latch 의심.
            // 3바이트 스왑 가설 검증용으로 뒤바꿔 해석한 값과 히터 비트를 함께 남긴다.
            float t_sw = -45.0f + 175.0f * (s_rawH / 65535.0f);
            float h_sw = 100.0f * (s_rawT / 65535.0f);
            uint16_t st = 0;
            bool st_ok = read_status(st);
            LOG_ERROR(TAG_CUSTOM,
                "SHT30 implausible T=%.2f (last=%.2f) rawT=0x%04X rawH=0x%04X"
                " | swapped T=%.2f H=%.1f | status=0x%04X heater=%s (bad #%d)",
                t, s_last_t, s_rawT, s_rawH, t_sw, h_sw,
                st, st_ok ? ((st & (1u<<13)) ? "ON" : "off") : "n/a", s_bad_count);
        } else {
            LOG_ERROR(TAG_CUSTOM, "SHT30 read failed (bad #%d)", s_bad_count);
        }

        if (s_bad_count % RECOVER_EVERY == 0) {
            recover_bus_and_sensor();
        }

        // 복구를 거쳤는데도 타당 범위 안의 같은 값이 유지되면 실제 환경 변화로
        // 판단하고 새 기준으로 수용한다 (진짜 급변을 영구 거부하지 않기 위함)
        if (ok && s_bad_count >= RESEED_AFTER &&
            t >= TEMP_ABS_MIN && t <= TEMP_ABS_MAX) {
            LOG_INFO(TAG_CUSTOM, "SHT30 accepting persistent T=%.2f as new baseline", t);
            s_last_t = t; s_bad_count = 0;
            T = t; H = h;
            return true;
        }
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