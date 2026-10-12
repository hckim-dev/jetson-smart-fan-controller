# 7단계: Multi-Thread / Synchronization

## 역할 분리

현재 6단계의 센서 프로세스와 단일 제어 이벤트 루프를 유지하고, 필요할 때 파일 기록만 추가 스레드에 맡깁니다.

| 실행 문맥                    | 책임                                                                            | 공유 방식                                           |
| ---------------------------- | ------------------------------------------------------------------------------- | --------------------------------------------------- |
| main thread                  | CLI/encoder/센서 IPC event 처리, MANUAL/AUTO, motor ioctl, heartbeat, 자원 수명 | controller/device 상태를 main만 변경                |
| LCD worker                   | LCD 초기화·변경 행 I²C 전송                                                     | Mutex로 보호하는 최신 snapshot + condition variable |
| log worker (`--log` 선택 시) | append-only 이벤트 파일 기록                                                    | Mutex/condition으로 보호하는 고정 크기 FIFO         |
| sensor child process         | blocking I²C 측정·보정·결과 전달                                                | 6단계의 pipe; thread 공유 메모리 없음               |

센서를 thread로 다시 구현하면 6단계의 격리와 pipe 구조가 중복됩니다. Event Processing을 여러 thread로 나누면 모터/모드 상태를 동시에 변경하고 순서를 재조정해야 하므로 현재 단일 owner를 유지합니다. 파일 I/O는 지연·실패할 수 있으므로 선택형 worker로 분리합니다. `--log`를 생략하면 추가 log thread/file이 생성되지 않고 기존 실행을 유지합니다.

## 동기화와 수명

