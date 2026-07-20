# 원격 OTA + 버전 가시화 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 어드민에서 방별 펌웨어 버전 확인 + 원클릭 원격 OTA(진행/성공/실패 표시, 중복 방지).

**Architecture:** 펌웨어 CI가 릴리즈 태그를 빌드에 주입하고 `webflash/version.json`을 발행 → 장치가 `sensor_health.fw`로 버전 보고 → 백엔드가 version.json과 비교해 "업데이트 가능" 판단, 요청 시 bin을 캐시해 LAN 서빙하고 `command/update`(코어 기능, 실기 검증됨) 발행 → fw 변경 관측으로 성공 판정. 스펙: `docs/superpowers/specs/2026-07-20-remote-ota-design.md`

**Tech Stack:** GitHub Actions, Arduino C++, NestJS(+@nestjs/axios), 순수 브라우저 JS.

## Global Constraints

- 저장소 2개: 펌웨어 `~/Projects/openhasp-hvac-thermostat`, 백엔드 `~/Projects/superb-backend`. 각각 feature 브랜치 → PR 생성까지만(머지는 사용자).
- 버전 문자열: 릴리즈 빌드 = 태그명 그대로(`v1.0.0`), dispatch 빌드 = `dev-<sha7>`. 펌웨어 페이로드 키는 `fw`.
- version.json 스키마: `{"version":"v1.0.0","date":"YYYY-MM-DD","sha":"<sha7>","notes":"<릴리즈 본문>"}` — **release 이벤트에서만** 생성/커밋.
- OTA URL 형식: `${otaBaseUrl}/sensor-health/firmware/<version>/firmware_ota.bin`. otaBaseUrl 미설정 시 update 요청은 400.
- 중복 방지: 방별 진행 중 POST update → **409**. 타임아웃 3분(성공 판정: 보고된 fw == latest.version).
- 어드민 펌웨어 칸: 최신 `vX ✅`(버튼 없음) / 구버전 `vA → vB 업데이트 가능`+[업데이트] / fw null `구형 – 업데이트 필요`+[업데이트] / updating `업데이트 중…`(잠김) / failed `실패 – 재시도` / offline 버튼 비활성. 버튼 전부 `type="button"`, 위임 리스너, confirm() 필수.
- `GET /sensor-health` 응답이 `{latest, rooms}`로 바뀜(breaking) — 어드민 같은 PR에서 함께 수정.

---

### Task 1: 펌웨어 fw 필드 + CI 버전 주입/version.json

**Files:**
- Modify: `~/Projects/openhasp-hvac-thermostat/src/custom/my_custom.cpp` (sensor_health 발행부, `hb["temp_offset"]` 근처)
- Modify: `~/Projects/openhasp-hvac-thermostat/.github/workflows/build.yml`

**Interfaces:**
- Produces: sensor_health 페이로드에 `"fw":"<버전>"` (릴리즈 태그 또는 `dev-<sha7>`, 미주입 빌드는 `"dev"`)
- Produces: release 배포 시 `webflash/version.json` (Global Constraints 스키마)

- [ ] **Step 1: 브랜치** — `cd ~/Projects/openhasp-hvac-thermostat && git checkout main && git pull && git checkout -b feat/remote-ota`

- [ ] **Step 2: my_custom.cpp** — 파일 상단 include 아래에:

```cpp
// CI가 빌드 시 주입 (릴리즈 태그 또는 dev-<sha7>). 미주입 로컬 빌드는 "dev".
#ifndef SUPERB_FW_VERSION
#define SUPERB_FW_VERSION "dev"
#endif
```

sensor_health 발행부 `hb["temp_offset"]=...;` 다음 줄에:

```cpp
        hb["fw"]=SUPERB_FW_VERSION;                  // 펌웨어 버전 (어드민 표시/업데이트 판정용)
```

- [ ] **Step 3: build.yml — 버전 결정 + 주입.** "Apply overlay onto openHASP tree" 스텝 바로 뒤에 추가:

```yaml
      - name: Determine firmware version
        id: fwver
        run: |
          if [ "${{ github.event_name }}" = "release" ]; then
            echo "version=${{ github.event.release.tag_name }}" >> $GITHUB_OUTPUT
          else
            echo "version=dev-$(git -C overlay rev-parse --short HEAD)" >> $GITHUB_OUTPUT
          fi

      - name: Inject firmware version into build flags
        run: |
          # [override] build_flags에 SUPERB_FW_VERSION define 추가
          sed -i "s|^build_flags =|build_flags =\n   -D SUPERB_FW_VERSION='\"${{ steps.fwver.outputs.version }}\"'|" openhasp/platformio_override.ini
          grep -A3 "^build_flags" openhasp/platformio_override.ini | head -5
```

- [ ] **Step 4: build.yml — release 트리거에서도 deploy 동작 + version.json.** 현재 `Deploy to webflash` 스텝의 if는 release || (dispatch && deploy). 그 스텝의 cp 3줄 다음에 추가:

