# 원격 온도 보정 + 장치 재부팅 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** superb-backend 웹 어드민에서 각 방 ESP32의 온도 보정값을 MQTT로 수정하고 장치를 재부팅할 수 있게 한다.

**Architecture:** 장치가 보정값의 원본(A안). 어드민 → 백엔드 REST → MQTT `command/custom/calibrate` → 펌웨어가 `/superb.json`에 저장 → `sensor_health.temp_offset` 재발행으로 적용 확인. 재부팅은 openHASP 코어의 기존 `command/reboot` 사용(펌웨어 수정 0줄).

**Tech Stack:** Arduino C++ (openHASP 0.7.0-rc13 오버레이), NestJS + mqtt v5, 순수 브라우저 JS + Tailwind.

**스펙:** `docs/superpowers/specs/2026-07-16-remote-calibration-design.md`

## Global Constraints

- 두 저장소에 걸침: 펌웨어 `~/Projects/openhasp-hvac-thermostat`, 백엔드+어드민 `~/Projects/superb-backend`. 각 저장소에서 feature 브랜치로 작업, PR 생성까지만 (머지는 사용자).
- 보정 범위: **−30.0 ~ +30.0** (초과 시 거부, 클램프 금지). 온도만(rh_offset은 기존 값 유지).
- MQTT 토픽 계약: 보정 `hasp/<hostname>/command/custom/calibrate` 페이로드 `{"temp_offset": <number>}` / 재부팅 `hasp/<hostname>/command/reboot` 페이로드 빈 문자열.
- 어드민 표는 15초 폴링 재렌더 → 이벤트는 위임으로, 입력 중(focus) 재렌더 금지.
- 어드민 섹션은 `<form id="config-form">` 내부 → 모든 버튼 `type="button"` 필수(폼 제출 방지).
- 펌웨어는 로컬 빌드 불가 → 컴파일 검증은 CI(`gh workflow run build.yml --ref <branch> -f deploy=false`).

---

### Task 1: 펌웨어 — calibrate MQTT 명령

**Files:**
- Modify: `~/Projects/openhasp-hvac-thermostat/src/custom/my_custom.cpp` (apply_temp_offset 추가; `active_temp_offset()` 함수 근처, ~L86)
- Modify: `~/Projects/openhasp-hvac-thermostat/src/custom/mqtt_ctrl.cpp` (handle_inbound에 calibrate 분기)

**Interfaces:**
- Produces: `bool custom_apply_temp_offset(float temp_offset)` — 활성 센서에 보정 적용+영속화. SENSOR_NONE이면 false. mqtt_ctrl.cpp에서 extern 선언으로 사용 (기존 `get_device_name_from_config()` 패턴, mqtt_ctrl.cpp:12).
- Produces: MQTT 계약 — `.../command/custom/calibrate` + `{"temp_offset":N}` 수신 시 적용. (Task 2의 백엔드가 이 계약으로 발행)

- [ ] **Step 1: 브랜치 생성**

```bash
cd ~/Projects/openhasp-hvac-thermostat && git checkout main && git pull && git checkout -b feat/remote-calibration
```

- [ ] **Step 2: my_custom.cpp에 custom_apply_temp_offset 구현**

`active_temp_offset()` 함수 정의 바로 아래에 추가:

```cpp
// MQTT calibrate 명령용: 활성 센서의 온도 보정을 적용하고 /superb.json에 저장.
// rh_offset은 기존 값 유지. 센서 없으면 false.
bool custom_apply_temp_offset(float temp_offset){
    float t = 0.0f, h = 0.0f;
    if(g_sensor == SENSOR_SHT30){
        sht30::get_offsets(t, h);
        sht30::set_offsets(temp_offset, h, true);
    }else if(g_sensor == SENSOR_SHT20){
        sht20::get_offsets(t, h);
        sht20::set_offsets(temp_offset, h, true);
    }else{
        return false;
    }
    LOG_INFO(TAG_CUSTOM, "temp_offset %.1f -> %.1f (persisted)", t, temp_offset);
    return true;
}
```

- [ ] **Step 3: mqtt_ctrl.cpp에 calibrate 분기 추가**

