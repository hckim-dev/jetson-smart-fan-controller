# 실행

**현재 설정된 Jetson에서 재현하는 절차입니다.** Ubuntu 22.04 / L4T 36.5.2 / kernel 5.15.199-tegra / GCC 11.4.0 / glibc 2.35에서 검증했습니다. GCC·Make·matching kernel headers·dtc 도구·Python 3가 필요합니다.

## 1. 빌드와 하드웨어 없는 실행

```sh
make all
make test
./build/fanctl --dry-run --encoder --bmp180 --lcd
```

CLI 안에서 `temp 27` → `mode auto` → `on` → `temp error` → `status` → `quit`를 입력합니다. 온도 주입과 `cw/ccw`는 dry-run 전용입니다.

## 2. 실제 장비

[배선·전원](hardware.md)과 DT가 적용된 장비에서 사용합니다. 다른 monitor는 먼저 종료합니다.

```sh
# 모듈이 아직 적재되지 않은 경우
sudo insmod driver/smartfan.ko
sudo ./build/fanctl --encoder --bmp180 --lcd --lcd-address 0x27
```

Driver를 다시 빌드했다면 controller를 종료한 뒤 `sudo rmmod smartfan` → `sudo insmod driver/smartfan.ko`로 반영합니다. 실패 후 그대로 진행하지 않습니다. 현재 DT는 재설치·재부팅할 필요가 없습니다.

## CLI

| 명령                                             | 동작                                     |
| ------------------------------------------------ | ---------------------------------------- |
| `on` / `off`                                     | 기동 / 정지                              |
| `speed 0..5`, `up`, `down`                       | MANUAL 풍속 선택; 0은 정지               |
| `mode manual` / `mode auto`                      | 모드 전환 시 OFF/disarm                  |
| `led 0..8` / `led auto`                          | 모터 OFF의 LED 시험 / 자동 표시          |
| `status` / `stats`                               | 현재 상태 / session·logger 통계          |
| `monitor` / `monitor 500..10000` / `monitor off` | 기본 1000ms / 지정 주기 / 반복 출력 중단 |
| `help` / `quit`                                  | 도움말 / 정지·정리 후 종료               |

로그는 실행 옵션에 `--log build/fan-events.log`를 추가합니다. 기존 파일에 append하며 `sudo tail -n 20 build/fan-events.log`로 확인합니다. 정상 종료의 `LOG_CLOSED`는 dropped=0/error=0입니다. Rotation/fsync는 없으므로 파일 용량·영속성을 별도로 관리해야 합니다.

Stats는 재실행 시 초기화합니다. commands는 비어 있지 않은 모든 명령, sensor_frames는 유효 IPC(error 포함), injected_samples는 dry-run temp입니다. Fault/recovery는 장애 구간 진입/valid 복귀이며 재기동 횟수가 아닙니다. Encoder는 방향 반전 후 step이며 AUTO에서 무시한 실물 입력도 포함하고 CLI up/down은 제외합니다.

<details>
<summary>개발 검사 · DT 준비 · 문제 확인</summary>

```sh
make analyze
make sanitize
make thread-check
make module W=1
make prepare-dt
bash scripts/install-dt.sh --check
```

KDIR은 `/lib/modules/$(uname -r)/build`입니다. 다른 kernel 버전의 tree로 대체하지 않습니다. Python tests는 표준 라이브러리만 사용합니다. `make clean`은 알려진 산출물만 제거하며 로그·메모·boot proposal은 보존합니다.

DT 준비/check는 offline이며 /boot에 쓰지 않습니다. 실제 `sudo bash scripts/install-dt.sh`는 boot 설정을 바꾸므로 백업/fallback과 새 장비 적합성을 검토한 뒤 실행합니다. Installer는 hash·pad·기존 entry를 검사하고 다른 DTB 덮어쓰기를 거부합니다. 범용 자동 설치 도구는 아닙니다.

- 명령 무응답: 외부 `| tee` 없이 직접 실행. 이 장비의 파이프 입력 장애 내부 원인은 미확정.
- GPIO EBUSY: 중복 controller/monitor 종료. GPIO/PWM export로 자원을 이중 점유하지 않음.
- BMP 오류: 통합 실행 종료 후 `./build/bmp180-monitor --once`로 단독 확인.
- 엔코더 진단: `sudo ./build/encoder-monitor`. 방향 반대는 `--reverse-encoder`.
- LCD 오류: 실제 address/100kHz 확인. 표시 실패는 motor 제어와 분리.

</details>
