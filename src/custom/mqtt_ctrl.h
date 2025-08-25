// mqtt_ctrl.h
#pragma once
#include "state.h"

namespace mqttc {
    void publish_mode_state(const char* mode, bool power, float setpt, float current /*NaN 허용*/);
    void publish_all();
    void handle_inbound(const char* topic, const char* payload, uint8_t source);
}