파일 상단 `String get_device_name_from_config();` (L12) 아래에 extern 선언:

```cpp
bool custom_apply_temp_offset(float temp_offset); // my_custom.cpp
```

`handle_inbound` 안, `deserializeJson(doc, payload)` 성공 직후 / `apply` 람다 정의 이전에:

```cpp
        // 온도 보정 명령: hasp/<dev>/command/custom/calibrate {"temp_offset":-3}
        if(topic_ends_with(topic, "calibrate")){
            if(!doc.containsKey("temp_offset")){
                LOG_ERROR(TAG_CUSTOM, "calibrate: temp_offset missing");
                return;
            }
            float v = doc["temp_offset"].as<float>();
            if(isnan(v) || v < -30.0f || v > 30.0f){
                LOG_ERROR(TAG_CUSTOM, "calibrate: out of range %.1f", v);
                return;
            }
            if(!custom_apply_temp_offset(v)){
                LOG_ERROR(TAG_CUSTOM, "calibrate: no active sensor");
            }
            return;
        }
```

- [ ] **Step 4: 커밋 + CI 컴파일 검증**

```bash
git add src/custom/my_custom.cpp src/custom/mqtt_ctrl.cpp
git commit -m "feat: MQTT calibrate 명령으로 온도 보정 원격 설정

hasp/<dev>/command/custom/calibrate {\"temp_offset\":N} 수신 시 활성 센서에
보정 적용 + /superb.json 영속화. 범위 -30~+30 밖은 거부. rh_offset 유지.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
git push -u origin feat/remote-calibration
gh workflow run build.yml --ref feat/remote-calibration -f deploy=false
# gh run list --workflow build.yml --branch feat/remote-calibration 로 완료 대기
```
Expected: CI `success`. 실패 시 로그 보고 수정 후 재푸시.

---

### Task 2: 백엔드 — SensorHealthService에 setTempOffset / rebootDevice (TDD)

**Files:**
- Create: `~/Projects/superb-backend/src/sensor-health/sensor-health.service.spec.ts`
- Modify: `~/Projects/superb-backend/src/sensor-health/sensor-health.service.ts`

**Interfaces:**
- Consumes: `MqttService.publish(topic: string, message: string, options?)` (기존), `ThermostatConfigService.getThermostatItemConfigs()` (기존)
- Produces: `setTempOffset(hostname: string, tempOffset: number): void` — 미등록 hostname이면 `NotFoundException`, |v|>30 또는 비유한수면 `BadRequestException`, 정상이면 calibrate 토픽 publish
- Produces: `rebootDevice(hostname: string): void` — 미등록이면 `NotFoundException`, 정상이면 reboot 토픽 publish

- [ ] **Step 1: 브랜치 생성**

```bash
cd ~/Projects/superb-backend && git checkout main && git pull && git checkout -b feat/remote-calibration
```

- [ ] **Step 2: 실패하는 테스트 작성** (`sensor-health.service.spec.ts` 신규 — 이 저장소 첫 유닛 spec)

```ts
import { BadRequestException, NotFoundException } from '@nestjs/common';
import { SensorHealthService } from './sensor-health.service';

describe('SensorHealthService commands', () => {
  let service: SensorHealthService;
  let publish: jest.Mock;

  beforeEach(() => {
    publish = jest.fn();
    const mqtt = { publish, subscribe: jest.fn() };
    const config = {
      getThermostatItemConfigs: jest.fn().mockReturnValue([
        {
          id: 'room01-heating',
          displayName: '1F 난방',
          type: 'esp32-s3',
          mqtt: { hostname: 'room01' },
        },
        { id: 'heating', displayName: '난방', type: 'built-in' },
      ]),
    };
    service = new SensorHealthService(mqtt as never, config as never);
  });

  it('publishes calibrate command for known room', () => {
    service.setTempOffset('room01', -3);
    expect(publish).toHaveBeenCalledWith(
      'hasp/room01/command/custom/calibrate',
      JSON.stringify({ temp_offset: -3 }),
    );
  });

  it('rejects unknown hostname', () => {
    expect(() => service.setTempOffset('nope', 0)).toThrow(NotFoundException);
    expect(publish).not.toHaveBeenCalled();
  });

  it.each([31, -30.5, NaN, Infinity])('rejects out-of-range %p', (v) => {
    expect(() => service.setTempOffset('room01', v as number)).toThrow(
      BadRequestException,
    );
    expect(publish).not.toHaveBeenCalled();
  });

  it('publishes reboot command for known room', () => {
    service.rebootDevice('room01');
    expect(publish).toHaveBeenCalledWith('hasp/room01/command/reboot', '');
  });

  it('reboot rejects unknown hostname', () => {
    expect(() => service.rebootDevice('nope')).toThrow(NotFoundException);
  });
});
```

