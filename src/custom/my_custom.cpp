/* MIT License - Copyright (c) 2019-2024 Francis Van Roie
   For full license information read the LICENSE file in the project folder */

// USAGE: - Copy this file and rename it to my_custom.cpp
//        - Change false to true on line 9

#include "hasplib.h"

#if defined(HASP_USE_CUSTOM) && true // <-- set this to true in your code

#include "hasp_debug.h"

#include <Wire.h>
#include "state.h"
#include "sht20.h"
#include "sht30.h"
#include "ui_sync.h"
#include "mqtt_ctrl.h"

static bool g_need_initial_publish = false;
static uint32_t g_publish_after_ms = 0;

static String g_device_name;

// ========= 센서 자동 선택 =========
// 이 보드엔 내장 SHT20(0x40, 기판 발열로 부정확)이 있고, 정확도를 위해 외장 SHT30(0x44)을
// 추가한다. 부팅 시 버스를 스캔해 SHT30을 "우선" 선택하고, 없으면 SHT20으로 폴백하되
// fallback(부정확) 임을 명시한다. 한 펌웨어로 모든 보드가 동작한다.
static const int SENSOR_SDA = 15, SENSOR_SCL = 6;
enum SensorKind { SENSOR_NONE = 0, SENSOR_SHT30, SENSOR_SHT20 };
static SensorKind g_sensor = SENSOR_NONE;
static const char* sensor_name(SensorKind k){
    return k==SENSOR_SHT30 ? "sht30" : k==SENSOR_SHT20 ? "sht20" : "none";
}
static bool sensor_is_fallback(){ return g_sensor == SENSOR_SHT20; } // 내장=부정확 폴백

// 진단/상태
static int    g_read_fails = 0;   // 연속 read 실패 횟수
static String g_i2c_scan   = "";  // 마지막 I2C 버스 스캔 결과
static bool   g_last_ok    = false;

// SHT30이 주소는 ACK하나 측정 읽기가 계속 실패(마진 불량)하는 경우, 이만큼 연속 실패하면
// 내장 SHT20으로 폴백한다(부정확하지만 0/무표시보단 낫게). 재부팅 전까지 SHT20 유지.
// 정상 SHT30은 실패가 없어 발동하지 않는다.
static const int FALLBACK_AFTER = 4;  // 4회 = 약 20초

static bool i2c_present(uint8_t addr){
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

// 버스 전역 스캔 (센서 유무/종류 원격 진단용). 버스는 터치와 공유 → 잦은 스캔 지양.
static String i2c_scan(){
    String s;
    for(uint8_t a = 0x08; a <= 0x77; a++){
        Wire.beginTransmission(a);
        if(Wire.endTransmission() == 0){
            s += "0x"; if(a < 16) s += "0"; s += String(a, HEX); s += " ";
        }
    }
    s.trim();
    return s;
}

// 버스 스캔 → 센서 종류 결정 → 해당 드라이버 begin(). 재감지 시에도 재사용.
static void detect_and_begin(){
    Wire.begin(SENSOR_SDA, SENSOR_SCL);
    Wire.setClock(50000);
    delay(5);
    if(i2c_present(0x44))      { g_sensor = SENSOR_SHT30; sht30::begin(); }
    else if(i2c_present(0x40)) { g_sensor = SENSOR_SHT20; sht20::begin(); }
    else                       { g_sensor = SENSOR_NONE; }
    g_i2c_scan = i2c_scan();
    LOG_INFO(TAG_CUSTOM, "sensor detected: %s%s [%s]", sensor_name(g_sensor),
             sensor_is_fallback() ? " (fallback/inaccurate)" : "", g_i2c_scan.c_str());
}

static bool sensor_read(float &t, float &h){
    if(g_sensor == SENSOR_SHT30) return sht30::read(t,h);
    if(g_sensor == SENSOR_SHT20) return sht20::read(t,h);
    return false;
}

// 현재 활성 센서에 적용 중인 온도 보정값(/superb.json). 어드민에서 보정 확인용.
static float active_temp_offset(){
    float t = 0.0f, h = 0.0f;
    if(g_sensor == SENSOR_SHT30) sht30::get_offsets(t, h);
    else if(g_sensor == SENSOR_SHT20) sht20::get_offsets(t, h);
    return t;
}

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
    detect_and_begin();
    LOG_INFO(TAG_CUSTOM, "%s warm-up T=%.2f H=%.1f", sensor_name(g_sensor), G.temp_c, G.rh_pct);
}

