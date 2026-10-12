# 시스템 설계

## 역할과 소유권

| 구성                   | 책임                                                  | 소유 자원                                              |
| ---------------------- | ----------------------------------------------------- | ------------------------------------------------------ |
| smartfan kernel driver | 출력, 원자적 AUTO 갱신, lease/max-on/boost, 보호 정지 | ENA/IN2 GPIO, PWM1, LED GPIO 배열, mutex, delayed work |
| fanctl main            | 초기화·입력 poll·heartbeat 스케줄·종료 정리           | 단일 /dev/smartfan fd, GPIO event fd, poll/self-pipe   |
| controller.c           | MANUAL/AUTO 정책, 명령 해석, 상태 출력/LCD snapshot   | 제어 상태, 센서 최신 sample, arm/disarm                |
| fan-device.c           | ioctl 및 dry-run 상태 모델                            | main이 관리하는 fd 참조, dry-run deadlines             |
| bmp180-monitor child   | sensor ID/calibration 확인, 온도·기압 보정, 주기 측정 | I²C fd, cooperative flock, pipe stdout                 |
| LCD worker thread      | LCD 초기화, 변경된 행 전송                            | 별도 I²C fd, snapshot mutex/condition                  |

```mermaid
flowchart TD
    CMD[stdin · CR/LF/CRLF] --> MAIN[fanctl main · poll]
    ENC[GPIO v2 · offsets 50/52] --> MAIN
    SIG[signal flag + self-pipe] --> MAIN
    CHILD[BMP180 child · fork/exec] --> IPC[fixed 32-byte pipe · nonblocking reader]
    IPC --> MAIN
    MAIN --> CONTROL[controller · MANUAL/AUTO/commands]
    CONTROL --> DEVICE[fan-device · ioctl / dry-run]
    DEVICE --> KERNEL[/dev/smartfan · ioctl]
    KERNEL --> LOCK[driver mutex · current state / deadlines]
    LOCK --> OUT[ENA / IN2 / PWM1 / LED]
    WORK[delayed work] --> LOCK
    PM[suspend / remove / close] --> LOCK
    MAIN --> SNAP[LCD snapshot · mutex / condition]
    SNAP --> LCD[LCD worker · I²C]
```

GPIO/PWM provider는 기존 Linux/Tegra driver를 사용합니다. 애플리케이션과 커널 driver가 같은 모터 자원을 중복 요청하지 않습니다. IN1은 PWM provider가 소유하며 GPIO로 함께 요청하지 않습니다. Encoder는 사용자 공간 GPIO v2 입력 요청으로 받습니다.

## 동작 정책

- MANUAL: `on`이 기동하고 `speed 0`/로터리0/`off`가 정지합니다. OFF에서 풍속을 바꿔도 기동하지 않습니다. 풍속0 뒤의 명시적 ON은 마지막 nonzero 풍속을 사용합니다.
- AUTO: `mode auto`는 OFF/disarm, `on`은 arm입니다. 온도가 낮아 target0이면 OFF로 대기하며 온도 상승 시 기동할 수 있습니다. 명시적 OFF·모드 전환·센서 fault/stale·보호 정지는 disarm합니다.
- 센서가 정상으로 돌아온 것만으로 보호 정지 뒤 재기동하지 않습니다. 현재 유효 sample을 확인한 명시적 ON이 필요합니다.
- AUTO에서는 로터리 입력이 목표 풍속을 덮어쓰지 않습니다.

| 초기 선택 온도 | 단계 | PWM 목표 | 구동 중 LED |
| -------------- | ---- | -------- | ----------- |
| <24°C          | 0    | 0%       | 0           |
| 24~26°C        | 1    | 60%      | 2           |
| 26~28°C        | 2    | 70%      | 4           |
| 28~30°C        | 3    | 80%      | 5           |
| 30~32°C        | 4    | 90%      | 7           |
| ≥32°C          | 5    | 100%     | 8           |