- [ ] **Step 3: 실패 확인**

Run: `npm test -- sensor-health.service`
Expected: FAIL — `setTempOffset is not a function`

- [ ] **Step 4: 최소 구현** (`sensor-health.service.ts`의 `getReport()` 위에 추가)

```ts
  /** 어드민에서 보정값 원격 설정. 장치가 /superb.json에 저장하고 sensor_health로 재보고한다. */
  setTempOffset(hostname: string, tempOffset: number): void {
    this.assertKnownRoom(hostname);
    if (!Number.isFinite(tempOffset) || Math.abs(tempOffset) > 30) {
      throw new BadRequestException(
        `tempOffset must be within ±30 (got ${tempOffset})`,
      );
    }
    this.mqttService.publish(
      `hasp/${hostname}/command/custom/calibrate`,
      JSON.stringify({ temp_offset: tempOffset }),
    );
    this.logger.log(`Calibrate ${hostname}: temp_offset=${tempOffset}`);
  }

  /** 장치 원격 재부팅 (openHASP 코어 reboot 명령). */
  rebootDevice(hostname: string): void {
    this.assertKnownRoom(hostname);
    this.mqttService.publish(`hasp/${hostname}/command/reboot`, '');
    this.logger.log(`Reboot requested: ${hostname}`);
  }

  private assertKnownRoom(hostname: string): void {
    if (!this.enumerateRooms().has(hostname)) {
      throw new NotFoundException(`Unknown esp32 room: ${hostname}`);
    }
  }
```

import 줄 수정: `import { BadRequestException, Injectable, Logger, NotFoundException, OnApplicationBootstrap } from '@nestjs/common';`

- [ ] **Step 5: 통과 확인**

Run: `npm test -- sensor-health.service`
Expected: PASS (5 tests)

- [ ] **Step 6: 커밋**

```bash
npx eslint src/sensor-health/ --fix && npx tsc --noEmit -p tsconfig.json
git add src/sensor-health/sensor-health.service.ts src/sensor-health/sensor-health.service.spec.ts
git commit -m "feat(sensor-health): setTempOffset/rebootDevice MQTT 명령 (TDD)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: 백엔드 — REST 엔드포인트

**Files:**
- Modify: `~/Projects/superb-backend/src/sensor-health/sensor-health.controller.ts`

**Interfaces:**
- Consumes: Task 2의 `setTempOffset` / `rebootDevice`
- Produces: `POST /sensor-health/:hostname/offset` body `{"tempOffset": number}` → 200 `{"sent": true}` / `POST /sensor-health/:hostname/reboot` → 200 `{"sent": true}`. (Task 4 어드민이 이 경로 사용)

- [ ] **Step 1: 컨트롤러에 엔드포인트 추가** (파일 전체 교체)

```ts
import { Body, Controller, Get, Param, Post } from '@nestjs/common';
import { ApiOperation, ApiProperty, ApiTags } from '@nestjs/swagger';
import { RoomSensorStatus, SensorHealthService } from './sensor-health.service';

class SetTempOffsetDto {
  @ApiProperty({ description: '온도 보정값(도), -30 ~ +30', example: -3 })
  tempOffset: number;
}

@ApiTags('sensor-health')
@Controller('sensor-health')
export class SensorHealthController {
  constructor(private readonly sensorHealthService: SensorHealthService) {}

