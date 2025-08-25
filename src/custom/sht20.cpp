// sht20.cpp
#include <Wire.h>
#include "sht20.h"

// --- Board-fixed I2C pins (WT32S3/ZX3D95CE01S-TR-4848) ---
// https://github.com/wireless-tag-com/ZX3D95CE01S-TR-4848
static const int I2C_SDA = 15;
static const int I2C_SCL = 6;

// --- SHT20 I2C/commands ---
#define SHT20_ADDR            0x40
#define CMD_TRIG_T_NOHOLD     0xF3
#define CMD_TRIG_RH_NOHOLD    0xF5
#define CMD_SOFT_RESET        0xFE

static bool soft_reset() {
    Wire.beginTransmission(SHT20_ADDR);
    Wire.write(CMD_SOFT_RESET);
    if(Wire.endTransmission()!=0) return false;
    delay(20); // datasheet: >= 15ms
    return true;
}

static bool read_raw(uint8_t cmd, uint16_t &raw) {
    Wire.beginTransmission(SHT20_ADDR);
    Wire.write(cmd);
    if (Wire.endTransmission() != 0) return false;

    // conversion wait: ~85ms (T), ~29ms (RH)
    if (cmd == CMD_TRIG_T_NOHOLD) delay(90);
    else                          delay(35);

    uint8_t n = Wire.requestFrom((int)SHT20_ADDR, 3);
    if (n < 2) return false;

    uint8_t msb = Wire.read();
    uint8_t lsb = Wire.read();
    uint8_t crc = (n >= 3) ? Wire.read() : 0; (void)crc;

    raw = ((uint16_t)msb << 8) | lsb;
    raw &= ~0x0003; // clear status bits
    return true;
}

namespace sht20 {
    void begin() {
        // init shared I2C bus (FT6336U touch shares same bus)
        Wire.begin(I2C_SDA,I2C_SCL);
        Wire.setClock(100000);   // 100kHz recommended
        Wire.setTimeOut(200);    // ms
        soft_reset();
        float t,h; if(read(t,h)){ G.temp_c=t; G.rh_pct=h; }
    }

    bool read(float &T, float &H) {
        uint16_t rawT = 0, rawH = 0;
        if (!read_raw(CMD_TRIG_T_NOHOLD, rawT)) return false;
        if (!read_raw(CMD_TRIG_RH_NOHOLD, rawH)) return false;

        T = -46.85f + 175.72f * (rawT / 65536.0f);
        H =  -6.00f + 125.00f * (rawH / 65536.0f);
        if (H < 0)   H = 0;
        if (H > 100) H = 100;
        return true;
    }
}