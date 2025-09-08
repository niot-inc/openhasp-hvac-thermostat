#pragma once
#include "state.h"

namespace sht30 {
    void begin();                  // I2C init + soft reset + warm-up read
    bool read(float &t, float &h); // single-shot measurement

    void set_offsets(float temp_offset, float rh_offset, bool persist=false);
    void get_offsets(float &temp_offset, float &rh_offset);
}