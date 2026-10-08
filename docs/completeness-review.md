# 완성도 검토 — BMP180 통합 후

**후속 수정/재검증 완료:** [final-review.md](final-review.md)의 AUTO 경쟁 조건은
kernel의 원자적 SET_AUTO와 보호 정지 latch로 수정했다. 현재 Kbuild도 정상이며
module/W=1, C6개/CLI25 tests를 통과했다. LCD 종료2초 join과 sensor exec 진단도
보완했다. 아래는 초기 통합 검토 기록이며, 실물 확인은 [최종 테스트](final-test.md)를 따른다.

2026-10-08. 엔코더 실물 동작은 사용자 요청에 따라 이번 완료 범위에서 제외한다.
아래는 코드·테스트·실물 관찰의 구분이며, 모든 전기 특성이나 장기 내구성 검증을
완료했다는 뜻은 아니다.

| 요구 기능 | 구현 / 검증 | 남은 실제 확인 |
|---|---|---|
| Motor ON/OFF, PWM5단계 | Kernel GPIO/PWM consumer와 CLI. 사용자 회전 성공 보고 | 단계별RPM/전류/발열 정량 측정 |
| 정지/lease/max-on | Kernel worker, close/remove/PM 정지. CLI 회귀 검증 | 강제종료/절전 실물 조합 시험 |
| LED Bar | GPIO8개, 시험/자동 표시. 사용자8칸 점등 성공 보고 | 실제 AUTO 풍속 연동 시험 |
| LCD | 비동기 worker, MANUAL/AUTO·온도·풍속 표시, 행별 갱신. 사용자 문자 표시 보고 | 3.3V에서 대비 부족; AUTO 실제 화면 확인 |
| BMP180 | 실제온도/기압 + live process IPC 성공. Bosch 보정식 기준값 검증 | 기준 온도계/기압계 대조 |
| 온도 AUTO | 구간별0~5단계,1°C hysteresis, 명시적 arm/OFF, 센서 오류/stale 정지 | 실제 온도 변화에 따른 모터/LED/LCD 통합 시험 |
| Multi-process / IPC | fork+exec sensor child,32-byte pipe frame, nonblocking partial reads, signal/waitpid | 반복 실행/센서 탈착 시 실물 확인 |
| Multi-thread / 동기화 | LCD worker, mutex/condition, 최신 상태 coalescing, motor stop 후 join | LCD 통신 고장 시 실물 확인 |
| Rotary Encoder | source/decoder/diagnostic 있음 | 실물 입력 무응답, 제외/미완료 |
| 고급 확장 | watchdog/Qt/자동 process 복구 등 선택 기능 | 이번 요청 범위에 포함하지 않음 |

## 검토하며 확인/수정한 사항

1. 센서 calibration의 signed/unsigned와big-endian 구분,64-bit 중간값,
   음수 arithmetic shift와 division0/비정상계수/온도·기압 범위를 검사했다.
   Bosch 예제15.0°C/69964Pa와 일치하며 malformed calibration10,000개를
   AddressSanitizer/UndefinedBehaviorSanitizer로 검사했다.
2. I²C register read는 repeated-start I2C_RDWR로 수행하고 exact message count를
   검사한다. ID0x55를 먼저 확인해다른 sensor에 conversion 명령을 쓰지 않는다.
   I2C_SLAVE_FORCE는 사용하지 않으며 cooperative sensor lock을 사용한다.
3. Parent가 모든 모터 정책/명령을 소유한다. Child는BMP만 읽고 GPIO나motor
   fd를 사용하지 않는다. fork 후exec 전에는async-signal-safe 연산만 사용하며
   기존 fd의CLOEXEC를 유지한다. Sensor와LCD는서로다른주소로동일bus를 공유한다.
4. IPC의partial read, EAGAIN,EOF,잘못된sequence/future timestamp/압력 범위,
   error frame 및fork/exec/종료정리를검사했다. 실제IPC sample age34ms도확인했다.
5. AUTO 정책이최대ON/lease 정지뒤hot sample로재시작하는경로를차단했다.
   센서복구만으로재시작하지않으며사용자의ON이필요하다. 수동OFF도arm을해제한다.
6. PM suspend가USER와같은stop reason이었던점을구분했다. 새SUSPEND 사유로
   AUTO 재시작을차단하고sample age는절전시간을포함하는CLOCK_BOOTTIME을사용한다.
   실제system suspend 시험은수행하지않았다.
7. Kernel GPIO/PWM 변경은mutex를소유한process/workqueue 문맥에서한다.
   LCD I/O lock은publish snapshot 복사시만보유해Main heartbeat를막지않는다.
8. Driver ABI 기존layout/ioctl은보존했다. 후속 수정에서 STOP_SENSOR,
   SET_AUTO 및 CAP_AUTO도 추가했다. 새module을적재해야보완이실제적용된다.
   기존핀/DT를변경하지않았다.

## 실행한 검증

- Native app/module/overlay build, module W=1 및vermagic 대조.
- Encoder/LCD/BMP180/sensor IPC의4개C test 프로그램과CLI25 tests.
- BMP180 math의ASan/UBSan 검증 및변경한userspace 소스의GCC analyzer.
- 실제BMP180 단독read와binaryIPC read 성공. 동시에두번째sensor reader가접근하면
  cooperative lock으로거부되는것도확인했다. 모터를켜는새AUTO 실물시험은미수행.
- 현재LCD용100kHz DT가이미적용돼BMP 구현을위한추가reboot/DT는필요없다.

**판정:** 엔코더를제외한필수기능의코드는구현돼있다. 발표/시연용완료판정에는
실제AUTO 통합동작,센서오류정지,재시작방지,프로그램종료정지를한번씩확인해야한다.
LCD 시인성문제도기능구현과별도로남아있다.