  /** 방별 센서(SHT30/SHT20) 상태 리포트. 어드민 "센서 상태" 뷰에서 사용. */
  @Get()
  @ApiOperation({
    summary: '방별 센서 상태 조회',
    description:
      'config에 정의된 모든 ESP32 방의 센서 상태(SHT30 정상 / SHT20 폴백 / 없음 / 구형펌웨어 / 오프라인)를 반환합니다.',
  })
  getSensorHealth(): RoomSensorStatus[] {
    return this.sensorHealthService.getReport();
  }

  @Post(':hostname/offset')
  @ApiOperation({
    summary: '온도 보정값 설정',
    description:
      'MQTT calibrate 명령을 발행합니다. 장치가 저장 후 sensor_health로 재보고(≤5초)하면 적용 확인.',
  })
  setOffset(
    @Param('hostname') hostname: string,
    @Body() body: SetTempOffsetDto,
  ): { sent: boolean } {
    this.sensorHealthService.setTempOffset(hostname, Number(body.tempOffset));
    return { sent: true };
  }

  @Post(':hostname/reboot')
  @ApiOperation({ summary: '장치 재부팅 (openHASP 코어 reboot 명령)' })
  reboot(@Param('hostname') hostname: string): { sent: boolean } {
    this.sensorHealthService.rebootDevice(hostname);
    return { sent: true };
  }
}
```

- [ ] **Step 2: 검증 + 커밋**

```bash
npx tsc --noEmit -p tsconfig.json && npx eslint src/sensor-health/ && npm test -- sensor-health.service
git add src/sensor-health/sensor-health.controller.ts
git commit -m "feat(sensor-health): 보정/재부팅 REST 엔드포인트

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```
Expected: tsc/eslint 클린, 테스트 PASS.

---

### Task 4: 어드민 — 보정 인라인 편집 + 재부팅 버튼

**Files:**
- Modify: `~/Projects/superb-backend/public/admin/admin.js` (`setupSensorStatus`, `loadSensorStatus`, `renderSensorStatus`)

**Interfaces:**
- Consumes: Task 3의 `POST /sensor-health/:hostname/offset|reboot`, 기존 `GET /sensor-health`(`tempOffset`, `status` 필드), 기존 `this.showToast(msg, type)`, `this.escapeHtml(v)`

- [ ] **Step 1: renderSensorStatus의 보정 칸을 input으로, 조치 칸에 재부팅 버튼 추가**

기존 `const offset = ...` 블록을 다음으로 교체 (⚠ 의심 표시는 유지):

```js
        const offSuspect =
          typeof r.tempOffset === 'number' &&
          r.sensor === 'sht30' &&
          Math.abs(r.tempOffset) >= 5;
        // 보정 편집: offline은 불가, legacy(구형 펌웨어)는 calibrate 명령 미지원이라 불가
        const canCalibrate = r.status !== 'offline' && r.status !== 'legacy';
        const offVal = typeof r.tempOffset === 'number' ? r.tempOffset : '';
        const offset = canCalibrate
          ? `<span class="inline-flex items-center gap-1">
              <input type="number" step="0.5" min="-30" max="30" value="${offVal}"
                data-offset-input="${this.escapeHtml(r.hostname)}"
                class="w-20 px-2 py-1 border border-gray-300 rounded text-sm ${offSuspect ? 'text-amber-700 font-medium' : 'text-gray-700'}">
              <button type="button" data-save-offset="${this.escapeHtml(r.hostname)}"
                class="text-xs px-2 py-1 rounded bg-blue-50 text-blue-700 hover:bg-blue-100">저장</button>
              ${offSuspect ? '<span class="text-amber-700">⚠</span>' : ''}
            </span>`
          : typeof r.tempOffset === 'number'
            ? `<span class="text-gray-500">${r.tempOffset.toFixed(1)}°C</span>`
            : '-';
```

행 마지막 `<td>`(조치)를 다음으로 교체 — 재부팅 버튼 추가 (offline만 비활성):

```js
          <td class="py-2 text-sm">
            <span class="text-amber-700">${note}</span>
            <button type="button" data-reboot="${this.escapeHtml(r.hostname)}"
              ${r.status === 'offline' ? 'disabled' : ''}
              class="ml-2 text-xs px-2 py-1 rounded ${r.status === 'offline' ? 'bg-gray-100 text-gray-400' : 'bg-gray-100 text-gray-600 hover:bg-gray-200'}">재부팅</button>
          </td>