하강은 해당 경계보다1°C 낮아져야 합니다. 예: 단계2는25.0°C까지 유지하고 그 미만에서 단계1로 내려갑니다. 이 값은 시연용 기본 정책이며 실제 RPM/열 제어 성능을 교정한 값이 아닙니다.

## 커널 정지와 동시성

출력 변경은 mutex를 가진 process/workqueue 문맥에서 수행하며 GPIO `_cansleep`/PWM API의 sleeping을 허용합니다. 정지는 ENA를 먼저 LOW로 만들고 IN2/LED/PWM을 정리합니다. PWM disable 자체의 출력 레벨에 의존하지 않습니다.

연속 ON deadline은 풍속 변경·반복 ON·heartbeat로 연장하지 않습니다. Lease는 heartbeat 또는 유효한 구동 갱신으로 유지합니다. 하나의 delayed worker가 boost/lease/max-on 중 가장 가까운 deadline을 처리하고, ioctl에서도 만료를 검사합니다. Worker scheduling은 hard real-time deadline 보장이 아닙니다.

SET_AUTO는 같은 mutex 안에서 보호 정지 latch와 BOOTTIME sample age를 확인합니다. 자동 갱신은 rearm=0, 명시적 ON만 rearm=1입니다. PWM 설정 뒤에도 만료/freshness를 재검사합니다. 기존 SET/GET 구조와 ioctl 번호는 보존한 additive ABI입니다.

`misc_deregister`와 driver lock 순서를 분리하고, remove는 removed flag를 설정해 이후 fd의 devm 자원 접근을 차단합니다. kref가 마지막 fd까지 상태 객체의 수명을 유지합니다. delayed work 취소는 mutex 밖에서 기다립니다.

## IPC와 종료

센서 IPC는 magic, sequence, errno, temperature(m°C), pressure(Pa), reserved, BOOTTIME timestamp를 가진32-byte native-local frame입니다. 외부 네트워크 wire protocol이 아닙니다. Partial read/EAGAIN/잘린 EOF/범위/순번을 검사합니다. 협력적인 sensor reader끼리는 flock으로 같은 센서 conversion을 중복 수행하지 않습니다. LCD는 다른 주소를 사용하며 이 lock에 참여하지 않습니다.

Fork caller는 control signal을 잠시 차단합니다. Child는 상속된 main 핸들러를 default로 바꾸고 signal을 해제한 뒤 exec합니다. Exec 이후 worker가 자체 핸들러와 PR_SET_PDEATHSIG를 설정합니다. 실패한 exec는 errno frame을 보냅니다. 단순 프로세스 개수 증가가 목적이 아니라, sensor I²C blocking을 heartbeat 경로에서 분리하기 위한 구조입니다.

종료 순서는 motor OFF/fd close → sensor SIGTERM/필요 시 SIGKILL·waitpid → LCD final OFF/join입니다. LCD worker는 control signal을 차단하고 main이 signal flag/self-pipe를 소유합니다. Join은 MONOTONIC 기준2초를 기다리며 timeout 후 살아 있는 worker 객체를 free하지 않습니다. Kernel I²C의 uninterruptible 작업까지 종료 시간을 보장하지는 않습니다.

## 성능·유지보수 판단

Main은50ms poll 주기, heartbeat는 lease/4(최대250ms), sensor는 측정 후 약1초 대기, encoder는 read당최대32events입니다. LCD는 최신 snapshot을 coalesce하고 달라진 행만 전송합니다. 정상 sensor 값은 반복 출력하지 않아 명령 화면을 보존합니다.

현재 규모에서 epoll, 공유 메모리, 추가 daemon 또는 대규모 클래스 구조로 바꿀 근거는 없습니다. `main.c`는 실행 흐름과 자원 수명, `controller.c`는 MANUAL/AUTO 정책과 명령 처리, `fan-device.c`는 ioctl과 dry-run 모델을 담당합니다. 기존 단일 poll 루프와 커널 보호 정책은 유지합니다.

Process 생성·IPC·종료 status 및 회수 지연 시 PID 보존의 상세 검증은 [6단계](stage6-process-ipc.md)를 참조합니다.