```yaml
          if [ "${{ github.event_name }}" = "release" ]; then
            python3 - <<'EOF'
          import json, os, subprocess, datetime
          notes = os.environ.get("RELEASE_NOTES", "").strip()
          sha = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"]).decode().strip()
          json.dump({
            "version": os.environ["FW_VERSION"],
            "date": datetime.date.today().isoformat(),
            "sha": sha,
            "notes": notes,
          }, open("webflash/version.json", "w"), ensure_ascii=False, indent=2)
          EOF
          fi
```

같은 스텝에 env 추가:

```yaml
        env:
          FW_VERSION: ${{ steps.fwver.outputs.version }}
          RELEASE_NOTES: ${{ github.event.release.body }}
```

주의: 이 heredoc은 워크플로우 YAML 들여쓰기 안에서 동작해야 함 — 기존 스텝의 `run: |` 블록 들여쓰기에 맞출 것. `git add webflash/`가 이미 version.json을 포함하므로 커밋 로직 변경 불필요. release 이벤트는 태그(detached)에서 돌므로 기존 `TARGET_BRANCH` 로직(release→main)이 그대로 적용됨.

- [ ] **Step 5: 커밋 + dispatch 빌드로 검증**

```bash
git add src/custom/my_custom.cpp .github/workflows/build.yml
git commit -m "feat: 펌웨어 버전 보고(fw) + CI 버전 주입/version.json

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
git push -u origin feat/remote-ota
gh workflow run build.yml --ref feat/remote-ota -f deploy=false
# 완료 대기 후 conclusion=success 확인. 빌드 로그에서 SUPERB_FW_VERSION 주입 라인(grep 출력) 확인.
```

---

### Task 2: 백엔드 FirmwareUpdateService (TDD)

**Files:**
- Create: `~/Projects/superb-backend/src/sensor-health/firmware-update.service.ts`
- Create: `~/Projects/superb-backend/src/sensor-health/firmware-update.service.spec.ts`
- Modify: `~/Projects/superb-backend/src/sensor-health/sensor-health.module.ts` (provider 등록)

**Interfaces:**
- Consumes: `MqttService.publish(topic, message)`; `HttpService`(@nestjs/axios, ExternalServicesModule가 export하는 HttpModule 없으면 모듈에 HttpModule import 추가); SystemConfigService(전역) — otaBaseUrl 설정 조회.
- Produces (Task 3/어드민이 사용):
  - `getLatest(): Promise<{version:string; date:string; notes:string}|null>` — Pages version.json, 10분 캐시, 실패 시 null
  - `startUpdate(hostname: string): Promise<void>` — 409 ConflictException(진행 중), 400 BadRequestException(otaBaseUrl 미설정/최신 불명), bin 캐시 보장 후 `hasp/<host>/command/update`에 URL 발행, 상태 updating
  - `onFwReported(hostname: string, fw: string): void` — SensorHealthService가 sensor_health 수신 시 호출; updating 중 fw==latest면 success
  - `getUpdateState(hostname: string): 'idle'|'updating'|'success'|'failed'` — 3분 경과 시 failed로 평가
- 구현 요점: 상태는 in-memory Map<hostname,{state,since,target}>. 시간은 `Date.now()` 주입 가능하게(테스트용 now() 함수 프로퍼티 또는 jest.useFakeTimers). bin 캐시 경로 `/app/data/firmware/<version>/firmware_ota.bin` (환경변수 DATA_PATH 있으면 그것, 없으면 ./data — 기존 코드에서 데이터 경로 관례 탐색해 맞출 것). version.json/bin URL 베이스: `https://niot-inc.github.io/openhasp-hvac-thermostat/webflash/`.
- otaBaseUrl 설정: SystemConfigService 스키마 탐색 후 **가장 작은 침습**으로 추가 (예: 기존 sensorHealth/externalServices 섹션에 optional 필드). config에 없으면 undefined 허용.

- [ ] **Step 1: 브랜치** — `cd ~/Projects/superb-backend && git checkout main && git pull && git checkout -b feat/remote-ota`
- [ ] **Step 2: 실패하는 spec 작성** — 최소 케이스: ①getLatest 성공 파싱+캐시(HttpService mock 1회만 호출) ②getLatest 실패→null ③startUpdate: otaBaseUrl 미설정→BadRequest ④진행 중 재호출→Conflict, publish 1회만 ⑤정상: publish 토픽/URL 정확성 ⑥onFwReported로 success 전이 ⑦3분 경과→failed. bin 캐시는 fs mock 또는 다운로드 함수 spy로 격리.
- [ ] **Step 3: FAIL 확인** — `npm test -- firmware-update`
- [ ] **Step 4: 구현** — 위 Produces 계약대로. 파일 I/O는 명확히 분리된 private 메서드(테스트에서 spy 가능하게).
- [ ] **Step 5: PASS 확인** — `npm test -- firmware-update && npm test -- sensor-health.service` (기존 10건 유지)
- [ ] **Step 6: 검증+커밋** — `npx eslint src/sensor-health/ --fix && npx tsc --noEmit -p tsconfig.json` 클린 후 커밋(`feat(sensor-health): FirmwareUpdateService — 버전 조회/bin 캐시/OTA 상태머신 (TDD)`).