```

- [ ] **Step 2: loadSensorStatus에 입력 중 재렌더 가드**

`loadSensorStatus()` 함수 시작부 `if (!body) return;` 다음에:

```js
    // 사용자가 보정값 입력 중이면 이번 주기 재렌더를 건너뜀 (입력 덮어쓰기 방지)
    if (body.contains(document.activeElement) && document.activeElement.tagName === 'INPUT') {
      return;
    }
```

- [ ] **Step 3: setupSensorStatus에 위임 리스너 추가** (표는 매 폴링 재렌더되므로 위임 필수)

`setupSensorStatus()` 끝에:

```js
    const body = document.getElementById('sensor-status-body');
    if (body && !body.dataset.wired) {
      body.dataset.wired = '1';
      body.addEventListener('click', (e) => {
        const save = e.target.closest('[data-save-offset]');
        if (save) this.saveSensorOffset(save.dataset.saveOffset);
        const reboot = e.target.closest('[data-reboot]');
        if (reboot && !reboot.disabled) this.rebootSensorDevice(reboot.dataset.reboot);
      });
      // Enter로 저장 (폼 제출 방지)
      body.addEventListener('keydown', (e) => {
        if (e.key === 'Enter' && e.target.matches('[data-offset-input]')) {
          e.preventDefault();
          this.saveSensorOffset(e.target.dataset.offsetInput);
        }
      });
    }
```

- [ ] **Step 4: saveSensorOffset / rebootSensorDevice 메서드 추가** (`loadSensorStatus` 아래)

```js
  async saveSensorOffset(hostname) {
    const input = document.querySelector(`[data-offset-input="${hostname}"]`);
    if (!input) return;
    const v = parseFloat(input.value);
    if (!Number.isFinite(v) || Math.abs(v) > 30) {
      this.showToast('보정값은 -30 ~ +30 범위여야 합니다.', 'error');
      return;
    }
    try {
      const res = await fetch(`/sensor-health/${encodeURIComponent(hostname)}/offset`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ tempOffset: v }),
      });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      input.blur(); // 가드 해제 → 다음 폴링에서 장치 재보고 값으로 갱신
      this.showToast(`${hostname} 보정값 전송됨 (곧 반영)`, 'success');
    } catch (err) {
      this.showToast(`보정값 전송 실패: ${err.message}`, 'error');
    }
  }

  async rebootSensorDevice(hostname) {
    if (!confirm(`${hostname} 장치를 재부팅할까요?\n재부팅 중 약 30초간 오프라인이 됩니다.`)) return;
    try {
      const res = await fetch(`/sensor-health/${encodeURIComponent(hostname)}/reboot`, {
        method: 'POST',
      });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      this.showToast(`${hostname} 재부팅 명령 전송됨`, 'success');
    } catch (err) {
      this.showToast(`재부팅 실패: ${err.message}`, 'error');
    }
  }
