# 8단계: 운영 통계와 선택형 CLI Monitor

## 확장 선택

남은 일정의 구체적인 시간 제한은 제시되지 않았으므로, 기존 동작을 보존하면서 검증할 수 있는 운영 가시성을 우선했습니다. 제어 구조를 추가로 분산하거나 boot/system 설정을 바꾸지 않습니다.

| 기술                  | 판단                              | 이유                                                                                                |
| --------------------- | --------------------------------- | --------------------------------------------------------------------------------------------------- |
| Event Logging 및 통계 | 7단계 logging에 session 통계 추가 | 오류 발생·복구·누락과 입력량을 실행 중 확인 가능                                                    |
| CLI Monitor           | 구현                              | mode/arm/출력/센서/child PID/보호 deadline/log 상태를 선택적으로 관찰                               |
| Watchdog              | 기존 kernel lease 보호 유지       | heartbeat가 끊기면 모터 정지; 시스템 전체를 재부팅하는 hardware watchdog과는 다름                   |
| Process 자동 복구     | 보류                              | 제한된 backoff·old child 회수·I²C ownership·AUTO disarm·thread 생성 이후 fork 경로의 추가 검증 필요 |
| Shared Memory         | 미도입                            | 약1초 주기의 단방향 sample에 기존32-byte pipe가 충분함                                              |
| POSIX Semaphore       | 미도입                            | 공유 메모리 producer/consumer가 없고 기존 Mutex/condition이 thread 공유를 보호함                    |

자동 프로세스 복구가 모터 자동 재시작을 허용한다는 의미가 되어서는 안 됩니다. 현재 센서 fault/stale 뒤 명시적 ON을 요구하는 정책을 유지하고 sensor child를 자동 respawn하지 않습니다. 구현하지 않은 기능은 완료 기능으로 기록하지 않습니다.

## 실행과 명령

기존 정상 배선·전원을 유지합니다. Module/DT 설치나 재부팅이 필요하지 않습니다.

```sh
sudo ./build/fanctl --encoder --bmp180 --lcd --lcd-address 0x27 --log build/fan-events.log
```

fanctl 안에서 다음 명령을 사용합니다.

| 명령          | 동작                                        |
| ------------- | ------------------------------------------- |
| `stats`       | session 통계와 logger snapshot을 한 번 출력 |
| `monitor`     | 즉시 한 줄, 이후 약1000ms마다 상태 출력     |
| `monitor 500` | 약500ms 주기, 허용 범위500~10000ms          |
| `monitor off` | 주기 출력 중단; 모터/모드 상태는 유지       |
| `status`      | 기존 상세 상태 출력                         |

Monitor 기본값은 OFF입니다. 명령 결과를 읽기 어려우면 `monitor off`를 입력합니다. Monitor는 main의 기존 poll loop에서 실행하며 별도 thread/process를 만들지 않습니다. Heartbeat 처리 다음에 실행하고, 출력 지연·suspend 뒤 지난 주기만큼 몰아서 출력하지 않습니다. 주기 변경이나 출력은 ON deadline을 갱신하지 않습니다.

`MONITOR`는 mode/armed/running/선택 level/duty, sensor health, 온도(m°C)/기압(Pa), child PID, stop_reason/lease/on remaining, log dropped/error를 표시합니다. Temperature/pressure는 `sensor=valid`일 때만 유효합니다. Pending/error/stale/disabled에서는 마지막 값 또는0일 수 있습니다. Sensor health는 sample 상태이며 child PID와 별도로 봅니다. Running이면 stop_reason=none, OFF이면 실제 stop reason을 표시합니다.

출력값은 driver의 logical state와 sensor sample snapshot입니다. 실제 RPM/모터 회전/전기적 출력의 측정값이 아닙니다. 여러 ioctl과 logger/sensor 조회가 전체 시스템의 단일 원자 snapshot을 보장하지는 않습니다. STOP이 조회 사이에 발생하면 최종 OFF 상태에 맞춰 duty를0으로 표시합니다.

## 통계 의미

