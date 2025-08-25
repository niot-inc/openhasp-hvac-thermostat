/* MIT License - Copyright (c) 2019-2024 Francis Van Roie
   For full license information read the LICENSE file in the project folder */

// USAGE: - Copy this file and rename it to my_custom.cpp
//        - Change false to true on line 9

#include "hasplib.h"

#if defined(HASP_USE_CUSTOM) && true // <-- set this to true in your code

#include "hasp_debug.h"

#include "state.h"
#include "sht20.h"
#include "ui_sync.h"
#include "mqtt_ctrl.h"

static bool g_need_initial_publish = false;
static uint32_t g_publish_after_ms = 0;

static String g_device_name;

// ========= Get device name helpers =========
String get_device_name_from_config() {
  if (g_device_name.length()) return g_device_name;

  // FS 마운트 시도 (이미 마운트되어 있어도 OK)
  LittleFS.begin();

  File f = LittleFS.open("/config.json", "r");
  if (!f) {
    g_device_name = "openhasp";    // ★ safe fallback
    return g_device_name;
  }

  // 파일 크기만큼 읽어서 파싱
  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (err) {
    g_device_name = "openhasp";    // ★ fallback
    return g_device_name;
  }

  // 우선순위: hasp.name -> mqtt.name -> hasp.node -> mqtt.node
  const char* v = nullptr;
  if (doc["hasp"]["name"].is<const char*>())      v = doc["hasp"]["name"];
  else if (doc["mqtt"]["name"].is<const char*>()) v = doc["mqtt"]["name"];
  else if (doc["hasp"]["node"].is<const char*>()) v = doc["hasp"]["node"];
  else if (doc["mqtt"]["node"].is<const char*>()) v = doc["mqtt"]["node"];

  g_device_name = (v && *v) ? String(v) : String("openhasp"); // ★ fallback
  return g_device_name;
}

void custom_setup()
{
    LOG_INFO(TAG_CUSTOM, "custom_setup() entered");
    sht20::begin();
    LOG_INFO(TAG_CUSTOM, "SHT20 warm-up T=%.2f H=%.1f", G.temp_c, G.rh_pct);
}

// 5초마다 직접 MQTT publish (debug.tele과 무관)
static unsigned long lastPublish = 0;
void custom_loop()
{
    static unsigned long last=0; unsigned long now=millis();
    if(now-last>=5000){
        last=now;
        StaticJsonDocument<256> d;
        if(!isnan(G.temp_c)) d["temperature"]=round(G.temp_c*10)/10.0;
        if(!isnan(G.rh_pct)) d["humidity"]=round(G.rh_pct*10)/10.0;
        String dev=get_device_name_from_config(); char topic[128];
        snprintf(topic,sizeof(topic),"hasp/%s/state/sensors",dev.c_str());
        char payload[256]; size_t n=serializeJson(d,payload,sizeof(payload));
        mqttPublish(topic,payload,n,false);
    }
}

void custom_every_second()
{
    ui::attach_once();

    // MQTT 초기 퍼블리시 스케줄 처리
    if(g_need_initial_publish && millis() > g_publish_after_ms) {
        if(ui::is_ready()) { // attach_once() 완료 확인
            LOG_INFO(TAG_CUSTOM, "Initial publish_all after MQTT connect");
            mqttc::publish_all();
            g_need_initial_publish = false;
        }
        // 아직 UI 미준비면 다음 초에 다시 체크
    }
}

void custom_every_5seconds()
{
    // measure every 5s (keep bus usage modest; touch shares the bus)
    float t,h;
    if(sht20::read(t,h)){
        G.temp_c=t; G.rh_pct=h;
        LOG_DEBUG(TAG_CUSTOM, "SHT20 T=%.2fC H=%.1f%%", G.temp_c, G.rh_pct);
        ui::set_current_temp(G.temp_c);
    }else{
        LOG_ERROR(TAG_CUSTOM, "SHT20 read failed");
    }
}

bool custom_pin_in_use(uint8_t pin)
{
    // openHASP에게 "이 핀은 내가 쓰는 중"이라고 알려줌
    // switch (pin) {
    //     case I2C_SDA:
    //     case I2C_SCL:
    //     return true;
    //     default:
    //     return false;
    // }
    switch(pin){ case 15: case 6: return true; default: return false; }
}

// MQTT 기본 tele 경로는 사용 안 함 (loop에서 직접 발행하므로 비워둠)
void custom_get_sensors(JsonDocument& doc)
{
    // unused
}

void custom_topic_payload(const char* topic, const char* payload, uint8_t source)
{
    mqttc::handle_inbound(topic,payload,source);
}

void custom_state_subtopic(const char* subtopic, const char* payload){
    // MQTT 연결 직후 코어가 보내는 상태 토픽
    if(subtopic && strcmp(subtopic, "statusupdate") == 0) {
        // UI가 아직일 수 있으니, 1~2초 뒤에 1회만 퍼블리시
        g_need_initial_publish = true;
        g_publish_after_ms = millis() + 1500;   // 1.5s 여유
        LOG_INFO(TAG_CUSTOM, "Seen 'statusupdate' -> schedule initial publish");
    }
}

#endif // HASP_USE_CUSTOM