```

- [ ] **Step 5: 문법 + 렌더 검증**

```bash
node --check public/admin/admin.js
```
그리고 렌더 로직 단독 검증 (renderSensorStatus/escapeHtml만 추출 실행):

```bash
cat > /tmp/rt3.mjs <<'EOF'
import { readFileSync } from 'fs';
const src = readFileSync('public/admin/admin.js','utf8');
function grab(name){const i=src.indexOf(`  ${name}(`);let d=0,s=false,o='';for(let j=i;j<src.length;j++){const c=src[j];o+=c;if(c==='{'){d++;s=true}else if(c==='}'){d--;if(s&&d===0)break}}return o}
const M=new (eval(`(class T{ ${grab('renderSensorStatus')} ${grab('escapeHtml')} })`))();
const html=M.renderSensorStatus([
 {hostname:'room01',status:'ok',sensor:'sht30',temperature:15.4,tempOffset:-12,i2c:'x',lastSeenSecondsAgo:0},
 {hostname:'room07',status:'legacy',sensor:null,temperature:24,tempOffset:null,i2c:null,lastSeenSecondsAgo:3},
 {hostname:'room09',status:'offline',sensor:null,temperature:null,tempOffset:null,i2c:null,lastSeenSecondsAgo:99},
]);
const checks=[
 ['ok방 input', html.includes('data-offset-input="room01"')],
 ['ok방 저장버튼', html.includes('data-save-offset="room01"')],
 ['legacy방 input 없음', !html.includes('data-offset-input="room07"')],
 ['offline방 input 없음', !html.includes('data-offset-input="room09"')],
 ['재부팅 버튼', html.includes('data-reboot="room01"')],
 ['offline 재부팅 disabled', /data-reboot="room09"\s+disabled/.test(html)],
 ['버튼 type=button', !/<button(?![^>]*type="button")/.test(html)],
];
let fail=0; for(const [n,ok] of checks){console.log((ok?'OK  ':'FAIL')+' '+n); if(!ok)fail++;}
process.exit(fail?1:0);
EOF
node /tmp/rt3.mjs
```
Expected: 전부 OK, exit 0.

- [ ] **Step 6: 커밋 + PR 2건 생성 (머지 금지)**

```bash
git add public/admin/admin.js
git commit -m "feat(admin): 센서 상태 표에서 보정값 인라인 편집 + 장치 재부팅

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
git push -u origin feat/remote-calibration
gh pr create --title "feat: 어드민 원격 온도 보정 + 장치 재부팅" --body "## 변경
- SensorHealthService: setTempOffset/rebootDevice — config 검증 후 MQTT 발행 (유닛 테스트 5건, 이 저장소 첫 유닛 spec)
- POST /sensor-health/:hostname/offset {tempOffset} (±30 초과 400, 미등록 404) / POST :hostname/reboot
- 어드민 센서 상태 표: 보정 칸 인라인 편집(legacy/offline 비활성) + 재부팅 버튼(확인창)
- 15초 폴링 대응: 이벤트 위임 + 입력 중 재렌더 가드

## 계약
- calibrate: hasp/<host>/command/custom/calibrate {\"temp_offset\":N} (펌웨어 PR 필요)
- reboot: hasp/<host>/command/reboot (openHASP 코어 기존 명령, 구형 펌웨어에도 동작)

스펙: openhasp-hvac-thermostat docs/superpowers/specs/2026-07-16-remote-calibration-design.md

🤖 Generated with [Claude Code](https://claude.com/claude-code)"
cd ~/Projects/openhasp-hvac-thermostat
gh pr create --title "펌웨어: MQTT calibrate 명령으로 온도 보정 원격 설정" --body "## 변경
- hasp/<dev>/command/custom/calibrate {\"temp_offset\":N} 수신 시 활성 센서에 보정 적용 + /superb.json 영속화 (기존 set_offsets 재사용)
- 범위 -30~+30 밖/파싱 실패는 로그만 남기고 무시. rh_offset은 기존 값 유지
- 적용 확인은 sensor_health.temp_offset 재발행(≤5초)으로 관측

백엔드 카운터파트: superb-backend feat/remote-calibration
스펙: docs/superpowers/specs/2026-07-16-remote-calibration-design.md

🤖 Generated with [Claude Code](https://claude.com/claude-code)"
```

---

### Task 5: 통합 검증 (머지·배포 후, 실장치)

머지와 배포(펌웨어 webflash + 백엔드 릴리즈)는 사용자 결정. 그 후:

- [ ] 검증 장치 1대 OTA → mosquitto로 `hasp/<dev>/command/custom/calibrate {"temp_offset":-1}` 발행 → 5초 내 `sensor_health.temp_offset` 이 −1로 변하는지 확인
- [ ] 장치 재부팅 후에도 −1 유지 확인 (영속화)
- [ ] 어드민에서 room01 보정 −12 → 0 저장 → 표에 반영 + 실온 정상화 확인
- [ ] 어드민 재부팅 버튼 → 확인창 → 장치 재부팅 → ~30초 후 online 복귀 확인
- [ ] 입력 중 15초 폴링에 값이 덮이지 않는지 확인
