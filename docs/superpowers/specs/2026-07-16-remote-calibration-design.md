# 원격 온도 보정 + 장치 재부팅 설계

날짜: 2026-07-16
대상 저장소: openhasp-hvac-thermostat (펌웨어), superb-backend (백엔드+어드민)

## 배경 / 목적

각 방 ESP32 온도조절기의 온도 보정값(`temp_offset`)은 장치 littlefs의 `/superb.json`에
저장되며, 현재 바꾸려면 장치 웹 파일에디터로 JSON을 수정하고 재부팅해야 한다.
현장 정리 과정(내장 SHT20용 잔재 오프셋 −12/−14가 외장 SHT30 전환 후 과보정이 된 상황)에서
보정 조정이 빈번해져, superb-backend 웹 어드민에서 바로 수정할 수 있게 한다.
추가로 MQTT로 장치 재부팅도 어드민에서 가능하게 한다.

핵심 발견 2가지(구현 최소화 근거):
- 펌웨어에 `sht20/sht30::set_offsets(temp, rh, persist=true)`가 **이미 구현**되어 있으나
  호출처가 없음 → MQTT 명령만 배선하면 됨.
- openHASP 코어에 `reboot` 명령이 **이미 존재** (`dispatch_add_command("reboot")` →
  `haspDevice.reboot()`, 설정 저장 후 재부팅) → 재부팅은 펌웨어 수정 0줄.

## 결정 사항

| 결정 | 선택 | 근거 |
|---|---|---|
| 입력 방식 | 오프셋 숫자 직접 입력 | 사용자 선택 |
| 편집 범위 | 온도만 (`rh_offset`은 현 값 유지) | 지금 아픈 문제에 집중. 명령 JSON이라 향후 필드 추가 용이 |
| UI | 센서 상태 표의 '보정' 칸 인라인 편집 + 행별 재부팅 버튼 | 상태 보며 바로 조정, 추가 화면 없음 |
| 보정 원본(source of truth) | **A안: 장치가 원본** (펌웨어가 /superb.json에 저장) | set_offsets 재사용, 백엔드 저장소 불필요. 트레이드오프: 전체 플래시/장치 교체 시 소실(OTA는 안전). 교체 잦아지면 B안(config 원본+동기화)으로 이전 가능 |

## 데이터 흐름

보정 변경:
```
어드민 보정칸 입력 → POST /sensor-health/:hostname/offset {tempOffset: -3}
→ MqttService.publish("hasp/<host>/command/custom/calibrate", {"temp_offset": -3})
→ 펌웨어 handle_inbound → apply_temp_offset(-3)
   → 활성 센서 set_offsets(-3, 기존rh, persist=true) → /superb.json 저장
→ 다음 sensor_health 발행(≤5초)에 새 temp_offset → 어드민 표 갱신 = 적용 확인
```

재부팅:
```
어드민 재부팅 버튼(확인 대화상자) → POST /sensor-health/:hostname/reboot
→ MqttService.publish("hasp/<host>/command/reboot", "")
→ openHASP 코어가 설정 저장 후 재부팅 (펌웨어 수정 없음)
```

## 구성요소별 변경

### 펌웨어 (src/custom/)
- `my_custom.cpp`: `bool apply_temp_offset(float)` 제공 — 활성 센서(`g_sensor`)를 아는 쪽이
  소유. 활성 드라이버의 `get_offsets`로 기존 rh를 읽고 `set_offsets(new_t, rh, true)` 호출.
  SENSOR_NONE이면 false.
- `mqtt_ctrl.cpp`: `handle_inbound`에 `calibrate` 토픽 분기 — JSON `temp_offset` 파싱,
  범위(−30~+30) 검증 후 `apply_temp_offset` 호출. 헤더(`my_custom` 함수 선언) 노출 필요.

### 백엔드 (src/sensor-health/)
- `sensor-health.service.ts`: `setTempOffset(hostname, v)` / `rebootDevice(hostname)` —
  config에 존재하는 esp32 hostname인지 검증 후 MqttService.publish.
- `sensor-health.controller.ts`: `POST :hostname/offset` (body `{tempOffset:number}`),
  `POST :hostname/reboot`. 모듈 변경 없음.

### 어드민 (public/admin/)
- `renderSensorStatus`: 보정 칸을 숫자 input(step 0.5) + 저장 버튼으로, 행 끝에 재부팅 버튼.
- **이벤트 위임**: 표가 15초 폴링으로 통째 재렌더되므로 `#sensor-status-body`에 위임 리스너 1개.
- **폴링 가드**: 표 내부에 포커스된 input이 있으면 해당 주기 재렌더 건너뜀 (입력 덮어쓰기 방지).
- 비활성 조건: `offline` → 둘 다 비활성 / `legacy`(구형 펌웨어) → 보정만 비활성
  (calibrate 명령 미지원, 재부팅은 코어 기능이라 동작).

## 에러 처리

- 백엔드: 없는 hostname → 404, 범위 밖(|v|>30) → 400 (클램프 대신 거부).
- 펌웨어: 파싱 실패/범위 밖 → 무시 + LOG_ERROR (크래시 금지).
- MQTT fire-and-forget: API 응답은 "전송됨". **실제 적용 확인은 sensor_health의
  temp_offset 변화 관측**(5초 내). 어드민은 저장 직후 "전송됨, 곧 반영" 토스트.
- 재부팅: 확인 대화상자 필수.

## 검증

1. 펌웨어 CI 빌드 → 검증 장치 1대 OTA.
2. mosquitto로 calibrate 발행 → sensor_health.temp_offset 변화 확인 → 장치 재부팅 후 유지 확인.
3. 백엔드 tsc/eslint + 404/400/정상 경로.
4. 어드민: 입력 중 폴링 덮어쓰기 없음 / 저장→5초 내 표 반영 / 재부팅 확인창 / legacy·offline 비활성.
5. 통합 실사용: room01의 −12를 웹에서 0으로 변경해 실온 정상화. 재부팅 버튼으로
   room02 SHT30 수리 후 복구 시나리오 대비.

## 범위 밖 (명시)

- 습도 보정(rh_offset) UI — 추후 동일 패턴으로 확장 가능.
- B안(백엔드 원본 + 온라인 시 동기화) — 장치 교체가 잦아지면 재검토.
- 펌웨어 명령 ACK 토픽 — sensor_health 관측으로 충분하다고 판단.
