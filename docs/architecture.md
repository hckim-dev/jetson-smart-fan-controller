# 설계

**main은 운전 정책을, kernel은 출력과 보호 정지를 소유합니다.** 전체 흐름은 [README](../README.md)에 있습니다.

## 책임과 자원

| 구성                            | 담당                                                      |
| ------------------------------- | --------------------------------------------------------- |
| `main.c`                        | 초기화·poll·heartbeat·종료, 모터/GPIO fd와 child PID 수명 |
| `controller.c` / `fan-device.c` | MANUAL/AUTO 정책 / ioctl·dry-run 모델                     |
| `smartfan.c`                    | GPIO/PWM·LED 출력, mutex, lease/max-on/boost delayed work |
| `bmp180-monitor`                | 별도 process의 측정·보정, I²C fd와 pipe 송신              |
| LCD / log worker                | snapshot / 고정 크기 queue, mutex·condition 동기화        |

## 제어와 정지

- **MANUAL:** OFF에서 풍속 선택만으로 기동하지 않음. `on`으로 시작하고 단계 0에서 정지.
- **AUTO:** `mode auto`는 OFF/disarm, `on`은 arm. 정상 저온 대기에서는 이후 온도 상승 시 기동 가능.
- **보호 정지:** 센서 fault/stale·lease/max-on 정지 뒤에는 정상 sample과 명시적 ON 필요.
- **출력:** ENA를 먼저 LOW로 만든 뒤 방향·LED·PWM 정리. PWM disable의 출력 레벨에 의존하지 않음.

AUTO는 24/26/28/30/32°C 경계에서 단계 1~5를 선택하고 하강 hysteresis는 1°C입니다. 단계 1~5의 목표 duty는 60~100%, 저속 기동 boost는 200ms입니다. Duty는 측정 RPM이 아닙니다.

## 경쟁 조건과 수명

| 경계                    | 처리                                                                              |
| ----------------------- | --------------------------------------------------------------------------------- |
| GET → AUTO 기동         | `SET_AUTO`에서 차단 상태·BOOTTIME sample age를 출력 변경과 같은 mutex 안에서 검증 |
| Driver remove → 열린 fd | removed gate로 devm 자원 접근 차단, kref로 상태 객체 유지                         |
| Sensor → main           | 32-byte native-local frame의 partial read·순번·범위·오류 검증                     |
| Worker → main           | 상태는 복사해 전달하고 I²C/write는 mutex 밖에서 수행                              |

센서 fork는 thread 생성 전에 수행하고 제어 fd는 CLOEXEC으로 격리합니다. Child는 parent-death SIGTERM과 parent PID 검사를 사용합니다. Signal handler는 flag/self-pipe만 사용합니다.

**종료 순서:** 모터 OFF/fd close → sensor SIGTERM·필요 시 SIGKILL/waitpid → pipe close → LCD join → log drain/join. Read end를 먼저 닫지 않아 전송 중 SIGPIPE 경쟁을 피합니다. 회수 실패 시 PID, join 실패 시 살아 있는 객체를 유지합니다.

Log는 128×256바이트 queue이며 포화 시 새 record를 버리고 누락을 집계합니다. Worker join은 2초 기한을 사용하지만 kernel I²C 대기의 절대 종료 시간을 보장하지 않습니다. Monitor는 기본 OFF이고 추가 thread를 만들지 않습니다.

현재 주기와 통신량에는 pipe·mutex/condition이 충분해 Shared Memory·Semaphore·자동 respawn을 추가하지 않았습니다. Kernel lease는 모터 보호이며 시스템 재부팅 watchdog은 아닙니다.

[실행](build-run.md) · [검증](review.md)
