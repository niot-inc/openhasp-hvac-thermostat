#pragma once
#include <Arduino.h>
#include <math.h>

// LVGL v7 object ids (네 UI JSON과 일치)
enum : uint16_t {
    PAGE_MAIN = 1,
    ID_METER_HEAT = 110, ID_SET_HEAT = 111, ID_CUR_HEAT = 112, ID_STAT_HEAT = 113,
    ID_BTN_HEAT_MINUS = 120, ID_BTN_HEAT_PLUS = 121, ID_BTN_HEAT_POWER = 125,

    ID_METER_COOL = 210, ID_SET_COOL = 211, ID_CUR_COOL = 212, ID_STAT_COOL = 213,
    ID_BTN_COOL_MINUS = 220, ID_BTN_COOL_PLUS = 221, ID_BTN_COOL_POWER = 225
};

enum ModeState { MODE_OFF = 0, MODE_IDLE, MODE_RUNNING };
static inline const char* MODE_STATE_STR[] = {"OFF","IDLE","RUNNING"};

struct AppState {
    // sensor
    float temp_c = NAN;
    float rh_pct = NAN;

    // hvac
    float target_heat = 24.0f;
    float target_cool = 24.0f;
    bool  power_heat  = false;
    bool  power_cool  = false;
    ModeState state_heat = MODE_OFF;
    ModeState state_cool = MODE_OFF;
};

extern AppState G;

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// device name (config.json에서)
String get_device_name_from_config();