- `pthread_create()` 전 main에서 SIGINT/SIGTERM/SIGHUP를 차단해 worker가 mask를 상속하도록 하고 main mask는 복원합니다. 신호 handler는 flag/self-pipe만 사용하며 Mutex/condition/log API를 호출하지 않습니다. [pthread_create](https://man7.org/linux/man-pages/man3/pthread_create.3.html), [condition API](https://www.man7.org/linux/man-pages/man3/pthread_cond_signal.3.html).
- LCD는 한 Mutex 아래에 전체 snapshot을 복사하고 lock 밖에서 I²C를 수행합니다. 변경 통지는 최신 상태로 합쳐집니다. 종료 후 새 publish는 무시합니다.
- Log는 128개 × 256바이트 ring buffer입니다. submit은 문자열을 복사하며 포인터나 controller 객체를 worker에 넘기지 않습니다. 여러 producer를 지원하지만 실제 controller 이벤트의 producer는 main입니다.
- Queue/head/count/stopping/error/counter는 모두 같은 Mutex 아래에서 접근합니다. Worker는 predicate를 while로 검사하고 condition wait합니다. [POSIX condition wait](https://pubs.opengroup.org/onlinepubs/7908799/xsh/pthread_cond_wait.html).
- File write/close는 queue lock 밖에서 worker만 수행합니다. Partial write와 EINTR을 처리합니다. Queue가 가득 차면 새 record를 버리고 dropped를 올리며 producer를 공간 확보까지 대기시키지 않습니다. 기록 실패 시 경고하고 motor 제어는 계속합니다.
- 종료는 motor OFF/fd close → sensor cleanup → LCD join → STOP event → log drain/join입니다. Producer는 join 전에 중단합니다. Logger의 2초 MONOTONIC deadline은 `pthread_tryjoin_np()`로 회수하며, timeout에는 live 객체를 free하지 않습니다. LCD의 기존 bounded join도 유지합니다.
- Mutex 획득과 scheduling은 hard real-time 지연 보장이 아닙니다. 기존 stdout/stderr 터미널 출력도 동기식입니다. 이번 분리는 추가 파일 기록의 blocking을 제어 루프에서 제거합니다.

## 기록 내용과 한계

```sh
sudo ./build/fanctl --encoder --bmp180 --lcd --lcd-address 0x27 --log build/fan-events.log
```

`boottime_ms=...`와 START/CMD/RESULT/STATE/SENSOR/AUTO_STOP/MOTOR_STOP/SENSOR_EXIT/STOP 이벤트를 기록합니다. STATE는 mode/armed/level/duty/LED와 sensor_valid/온도/기압을 포함합니다. 매 초의 모든 sample이나 heartbeat는 기록하지 않습니다. Terminal 출력 전체의 복제도 아닙니다. 다중 producer의 timestamp는 enqueue 직전에 취득하므로 파일 전체의 timestamp 정렬을 보장하지 않습니다.

기존 파일에 append하며 새 파일 mode는0600(umask 적용)입니다. Regular file만 허용하고 마지막 경로 요소의 symlink와 FIFO/device는 거부합니다. 파일 열기는 실행 초기화에서 수행되며 실패하면 READY 전에 종료합니다. Worker 생성 이후의 write 실패는 제어를 중단하지 않으며 종료 시 실패를 exit status에 반영합니다.

Queue overflow/긴 record/쓰기 실패는 dropped에 반영합니다. `LOG_CLOSED written=N dropped=N error=N`으로 확인합니다. written은 완전한 record의 write 완료이며 fsync나 전원 장애 시 영속성을 보장하지 않습니다. 부분 쓰기 후 실패하면 파일 끝에 불완전한 record가 남을 수 있습니다. Rotation·최대 파일 크기·crash/SIGKILL 시 큐 drain은 구현하지 않았습니다. 장기 운전에서는 용량을 관리해야 합니다. 기본 build 경로는 Git에서 제외됩니다.

## 검증

```sh
make all
make test
make analyze
make sanitize
make thread-check
```

- Log writer를 막아도 producer가 128개 record와 overflow 결과를 반환하는지 검사합니다. 2초 join timeout 후 객체를 유지하고 writer를 풀면 재시도로 정상 회수합니다.
- 4개 producer가 4,000개 record를 제출하고 written+dropped 합계, 개별 producer 순서, 완전한 행, concurrent stats를 확인합니다.
- Partial write/EINTR, newline escaping, 너무 긴 record, ENOSPC, pthread_create 실패, special-file 거부를 검사합니다. Worker의 control signal mask와 main의 mask 복원도 확인합니다.
- LCD는 3개 publisher의 30,000개 snapshot과 concurrent observer를 실행해 field 일관성과 stopping 뒤 갱신 거부를 검사합니다. 실제 LCD worker의 I²C/종료 경로는 기존 mock tests가 담당합니다.
- CLI는 기존 파일 append, log+LCD+encoder+sensor dry-run, 정상 종료 drain, SIGTERM drain, 잘못된 log destination의 기동 거부를 확인합니다.
- ThreadSanitizer는 log와 LCD snapshot 스트레스 경로를 검사합니다. 실제 하드웨어의 모든 경로를 검증하거나 가능한 모든 interleaving에서 race가 없음을 증명하는 것은 아닙니다.

초기 TSan 실행은 종료된 log thread를 leak으로 보고했습니다. Native clockjoin은 성공했지만 이 환경 GCC11 libtsan의 exported interceptor에는 clockjoin이 없고 join/tryjoin/timedjoin만 있음을 확인했습니다. 새 log module의 bounded join을 monotonic deadline+tryjoin으로 변경했고 같은 테스트에서 thread leak과 data race 경고 없이 통과했습니다. suppression이나 leak 검사 비활성화를 사용하지 않았습니다.

## 사용자가 확인할 작업

현재 정상 배선·전원에서 위 통합 명령을 실행합니다. DT 설치·재부팅·module 재적재는 필요하지 않습니다.

1. `mode manual`, `speed 5`, `on`, `status`를 입력하고 로터리를 돌려 기존 출력·입력을 확인합니다.
2. `off`, `quit`로 종료합니다. `LOG_CLOSED ... dropped=0 error=0`을 확인합니다.
3. `sudo tail -n 20 build/fan-events.log`로 명령·상태·STOP 기록을 확인합니다.
4. 다시 실행해 Ctrl+C 종료에서도 모터가 멈추고 LOG_CLOSED가 표시되는지 확인합니다.
5. 실행 중 thread를 보려면 다른 터미널에서 `ps -T -p "$(pgrep -n -x fanctl)" -o pid,tid,stat,comm`를 실행합니다. main+LCD+log로 정상 운전 시3개 thread이며 센서는 별도 process입니다. 옵션을 줄이면 해당 worker 수가 줄어듭니다.

문제가 있으면 `LOG_CLOSED`, WARN LOG/LCD, 실행 명령과 관찰 결과를 전달합니다. `--log`를 생략하면 기존 기능으로 실행할 수 있습니다. 실물 성공은 사용자 확인 후 기록합니다.

## 이번 검증 결과

Jetson aarch64 / GCC11.4.0 / glibc2.35에서 native build, C11개·CLI32개·clean 보존1개, 사용자 소스12개 analyzer, ASan/UBSan9개, ThreadSanitizer2개가 통과했습니다. Dry-run 통합 실행에서 `/proc/PID/task`의 main+LCD+log 3개 thread를 관찰하고 정상 종료, dropped=0/error=0 및16개 log record를 확인했습니다. 하드웨어에는 접근하지 않았습니다.

최종 로그는 로컬 `build/stage7-final.log`, `build/stage7-thread-check.log`, `build/stage7-runtime-smoke.json`에 보존합니다. 초기 TSan 경고 로그도 `build/stage7-tsan.log`에 남겼습니다. 실물 통합은 이번 변경 후 아직 사용자 확인을 받지 않았습니다.