---

### Task 3: 백엔드 통합 — report 확장 + 엔드포인트

**Files:**
- Modify: `~/Projects/superb-backend/src/sensor-health/sensor-health.service.ts`
- Modify: `~/Projects/superb-backend/src/sensor-health/sensor-health.controller.ts`
- Modify(필요시): `~/Projects/superb-backend/src/sensor-health/sensor-health.service.spec.ts` (report 형태 변경 반영)

**Interfaces:**
- Consumes: Task 2의 FirmwareUpdateService 전체.
- Produces (어드민 계약):
  - `GET /sensor-health` → `{ latest: {version,date,notes}|null, rooms: RoomSensorStatus[] }`
    - RoomSensorStatus 추가 필드: `fw: string|null`(sensor_health의 fw), `updateState`, `updateAvailable: boolean`(fw!=null && latest!=null && fw!==latest.version, 또는 fw==null && online && latest!=null)
  - `POST /sensor-health/:hostname/update` → `{sent:true}` / 400 / 404(미등록) / 409
  - `GET /sensor-health/firmware/:version/firmware_ota.bin` → 캐시된 파일 스트리밍(StreamableFile), 없으면 404. :version은 `[\w.\-]+`만 허용(경로 탈출 방지).
- SensorHealthService 변경: SensorHealthPayload에 `fw?: string`; onHealth에서 `firmwareUpdateService.onFwReported(hostname, payload.fw)` 호출(있을 때); classify 결과에 fw/updateState/updateAvailable 채움; getReport 반환 형태는 유지하고 컨트롤러에서 `{latest, rooms}` 조합 (service 단위테스트 영향 최소화).

- [ ] **Step 1: 구현** (위 계약대로; 컨트롤러 GET은 async로 latest 조합)
- [ ] **Step 2: 검증** — `npm test -- sensor-health` 전체 PASS(기존+신규), tsc/eslint 클린
- [ ] **Step 3: 커밋** — `feat(sensor-health): 버전 비교/업데이트 API + 펌웨어 서빙`

---

### Task 4: 어드민 — 펌웨어 컬럼 + 업데이트 버튼 + 배너

**Files:**
- Modify: `~/Projects/superb-backend/public/admin/admin.js` (loadSensorStatus/renderSensorStatus/위임부)
- Modify: `~/Projects/superb-backend/public/admin/index.html` (섹션 설명에 최신버전/노트 표시 자리 `<div id="sensor-status-latest">` 추가 — 문구는 JS가 채움)

**Interfaces:**
- Consumes: Task 3의 `{latest, rooms}` 응답, `POST :hostname/update`. 기존 escapeHtml/showToast/위임 리스너/폴링 가드.

- [ ] **Step 1: loadSensorStatus** — `const data = await res.json();` 후 `data.rooms`로 렌더, `data.latest`로 `#sensor-status-latest`에 `최신 펌웨어: vX.Y.Z (날짜) — 노트` 표시(latest null이면 "최신 버전 정보를 가져오지 못했습니다 — 업데이트 비활성").
- [ ] **Step 2: renderSensorStatus** — I2C 컬럼 앞에 "펌웨어" 컬럼 추가, Global Constraints의 상태별 표시 규칙 그대로. 버튼: `data-fw-update="<hostname>"`, disabled 조건: offline || updating || !latest. 모든 문자열 escapeHtml.
- [ ] **Step 3: 위임 리스너에 fw-update 분기** — confirm(`"${hostname}을(를) ${latest.version}(으)로 업데이트할까요?\n재부팅 포함 약 1분 소요됩니다."`) → POST → 409면 "이미 진행 중" 토스트 → 성공 토스트 후 즉시 loadSensorStatus().
- [ ] **Step 4: 검증** — `node --check`; 렌더 단독 테스트(기존 /tmp 패턴): 최신✅/업데이트가능/구형–업데이트 필요/updating 잠김/failed 재시도/offline 비활성/latest null 전체 비활성 각 1케이스.
- [ ] **Step 5: 커밋 + PR 2건 생성(머지 금지)** — 백엔드 PR: "feat(admin): 방별 펌웨어 버전 표시 + 원격 OTA 업데이트" (스펙 경로·실증 실험 언급, breaking 응답형태 명시). 펌웨어 PR: "펌웨어: 버전 보고(fw) + CI 릴리즈 버전 주입/version.json" (v1.0.0 릴리즈 절차 안내 포함).

---

### Task 5: 릴리즈 + 실기 검증 (사용자와, 머지 후)

- [ ] 두 PR 머지 → 펌웨어 **release v1.0.0 발행**(노트 작성) → CI가 webflash+version.json 배포 확인
- [ ] 백엔드 릴리즈(v2.0.45) → watchtower 배포 → 각 사이트 config에 otaBaseUrl 설정
- [ ] 어드민에서: 구버전 방이 "구형 – 업데이트 필요"로 보이는지 → [업데이트] → 업데이트 중 → ✅ v1.0.0 확인 → 중복 클릭 409 확인
