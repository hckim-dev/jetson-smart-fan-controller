# Jetson Smart Fan Controller

Jetson Orin Nano에서 Linux Kernel Driver와 C 사용자 프로그램으로 모터, 로터리 엔코더, LED Bar, LCD, BMP180을 통합한 스마트 선풍기 제어 프로젝트입니다.

단순한 GPIO 출력에서 출발해 PWM 풍속 제어, 온도 AUTO 정책, 센서 프로세스 IPC, LCD 스레드를 구현했습니다. **모터 출력과 정지 정책은 커널이 소유**하고, 사용자 프로그램은 입력과 운전 정책을 조정합니다. 현재 범위는 1~6단계이며 이후 단계의 신규 기능은 포함하지 않습니다.

## 핵심 기능

| 기능            | 구현                                                                     |
| --------------- | ------------------------------------------------------------------------ |
| 수동 제어       | ON/OFF, 풍속 0~5, 4ms 요청 PWM(목표250Hz), 저속 기동 시 200ms boost      |
| 로터리          | GPIO v2 양쪽 edge, Gray 4전이 해석, 방향 반전, 상·하한 제한              |
| 온도 AUTO       | BMP180 온도에 따른 0~5단계, 하강 경계 1°C hysteresis                     |
| 출력 표시       | LED Bar 8칸, LCD 16×2 모드·온도·선택 풍속                                |
| 정지 정책       | 단일 controller, close/오류 정지, heartbeat lease 2초, 연속 ON 30초 제한 |
| 프로세스/스레드 | 센서 fork/exec + 32-byte pipe IPC, LCD worker + mutex/condition          |
| 회복 정책       | 센서 fault/stale 또는 보호 정지 뒤에는 명시적인 ON 필요                  |

```mermaid
flowchart LR
    ENC[EC11 + NPN] --> GPIO[GPIO v2 edge fd]
    GPIO --> CLI[fanctl · poll loop]
    BMP[BMP180 child] --> PIPE[32-byte pipe]
    PIPE --> CLI
    CLI --> IOCTL[/dev/smartfan · ioctl]
    IOCTL --> DRV[Kernel Driver · mutex / deadlines]
    DRV --> MOTOR[L298N / motor]
    DRV --> LED[LED Bar]
    CLI --> WORKER[LCD worker]
    WORKER --> LCD[PCF8574 / LCD1602]
```

## 현재 검증 상태

Native build와 소프트웨어 회귀 검증을 통과했습니다. 모터·LED·BMP180 및 파이프 없는 실행의 명령 입력/로터리 동작은 사용자 실물 확인을 받았습니다. 사용자에게서 AUTO 정지·복구, lease 정지, 30초 제한, Ctrl+C 정지도 확인받았습니다. AUTO 복구 시험의 구체적인 고장 주입 방식과 모든 전기 특성이 측정된 것은 아닙니다.

**남은 항목:** LCD 전체3.3V 구성의 대비 부족, Jetson 5V 핀으로 공급하는 모터의 기동/정지 전류와 전압 여유, 실제 system suspend, 단계별 RPM/발열/장기 반복 측정. 구현 및 단위 테스트 통과를 전기적 안전 인증이나 실물 worst-case 성능 보장으로 표현하지 않습니다.

## 빠른 시작

검증 환경은 Jetson aarch64, Ubuntu22.04, L4T36.5.2, kernel5.15.199-tegra, GCC11.4.0, glibc2.35입니다. GPIO/PWM/I²C 설정이 적용된 현재 장비를 기준으로 합니다.

```sh
make all
make test
./build/fanctl --dry-run --encoder --bmp180 --lcd
```

Dry-run은 실제 GPIO/I²C에 접근하지 않습니다. `temp 27`, `mode auto`, `on`, `temp error`, `off`, `quit`로 정책을 시험할 수 있습니다.

실제 장비에서는 배선과 전원 상태를 확인한 뒤:

```sh
sudo ./build/fanctl --encoder --bmp180 --lcd --lcd-address 0x27
```

시작 상태는 MANUAL/OFF입니다. `mode manual`, `speed 5`, `on`, `status`, `off`, `quit`를 한 줄씩 입력합니다. 이 장비에서 외부 `| tee`를 붙이면 명령 입력이 전달되지 않는 현상을 관찰했으므로 interactive 실행은 위처럼 직접 실행합니다. 내부 sudo/PTY 원인은 확정하지 않았습니다.

## 문서

- [시스템 설계와 동시성](docs/architecture.md)
- [현재 하드웨어·핀 연결·전원 한계](docs/hardware.md)
- [빌드·실행·개발 검증](docs/build-run.md)
- [통합 실물 테스트](docs/final-test.md)
- [개발 과정과 문제 해결](docs/development.md)
- [발표·포트폴리오용 설명](docs/portfolio.md)
- [이번 품질 개선 결과](docs/quality-review.md)
- [현재 완료/미검증 상태](docs/status.md)

## 저장소 구조

```text
app/       main.c / controller.c / fan-device.c, peripheral modules, diagnostics
driver/    platform driver + miscdevice + GPIO/PWM consumers
include/    fixed-width ioctl UAPI
dts/       Tegra234 overlay source
scripts/    offline DT preparation / guarded installer
tests/     mock, IPC, PTY, CLI, cleanup regression tests
docs/      current architecture, hardware, tests and selected development notes
```

빌드 산출물·실험 로그는 `build/`에 생성되며 Git에서 제외합니다. `make clean`은 알려진 산출물만 제거하고 실험 로그·사용자 메모·부팅 제안 자료는 보존합니다. Git commit/push는 사용자가 직접 수행합니다.

과거 상세 조사 원본은 로컬 `docs/history/`에 보존하며 Git에서 제외합니다. 공개 개발 기록은 `docs/development.md`를 기준으로 합니다.