| 필드                     | 의미                                                                    |
| ------------------------ | ----------------------------------------------------------------------- |
| uptime_ms                | 실행 초기화 시점부터 BOOTTIME 경과; suspend 시간을 포함                 |
| commands                 | 빈 줄을 제외한 수신 명령 수; help/stats/monitor/quit/잘못된 명령도 포함 |
| sensor_frames            | IPC 검증을 통과한 frame 수; 센서 error frame 포함                       |
| injected_samples         | dry-run `temp` 명령으로 넣은 sample 수; 실제 IPC frame과 구분           |
| sensor_faults            | 관찰한 error/stale 장애 구간에 진입한 횟수                              |
| sensor_recoveries        | error/stale에서 valid로 복구한 횟수; 모터 재기동 횟수가 아님            |
| encoder_cw / encoder_ccw | decoder 방향 반전을 적용한 step 수; AUTO에서 무시한 실물 step도 포함    |
| LOG_STATS                | enabled/written/dropped/queued/error를 logger mutex 아래에서 복사       |

센서가 계속 오류이거나 error와 stale 사이에서 바뀌면 같은 장애 구간으로 봅니다. Valid로 복구한 뒤의 새로운 장애만 새로 셉니다. 출력의 중복 억제와 통계용 health 상태는 별도로 관리해 status 출력이 카운트를 바꾸지 않습니다.

`up/down`은 encoder counter에 포함하지 않습니다. Dry-run에서 허용된 `cw/ccw`는 simulation step으로 셉니다. Counter는 선택 level의 변화량, 물리적 각도나 IRQ edge 수가 아닙니다. 포화 경계에서 level이 변하지 않아도 decoded step은 증가합니다.

통계는 main thread가 소유하며 worker가 읽거나 변경하지 않습니다. 재실행하면 초기화하고 파일이나 공유 메모리에 누적 저장하지 않습니다. `--log`를 사용하면 종료 시 SESSION_STATS(commands/frames/faults/recoveries) record를 기존 큐에 넣습니다. 큐 포화·쓰기 오류·SIGKILL 시 summary 보존은 보장하지 않으며 LOG_CLOSED의 dropped/error도 확인합니다.

## 소프트웨어 검증

```sh
make all
make test
make analyze
make sanitize
make thread-check
```

CLI 회귀에는 opt-in/off, 잘못된 주기 거부, 로그 활성화 통계/summary, 장애 중복·복구·명령·엔코더 counters, monitor 중 heartbeat/max-on 유지가 추가됐습니다. C 테스트는 stale→error를 한 장애로 세고 recovery와 새로운 장애를 별도로 세는지 검사합니다. 기존 AUTO guard·프로세스·LCD·로그 동시성 테스트를 유지합니다.

이 검증은 dry-run/mock/stub이며 실제 I²C/모터 구동은 이번 작업에서 실행하지 않습니다. Stdout/stderr는 기존 동기식 출력이므로 느린 터미널/파이프에서 hard real-time 지연을 보장하지 않습니다.

## 사용자가 확인할 절차

1. 위 명령으로 실행하고 `stats`, `monitor 1000`을 입력합니다. 처음부터 주기 출력이 시작되는 것이 아니라 명령 후 시작돼야 합니다.
2. Sensor가 valid이고 sensor_frames가 증가하는지 확인합니다. `monitor off`, `stats`로 확인할 수도 있습니다.
3. MANUAL에서 `speed 5`, `on`, 로터리 양방향, `status`, `off`를 확인합니다. Counters는 선택 단계가 아니라 decoder step을 셉니다.
4. `monitor off`로 반복 출력이 멈추고 명령은 계속 동작하는지 확인합니다.
5. `quit` 종료 후 `sudo tail -n 20 build/fan-events.log`로 SESSION_STATS와 STOP을 확인합니다.
6. AUTO의 센서 장애·재시작 방지는 기존 [통합 테스트](final-test.md)와 [6단계](stage6-process-ipc.md)를 따릅니다. Recoveries 증가만으로 모터가 켜져서는 안 됩니다.

이번 변경 후 실물 성공은 사용자 확인을 받은 항목만 기록합니다.

## 이번 검증 결과

Native build, C11개·CLI37개·clean 보존1개, 사용자 소스12개 GCC analyzer, ASan/UBSan9개와 ThreadSanitizer2개가 통과했습니다. 추가한 counter/stale 회귀는 일반·sanitizer 실행 모두 통과했습니다. 최종 로그는 로컬 `build/stage8-verification.log`, `build/stage8-final.log`에 보존합니다. Kernel/UAPI/DT/boot 설정은 이번 확장에서 변경하지 않았습니다.