// 5초마다 직접 MQTT publish (debug.tele과 무관)
static unsigned long lastPublish = 0;
void custom_loop()
{
    static unsigned long last=0; unsigned long now=millis();
    if(now-last>=5000){
        last=now;
        String dev=get_device_name_from_config();

        StaticJsonDocument<256> d;
        if(!isnan(G.temp_c)) d["temperature"]=round(G.temp_c*10)/10.0;
        if(!isnan(G.rh_pct)) d["humidity"]=round(G.rh_pct*10)/10.0;
        d["sensor"]=sensor_name(g_sensor);           // 어떤 센서로 잰 값인지
        if(sensor_is_fallback()) d["fallback"]=true; // 내장 SHT20 폴백 = 부정확
        char topic[128];
        snprintf(topic,sizeof(topic),"hasp/%s/state/sensors",dev.c_str());
        char payload[256]; size_t n=serializeJson(d,payload,sizeof(payload));
        mqttPublish(topic,payload,n,false);

        // 센서 상태(retained): 새 구독자가 접속 즉시 방별 센서 건강을 확인 → 원격 진단 용이
        StaticJsonDocument<256> hb;
        hb["sensor"]=sensor_name(g_sensor);          // sht30 / sht20 / none
        hb["fallback"]=sensor_is_fallback();
        hb["read_ok"]=g_last_ok;
        hb["fails"]=g_read_fails;
        hb["i2c"]=g_i2c_scan;                        // 0x44=SHT30, 0x40=SHT20, 0x38=touch
        hb["temp_offset"]=active_temp_offset();      // 적용 중인 온도 보정값(도)
        char htopic[128];
        snprintf(htopic,sizeof(htopic),"hasp/%s/state/sensor_health",dev.c_str());
        char hpay[256]; size_t hn=serializeJson(hb,hpay,sizeof(hpay));
        mqttPublish(htopic,hpay,hn,true);            // retained
    }
}

void custom_every_second()
{
    ui::attach_once();

    // NOTE: 부팅 후 이렇게 setpoint, power 기본값을 퍼블리시 하면
    // 이 메시지를 수신하여 디스크에 저장하는 SuperB 에선 상태가 꼬일 수 있다
    // 그러므로 메시지를 보내지 않는다

    // MQTT 초기 퍼블리시 스케줄 처리
    // if(g_need_initial_publish && millis() > g_publish_after_ms) {
    //     if(ui::is_ready()) { // attach_once() 완료 확인
    //         LOG_INFO(TAG_CUSTOM, "Initial publish_all after MQTT connect");
    //         mqttc::publish_all();
    //         g_need_initial_publish = false;
    //     }
    //     // 아직 UI 미준비면 다음 초에 다시 체크
    // }
}

void custom_every_5seconds()
{
    // measure every 5s (keep bus usage modest; touch shares the bus)
    float t,h;
    if(sensor_read(t,h)){
        G.temp_c=t; G.rh_pct=h;
        g_read_fails=0; g_last_ok=true;
        LOG_DEBUG(TAG_CUSTOM, "%s T=%.2fC H=%.1f%%", sensor_name(g_sensor), G.temp_c, G.rh_pct);
        ui::set_current_temp(G.temp_c);
    }else{
        g_read_fails++; g_last_ok=false;
        LOG_ERROR(TAG_CUSTOM, "%s read failed (#%d)", sensor_name(g_sensor), g_read_fails);
        g_i2c_scan = i2c_scan();
        if(g_sensor == SENSOR_SHT30 && g_read_fails >= FALLBACK_AFTER && i2c_present(0x40)){
            // 불량 SHT30(응답은 하나 못 읽음) → 내장 SHT20으로 폴백. 값이라도 나오게.
            // 재부팅 전까지 SHT20 유지. 어드민엔 fallback=true + '점검 필요'로 계속 표시됨.
            LOG_INFO(TAG_CUSTOM, "SHT30 unreadable, falling back to built-in SHT20");
            g_sensor = SENSOR_SHT20; sht20::begin(); g_read_fails = 0;
        }else if(g_sensor == SENSOR_NONE && g_read_fails % 3 == 0){
            // 센서가 아예 없던 상태 → 재탐색(나중에 연결되면 흡수)
            detect_and_begin();
        }
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