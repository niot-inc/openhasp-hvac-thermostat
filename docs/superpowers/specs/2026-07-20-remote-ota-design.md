# 어드민 원격 펌웨어 OTA + 버전 가시화 설계

날짜: 2026-07-20
대상: openhasp-hvac-thermostat (펌웨어+CI), superb-backend (백엔드+어드민)

## 배경 / 실증

장치 펌웨어 업데이트가 "장치 웹 UI에 수동 업로드"라 방마다 반복 작업이고, 어느 방이
무슨 버전인지 알 수 없다. **핵심 메커니즘은 2026-07-20 room02에서 실기 검증 완료**:
- openHASP 코어 `command/update` + URL 페이로드 → HTTP 다운로드→플래시→재부팅 (전 장치 지원, 펌웨어 수정 불필요)
- Docker 백엔드 호스트의 LAN HTTP 서빙을 ESP32가 정상 수신
- GitHub Pages(`niot-inc.github.io/openhasp-hvac-thermostat/webflash/`)는 public — 토큰 불필요
- 보정값(/superb.json)은 OTA에 보존됨

## 요구사항 (사용자 확정)

1. 어드민 센서 상태 표에 **방별 펌웨어 버전** 표시. 최신 버전·릴리즈 노트도 보이게.
2. **버전 비교** 후 업데이트: 최신이면 버튼 비활성 "최신 ✅", 구버전이면 "→ vX.Y.Z 업데이트 가능" + [업데이트].
3. **fw 미보고 방**(구형 openHASP/fw필드 이전 빌드)은 펌웨어 칸에 **"구형 – 업데이트 필요"** + 버튼 활성 (command/update는 코어 기능이라 동작).
4. **중복 클릭 방지: 백엔드가 원천 차단** — 방별 진행 상태 소유, 진행 중 POST는 409. 버튼은 그 상태로 잠김(다른 브라우저에서도 동일).
5. 업데이트 진행/성공/실패를 어드민에서 확인 (검증 포함).
6. 방별 버튼만 (일괄 없음). 확인창 필수.

## 버전 체계 (프로세스 변경)

- 펌웨어 저장소도 **GitHub Release 태그 기반 배포**로 전환. **v1.0.0부터 시작.**
- 릴리즈 발행 → 기존 deploy 워크플로우(release 트리거 이미 존재)가 빌드:
  - `-D SUPERB_FW_VERSION='"v1.0.0"'` 주입 (platformio_override.ini build_flags에 CI가 append)
  - webflash 갱신 + **`webflash/version.json` 생성**: `{"version","date","sha","notes"}` — notes는 릴리즈 본문에서.
- workflow_dispatch 빌드는 테스트용: SUPERB_FW_VERSION은 `dev-<sha7>`, version.json은 갱신하지 않음.

## 구성요소

### 펌웨어 (src/custom/my_custom.cpp + .github/workflows/build.yml)
- sensor_health에 `"fw": SUPERB_FW_VERSION` 추가. `#ifndef SUPERB_FW_VERSION #define SUPERB_FW_VERSION "dev" #endif`.
- CI: 오버레이 복사 후 build_flags에 버전 define append. release 이벤트 시 태그명, dispatch 시 dev-sha.
- CI(release 배포 시): version.json 생성해 webflash/와 함께 커밋.

### 백엔드 (src/sensor-health/)
- **FirmwareUpdateService** (신규):
  - 최신 버전 조회: Pages `webflash/version.json` fetch, 10분 캐시.
  - 바이너리 캐시: Pages에서 bin 다운로드해 데이터 볼륨(`/app/data/firmware/<version>/firmware_ota.bin`)에 캐시.
  - `startUpdate(hostname)`: 진행 중이면 ConflictException(409). bin 캐시 보장 → `command/update`에
    `${otaBaseUrl}/sensor-health/firmware/<version>/firmware_ota.bin` 발행 → 상태 updating.
  - 상태 머신 (방별): idle → updating → success(보고된 fw == latest) / failed(3분 타임아웃).
    fw 관측은 기존 SensorHealthService의 sensor_health 수신에 연동.
- **otaBaseUrl 설정**: 장치가 접근할 백엔드 LAN 베이스 URL (예: `http://192.168.0.47:8080`).
  system config에 선택 필드로 추가(정확한 스키마 위치는 구현 시 SystemConfigService 탐색).
  미설정 시 업데이트 버튼 비활성 + "otaBaseUrl 설정 필요" 안내.
- **엔드포인트**:
  - `GET /sensor-health` 응답 확장: `{ latest: {version,date,notes}|null, rooms: RoomSensorStatus[] }`
    (breaking — 유일 소비자인 어드민을 같이 갱신). RoomSensorStatus에 `fw: string|null`,
    `updateState: 'idle'|'updating'|'success'|'failed'`, `updateAvailable: boolean` 추가.
  - `POST /sensor-health/:hostname/update` → 200 `{sent:true}` / 409 진행 중 / 400 otaBaseUrl 미설정·최신불명.
  - `GET /sensor-health/firmware/:version/firmware_ota.bin` → 캐시된 bin 스트리밍 (장치용, 무인증 — 어차피 public 펌웨어).

### 어드민 (public/admin/)
- 섹션 헤더에 최신 버전 + 릴리즈 노트 표시.
- 표에 "펌웨어" 컬럼:
  - fw == latest → `vX.Y.Z ✅` (버튼 없음)
  - fw != latest → `vA → vB 업데이트 가능` + [업데이트]
  - fw null → **"구형 – 업데이트 필요"** + [업데이트]
  - updateState=updating → "업데이트 중…" (버튼 잠김) / failed → "실패 – 재시도" (버튼 활성)
  - offline → 버튼 비활성
- 확인창: "roomNN을 vX.Y.Z로 업데이트할까요? 재부팅 포함 약 1분."
- 기존 15초 폴링·위임 패턴 재사용.

## 에러 처리
- version.json fetch 실패 → latest null → 업데이트 버튼 전체 비활성 + 배너로 안내 (표시는 정상 동작).
- bin 다운로드 실패 → POST update 5xx + 토스트.
- 장치가 URL 못 받는 경우(잘못된 otaBaseUrl) → 아무 일도 안 일어남 → 3분 타임아웃 → failed 표시. (실증 실험에서 다운로드 실패는 무해함을 확인)
- 진행 중 백엔드 재시작 → 상태 소실 → idle로 복귀(무해: 장치는 알아서 완료하고 fw 재보고로 최신 인지).

## 검증
1. 펌웨어 CI: dispatch 빌드에서 fw="dev-<sha>" 발행 확인(장치 1대) → v1.0.0 릴리즈 → version.json 생성 확인.
2. 백엔드 유닛: FirmwareUpdateService 상태 머신(409, 타임아웃, 성공 전이) + 기존 스위트.
3. 실기: 어드민에서 구버전 방 [업데이트] → 업데이트 중 → LWT 사이클 → fw 갱신 → ✅. 중복 클릭 409 확인.

## 범위 밖
- 일괄 업데이트, 롤백 UI, 펌웨어 서명 검증, staged rollout.
