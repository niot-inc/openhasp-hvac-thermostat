# webflash — 웹 플래셔 배포물

[웹 플래셔](https://niot-inc.github.io/openhasp-hvac-thermostat/index.html)(ESP Web Tools)가 서빙하는 파일들. **직접 커밋하지 말 것** — CI가 갱신한다 (아래 참고).

## 파일 설명

| 파일 | 용도 |
|---|---|
| `manifest.json` | 웹 플래셔 레시피. 어떤 파일을 플래시 어느 주소에 쓸지 정의 |
| `firmware_full.bin` | **풀 이미지** (부트로더 + 파티션 테이블 + 앱 병합). 웹 플래셔가 0x0에 플래시. 새 장치/USB 복구용 |
| `firmware_ota.bin` | **앱 전용 이미지**. 이미 동작 중인 장치의 웹 UI(Firmware Update)에 업로드하는 OTA용. 웹 플래셔는 사용하지 않음 |
| `littlefs.bin` | 파일시스템 이미지 (`data/`의 pages.jsonl, 설정 등). 웹 플래셔가 0x410000(4259840)에 플래시 |

> ⚠️ `firmware_ota.bin`을 manifest에 연결하면 안 된다 — 부트로더/파티션 테이블이 없어 부팅 불가.

## 갱신 방법

둘 중 하나를 실행하면 CI가 최신 소스를 빌드해 이 폴더에 자동 커밋한다:

1. **Actions → Build firmware → Run workflow** (deploy 체크, 기본 on)
2. **GitHub 릴리스 발행**

빌드 파이프라인: openHASP(버전은 `.github/workflows/build.yml`의 `OPENHASP_REF`)에
이 저장소의 오버레이(`src/custom/`, `include/user_config_override.h`,
`platformio_override.ini`, `data/`)를 얹어 `panlee-zw3d95ce01s-tr-4848_16MB` 환경으로 빌드.
