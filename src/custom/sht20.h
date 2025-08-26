// sht20.h
#pragma once
#include "state.h"

namespace sht20 {
    void begin();                  // I2C init + soft reset + warm-up read
    bool read(float &t, float &h); // 측정 1회 (No-Hold)

    void set_offsets(float temp_offset, float rh_offset, bool persist=false);
    void get_offsets(float &temp_offset, float &rh_offset);
}