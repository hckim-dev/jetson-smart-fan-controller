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

## 프로세스·IPC와 종료

Sensor child는 I²C fd/센서 lock과 pipe write end를, main은 read end/PID와 모터 fd를 소유합니다. `pipe2(O_CLOEXEC)`→fork→dup2(stdout)→execv로 시작합니다. Fork caller는 제어 신호를 차단하고 child는 상속한 handler를 default로 바꾼 뒤 mask를 복원합니다. Exec 전에는 async-signal-safe 연산만 수행합니다. 모터/GPIO/self-pipe fd는 exec 자식에 전달하지 않습니다.

Sample은 native-local 고정32바이트이며 magic/sequence/errno/온도(m°C)/기압(Pa)/reserved/BOOTTIME을 포함합니다. Partial read·EAGAIN·중간 EOF·범위·순번을 검사합니다. Exec 실패도 errno frame으로 전송합니다. Child는 parent-death SIGTERM과 parent PID 재검사로 부모의 갑작스러운 종료를 처리합니다. 센서 reader의 flock은 협력적 중복 conversion 방지이며 LCD는 참여하지 않습니다.

센서 I²C 대기를 heartbeat 경로와 분리하기 위해 process/pipe를 사용합니다. Main만 모터/모드를 변경하며 fault/stale은 AUTO를 disarm합니다. 정상 sample만으로 재기동하지 않고 child 자동 respawn도 없습니다. 센서 자원 정체를 해결했다고 가정해 임의로 새 child를 생성하지 않습니다.

종료는 motor OFF/fd close→sensor SIGTERM→필요 시 SIGKILL/waitpid→pipe close→LCD join→log drain/join 순서입니다. Read end를 먼저 닫으면 전송 중 child가 SIGPIPE로 종료될 수 있어 회수 시도까지 유지합니다. 종료 신호가 blocked write를 중단한 경우 monitor는 정상 정리로 처리합니다. 자식 회수 기한을 넘기면 PID를 보존하고 실패를 표시합니다. Uninterruptible I²C 대기는 SIGKILL로 즉시 끝난다고 보장할 수 없습니다.

## 스레드·로그 동기화

LCD는 한 mutex 아래에서 최신 snapshot을 복사한 뒤 lock 밖에서 I²C를 수행합니다. 변경된 행만 전송하며 publish는 전송을 기다리지 않습니다. 종료 후 새 publish는 무시합니다. MONOTONIC 기준2초 join이 실패하면 살아 있는 객체를 free하지 않습니다.

선택형 log는 128개×256바이트 FIFO이며 record를 복사합니다. Queue/head/count/stopping/error/counter는 mutex로 보호하고 condition predicate는 while로 검사합니다. File write/close는 lock 밖에서 writer만 수행하며 partial write/EINTR을 처리합니다. 포화 시 새 record를 버리고 dropped를 올려 제어 경로가 queue 공간을 기다리지 않게 합니다. Regular append-only 파일만 허용하고 symlink/FIFO/device는 거부합니다.

Log join은 tryjoin과 2초 MONOTONIC 기한을 사용하고 실패 시 살아 있는 객체를 유지합니다. Producer는 join 전에 중단합니다. Worker는 controller/device 상태를 직접 읽거나 변경하지 않습니다. Control signal은 worker 생성 시 mask로 차단하며 signal handler는 flag/self-pipe만 사용합니다. Log/Mutex/condition API를 handler에서 호출하지 않습니다.

Written은 완전한 record의 write 완료이며 fsync나 전원 장애의 영속성 보장이 아닙니다. 부분 쓰기 후 실패하면 파일 끝에 불완전한 record가 남을 수 있습니다. Rotation은 없으므로 장기 운전 시 용량을 관리해야 합니다. Queue 포화·SIGKILL·쓰기 실패에는 종료 summary도 보존을 보장하지 않습니다.

## 운영 가시성과 확장 판단

Session counter는 main 소유이며 terminal 통지용 상태와 별도로 장애 구간을 관찰합니다. CLI Monitor는 main에서 heartbeat 뒤에 기한을 검사하고 기본 OFF이며 추가 thread를 만들지 않습니다. Logger snapshot만 기존 mutex 아래에서 복사합니다. 명령과 통계 정의는 [빌드·실행](build-run.md)을 따릅니다.

Monitor는 여러 ioctl/sensor/log 조회를 조합하므로 전체 시스템의 원자 snapshot이 아닙니다. 출력은 논리 상태이며 실제 RPM/회전의 측정값이 아닙니다. 최종 조회에서 OFF이면 duty를0으로 표시합니다. Stdout/stderr는 동기식이므로 느린 터미널/파이프의 hard real-time 지연을 보장하지 않습니다.

Main은50ms poll, heartbeat는 lease/4(최대250ms), 센서는 측정 후 약1초 대기, encoder는 호출당최대32events입니다. 정상 sensor sample은 반복 출력하지 않으며 monitor도 사용자 요청 때만 켭니다. 현재 규모에서 Shared Memory/Semaphore/하드웨어 watchdog/자동 복구를 추가할 근거보다 검증 비용이 큽니다. Kernel lease는 모터 제어 중단 보호이며 시스템 재부팅 watchdog과는 다릅니다.
