// mqtt_ctrl.cpp
#include "mqtt_ctrl.h"
#include "hasplib.h"   // mqttPublish, LOG_* 사용
#include <ArduinoJson.h>
#include "ui_sync.h"

static bool topic_ends_with(const char* topic, const char* suffix){
    size_t lt=strlen(topic), ls=strlen(suffix);
    return lt>=ls && strcasecmp(topic+lt-ls, suffix)==0;
}

String get_device_name_from_config(); // state.cpp 대신 my_custom에 이미 구현되어 있으면 링크됨

namespace mqttc {

    void publish_mode_state(const char* mode, bool power, float setpt, float current){
        StaticJsonDocument<160> d;
        d["mode"]=mode; d["power"]=power;
        d["setpoint"]= round(setpt*10)/10.0;
        if(!isnan(current)) d["current"]= round(current*10)/10.0;

        uint16_t label_id = (strcmp(mode,"heat")==0) ? ID_STAT_HEAT : ID_STAT_COOL;
        if(lv_obj_t* l = hasp_find_obj_from_page_id(PAGE_MAIN, label_id)) {
            d["status"] = lv_label_get_text(l);
        }

        char payload[160]; size_t n = serializeJson(d, payload, sizeof(payload));
        String dev = get_device_name_from_config();
        char topic[128]; snprintf(topic,sizeof(topic),"hasp/%s/state/%s", dev.c_str(), mode);
        mqttPublish(topic, payload, n, true);
    }

    void publish_all(){
        publish_mode_state("heat", G.power_heat, G.target_heat, G.temp_c);
        publish_mode_state("cool", G.power_cool, G.target_cool, G.temp_c);
    }

    void handle_inbound(const char* topic, const char* payload, uint8_t source){
        LOG_INFO(TAG_CUSTOM, "MQTT RX topic=%s payload=%s src=%u",
                topic?topic:"", payload?payload:"", source);

        // 1) 현재 상태 요청 => 상태 2건 그대로 publish
        if( topic_ends_with(topic, "get/hvac") || topic_ends_with(topic, "custom/get/hvac") )
        {
            // payload는 무시. 현재 값을 그대로 publish
            publish_mode_state("heat", G.power_heat, G.target_heat, G.temp_c);
            publish_mode_state("cool", G.power_cool, G.target_cool, G.temp_c);
            return;
        }



        if(!topic||!payload) return;

        StaticJsonDocument<256> doc;
        if(deserializeJson(doc, payload)) return;

        auto apply = [&](bool& power, float& sp, ModeState& st,
                     uint16_t label_id, void(*sync)(), const char* name){
            bool changed=false;
            if(doc.containsKey("setpoint")){
                float v = clampf(doc["setpoint"].as<float>(), 15.0f, 30.0f);
                if(fabsf(sp - v) > 0.01f){ sp=v; if(sync) sync(); changed=true; }
            }
            if(doc.containsKey("power")){
                bool p = doc["power"].as<bool>();
                if(power != p){ power=p; st = p? MODE_IDLE: MODE_OFF; ui::sync_power(); changed=true; }
            }
            if(doc.containsKey("status")){
                const char* txt = doc["status"];
                if(lv_obj_t* l = hasp_find_obj_from_page_id(PAGE_MAIN, label_id)) {
                    lv_label_set_text(l, txt);
                }
                changed = true;
            }
            if(changed) publish_mode_state(name, power, sp, G.temp_c);
        };

        if(topic_ends_with(topic, "heat")) { apply(G.power_heat, G.target_heat, G.state_heat, ID_STAT_HEAT, ui::sync_heat, "heat"); }
        else if(topic_ends_with(topic, "cool")) { apply(G.power_cool, G.target_cool, G.state_cool, ID_STAT_COOL, ui::sync_cool, "cool"); }
    }
}