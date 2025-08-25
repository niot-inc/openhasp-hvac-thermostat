// ui_sync.h
#pragma once
#include "state.h"
#include "hasplib.h" // openHASP 제공, LVGL v7 포함

namespace ui {

    // 버튼 핸들 (v7: 콜백 비교용)
    extern lv_obj_t *btn_h_minus, *btn_h_plus, *btn_h_power;
    extern lv_obj_t *btn_c_minus, *btn_c_plus, *btn_c_power;

    // 초기 1회: 버튼 포인터 획득 + 이벤트 연결 + UI 초기 반영
    void attach_once();
    bool is_ready();

    // 라벨/미터 동기화
    void sync_heat();
    void sync_cool();
    void sync_power();           // 113/213 상태 텍스트/색
    void set_current_temp(float t);

    // 버튼 콜백 (v7 시그니처)
    void on_btn(lv_obj_t* obj, lv_event_t event);
}