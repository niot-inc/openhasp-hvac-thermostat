    // ui_sync.cpp
    #include "ui_sync.h"
    #include "mqtt_ctrl.h"

    AppState G; // 정의

    static bool attached=false;

    lv_obj_t *ui::btn_h_minus=nullptr,*ui::btn_h_plus=nullptr,*ui::btn_h_power=nullptr;
    lv_obj_t *ui::btn_c_minus=nullptr,*ui::btn_c_plus=nullptr,*ui::btn_c_power=nullptr;

    static inline void publish_mode(bool heat) {
        if(heat) mqttc::publish_mode_state("heat", G.power_heat, G.target_heat, G.temp_c);
        else     mqttc::publish_mode_state("cool", G.power_cool, G.target_cool, G.temp_c);
    }

    void ui::sync_power(){
        if(lv_obj_t* l = hasp_find_obj_from_page_id(PAGE_MAIN, ID_STAT_HEAT)) {
            lv_label_set_text(l, MODE_STATE_STR[G.state_heat]);
            lv_obj_set_style_local_text_color(l, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT,
                (G.state_heat==MODE_OFF)? LV_COLOR_MAKE(0x6B,0x6B,0x6B) : LV_COLOR_MAKE(0xF2,0xF2,0xF7));
        }
        if(lv_obj_t* l = hasp_find_obj_from_page_id(PAGE_MAIN, ID_STAT_COOL)) {
            lv_label_set_text(l, MODE_STATE_STR[G.state_cool]);
            lv_obj_set_style_local_text_color(l, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT,
                (G.state_cool==MODE_OFF)? LV_COLOR_MAKE(0x6B,0x6B,0x6B) : LV_COLOR_MAKE(0xF2,0xF2,0xF7));
        }

        // 토글 색상은 JSONL로 val만 바꿔주면 스킨이 알아서 처리
        {
            char cmd[96];
            snprintf(cmd, sizeof(cmd),
                "jsonl {\"page\":1,\"id\":%u,\"obj\":\"btn\",\"val\":%d}",
                ID_BTN_HEAT_POWER, G.power_heat ? 1 : 0);
            dispatch_text_line(cmd, true);
        }
        {
            char cmd[96];
            snprintf(cmd, sizeof(cmd),
                "jsonl {\"page\":1,\"id\":%u,\"obj\":\"btn\",\"val\":%d}",
                ID_BTN_COOL_POWER, G.power_cool ? 1 : 0);
            dispatch_text_line(cmd, true);
        }
    }

    void ui::sync_heat(){
        int v = (int)lroundf(clampf(G.target_heat*10.0f, 150, 300));
        if(lv_obj_t* m = hasp_find_obj_from_page_id(PAGE_MAIN, ID_METER_HEAT)) lv_linemeter_set_value(m, v);
        if(lv_obj_t* l = hasp_find_obj_from_page_id(PAGE_MAIN, ID_SET_HEAT))   lv_label_set_text_fmt(l, "%.1f°C", G.target_heat);
    }

    void ui::sync_cool(){
        int v = (int)lroundf(clampf(G.target_cool*10.0f, 150, 300));
        if(lv_obj_t* m = hasp_find_obj_from_page_id(PAGE_MAIN, ID_METER_COOL)) lv_linemeter_set_value(m, v);
        if(lv_obj_t* l = hasp_find_obj_from_page_id(PAGE_MAIN, ID_SET_COOL))   lv_label_set_text_fmt(l, "%.1f°C", G.target_cool);
    }

    void ui::set_current_temp(float t){
        if(lv_obj_t* l = hasp_find_obj_from_page_id(PAGE_MAIN, ID_CUR_HEAT)) lv_label_set_text_fmt(l, "\xEE\x94\x8F %.1f°C", t);
        if(lv_obj_t* l = hasp_find_obj_from_page_id(PAGE_MAIN, ID_CUR_COOL)) lv_label_set_text_fmt(l, "\xEE\x94\x8F %.1f°C", t);
    }

    void ui::on_btn(lv_obj_t* obj, lv_event_t event){
        if(event != LV_EVENT_CLICKED) return;

        if(obj==btn_h_minus){ G.target_heat = clampf(G.target_heat-0.5f,15,30); sync_heat(); publish_mode(true); }
        else if(obj==btn_h_plus){ G.target_heat = clampf(G.target_heat+0.5f,15,30); sync_heat(); publish_mode(true); }
        else if(obj==btn_h_power){ G.power_heat=!G.power_heat; G.state_heat = G.power_heat? MODE_IDLE: MODE_OFF; sync_power(); publish_mode(true); }

        else if(obj==btn_c_minus){ G.target_cool = clampf(G.target_cool-0.5f,15,30); sync_cool(); publish_mode(false); }
        else if(obj==btn_c_plus){ G.target_cool = clampf(G.target_cool+0.5f,15,30); sync_cool(); publish_mode(false); }
        else if(obj==btn_c_power){ G.power_cool=!G.power_cool; G.state_cool = G.power_cool? MODE_IDLE: MODE_OFF; sync_power(); publish_mode(false); }
    }

    bool ui::is_ready(){ return attached; }

    void ui::attach_once(){
        if(attached) return;

        btn_h_minus = hasp_find_obj_from_page_id(PAGE_MAIN, ID_BTN_HEAT_MINUS);
        btn_h_plus  = hasp_find_obj_from_page_id(PAGE_MAIN, ID_BTN_HEAT_PLUS);
        btn_h_power = hasp_find_obj_from_page_id(PAGE_MAIN, ID_BTN_HEAT_POWER);
        btn_c_minus = hasp_find_obj_from_page_id(PAGE_MAIN, ID_BTN_COOL_MINUS);
        btn_c_plus  = hasp_find_obj_from_page_id(PAGE_MAIN, ID_BTN_COOL_PLUS);
        btn_c_power = hasp_find_obj_from_page_id(PAGE_MAIN, ID_BTN_COOL_POWER);

        if(!btn_h_minus||!btn_h_plus||!btn_h_power||!btn_c_minus||!btn_c_plus||!btn_c_power) return;

        lv_obj_set_event_cb(btn_h_minus, ui::on_btn);
        lv_obj_set_event_cb(btn_h_plus , ui::on_btn);
        lv_obj_set_event_cb(btn_h_power, ui::on_btn);
        lv_obj_set_event_cb(btn_c_minus, ui::on_btn);
        lv_obj_set_event_cb(btn_c_plus , ui::on_btn);
        lv_obj_set_event_cb(btn_c_power, ui::on_btn);

        attached = true;

        sync_heat(); sync_cool(); sync_power();
        if(!isnan(G.temp_c)) set_current_temp(G.temp_c);
    }