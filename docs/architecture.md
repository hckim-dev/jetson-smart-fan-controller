# Software Architecture 및 Build 설계

상태: 1단계 software 구현 및 Build 완료. 사용자 지시에 따라 작성/Build를 먼저 진행했고 실제 구동은 미검증이다.
Software Architecture / Kernel Driver / Build Integration / Test Reliability / Hardware Safety를
세 서브에이전트와 주 에이전트가 검토하여 통합했다.

## 1단계 최소 구조

```mermaid
flowchart TD
    CLI["foreground C CLI: ON/OFF · heartbeat · 종료 처리"]
    DEV["/dev/smartfan: 단일 controller fd"]
    DRV["custom platform driver + miscdevice\nGPIO 소유 · 상태 mutex · lease · 강제 OFF"]
    DT["프로젝트 DT node\nGPIO descriptors / polarity"]
    GPIO["기존 Tegra GPIO provider"]
    BUF["신호 전압 / 초기 OFF 회로 검증\n직결 또는 buffer 선택 미확정"]
    MOTOR["L298N → EZ MOTOR R300: 적합성 미확정"]
    CLI --> DEV --> DRV --> GPIO --> BUF --> MOTOR
    DT --> DRV
```

직접 구현한 Driver는 **실제 모터 신호 출력과 정지 정책**을 맡는다.
기존 Tegra GPIO/PWM/I²C controller Driver는 그대로 재사용한다.
Userspace는 모드·풍속 정책·센서·화면을 담당하고 GPIO를 Driver와 중복 점유하지 않는다.

- `platform_driver` + DT의 `enable-gpios`, `in1-gpios`, `in2-gpios`로 배선 표현.
- `miscdevice`로 동적 minor의 character device 제공. fixed major를 만들 필요 없음.
- 소수의 `ioctl()`로 상태 조회/ON/OFF/heartbeat 제공. fixed-width UAPI, 범위 검사, ABI version 명시.
- 단일 제어 fd만 허용, 두 번째 open은 `EBUSY`. CLI는 `O_CLOEXEC` 사용.
- EN을 LOW로 먼저 요청한 뒤 방향을 설정. ON은 방향 설정 후 EN 활성화, OFF는 EN 비활성화부터 수행.
- polarity는 DT active-high 조건으로 확인. 논리 LOW와 실제 전압 LOW를 혼동하지 않는다.
- 정상 close, probe 실패, remove/unload, lease 만료 시 OFF. 재연결/heartbeat만으로 자동 재시작하지 않는다.

`fanctl on` 후 CLI가 즉시 종료하면 fd release가 OFF시키므로,
1단계 CLI는 foreground로 fd를 유지하며 명령을 받는 형태로 설계한다.
신호 handler에서는 flag만 설정하고 main loop에서 ioctl/close를 수행한다.
lease 시간과 최대 연속 ON 시간은 구현 단계에서 명시하고 시험한다.

## Concurrency / lifetime / 장애

- owner/state/deadline/removed를 하나의 mutex로 보호한다.
- GPIO `_cansleep` 및 PWM API는 process/workqueue context에서 실행한다.
- `delayed_work`가 wraparound-safe deadline을 검사하여 OFF. timer/IRQ context에서 sleeping GPIO를 호출하지 않는다.
- OFF 후 오래된 heartbeat로 다시 ON되지 않도록 한다. 이전 session의 work가 새 owner의 deadline을 취소하지 않도록 현재 상태를 재확인한다.
- 열린 fd는 `.owner = THIS_MODULE`로 정상 module unload를 막는다. platform unbind 수명은 별도로 `kref`/removed 상태로 처리한다.
- remove는 removed 표시와 OFF → mutex 해제 → device 등록 해제 → work 동기 취소 → 참조 해제 순서를 검토한다.
- state mutex를 잡은 채 `misc_deregister()`/`cancel_delayed_work_sync()`를 호출하지 않는다. fd callback은 removed 이후 하드웨어 접근을 거부한다.

SSH 단절 자체를 kernel lease가 감지하는 것은 아니다. 남은 process가 heartbeat를 계속 보내면 ON도 지속될 수 있다.
foreground HUP/EOF 처리 + kernel lease + 갱신으로 무한 연장되지 않는 최대 ON 시간을 조합한다.
SIGKILL은 마지막 fd가 닫혀야 release가 실행되므로 fork/dup 상속도 주의한다.
Kernel hang/전원 장애는 software timeout으로 보장할 수 없다.
L298N EN 기본 OFF 회로 및 전원 순서 검증이 필요하다. GPIO 직결/외부 buffer 선택은 전압 검사 결과로 판단한다.
OFF는 전기적 구동 중단이며 날개의 즉시 정지와 다르다.

API 근거: 현재 headers의 `include/linux/{gpio/consumer.h,pwm.h,platform_device.h,miscdevice.h,workqueue.h,kref.h}`.
[GPIO descriptor 문서](https://docs.kernel.org/5.15/driver-api/gpio/consumer.html),
[PWM 문서](https://docs.kernel.org/5.15/driver-api/pwm.html).
현재 vendor headers에는 `.remove_new` 및 `pwm_apply_state()`가 존재한다.

## 확장 방향

| 단계 | 선택 방향 / 이유 |
|---|---|
| Encoder/PWM | Encoder upstream `rotary-encoder`의 별도 module build를 먼저 검토. 불가하면 userspace GPIO edge/Gray decoding. 현재 kernel에 driver 없음 |
| PWM | hardware PWM 우선. EN GPIO interlock을 유지하고 IN1을 GPIO→PWM으로 전환하는 후보. 같은 pin의 GPIO/PWM 동시 점유 금지. OFF는 EN LOW로 보장하고 실제 파형 확인 |
| LCD | chip/pin 연결 확인 후 i2c-dev userspace. 이미 kernel Driver가 점유한 주소와 병행 접근 금지 |
| LED BAR | 강의 LED+330Ω array 방식으로 한 칸부터 평가. 추가 Driver는 밝기/전류/반복 동작 검증 결과로 선택 |
| BMP180 | 온도 기반 AUTO에 적합, 습도 기능 없음. upstream `bmp280-i2c`의 BMP180 지원 재사용 우선; 현재 미설정이므로 matching source/module 가능성 확인. 대안은 보정식을 검증한 i2c-dev 접근 |
| Multi-process | 통합이 안정된 후 controller만 모터 fd 소유. display/logger 등 분리 필요가 생길 때 bounded IPC 도입 |
| Multi-thread | 센서/표시 I/O가 이벤트 처리를 막는 근거가 있을 때 도입. 처음부터 thread 추가하지 않음 |
| 고급 확장 | 둘째 날 안정 버전 확보 후 판단. Qt/Shared Memory 등은 필수 아님 |

Upstream 근거: [rotary_encoder.c](https://raw.githubusercontent.com/torvalds/linux/v5.15/drivers/input/misc/rotary_encoder.c),
[bmp280-i2c.c](https://raw.githubusercontent.com/torvalds/linux/v5.15/drivers/iio/pressure/bmp280-i2c.c).

## Repository / Build 계획

현재 1단계 구조는 아래와 같다.

```text
driver/smartfan.c       platform/miscdevice Driver
driver/Kbuild          external module Build
include/smartfan_uapi.h fixed-width ioctl ABI
app/fanctl.c           foreground CLI / dry-run
dts/smartfan.dts        device + 세 motor pad의 output pinctrl overlay
tests/test_fanctl.py    dry-run CLI integration test
Makefile               app / module / dt / test
.gitignore             산출물 제외
scripts/prepare-dt.py   현재 boot 설정 기반 offline DT/config 준비
scripts/install-dt.sh   승인된 DT 설치, hash검증/backup/atomic publish
docs/                  조사 · 배선 · Build/Run · 상태
```

`KDIR ?= /lib/modules/$(uname -r)/build`와 external Kbuild `make -C $(KDIR) M=<driver> modules`를 사용한다.
Native C CLI는 GCC로 warning을 켜고 빌드한다. 다른 버전 source를 대체하지 않는다.
DT compile과 실제 boot 설치를 분리하고, 기본 build target은 sudo/load/설정을 수행하지 않는다.
`.ko`, `.o`, `.mod*`, `.cmd`, `Module.symvers`, `modules.order`, `.dtbo`, 실행 바이너리는 제외한다.
`/dev`, `/sys`, `/proc`, `/boot`는 runtime/system 상태이며 repository source와 구분한다.

## 구현한 1단계 동작

- Driver 기본 lease2초, 갱신 불가능한 연속 ON 상한30초. DT에서 제한 범위 내 설정 가능.
- 반복 ON은 lease만 갱신하며 기존30초 deadline은 유지. 만료 후 새 ON 명령을 명시적으로 입력하면 새 run 시작.
- Heartbeat는 OFF 상태를 ON으로 만들지 않음. GET/SET/heartbeat에서도 deadline을 확인해 늦은 work 처리에 따른 복구를 막음.
- fd release/suspend/shutdown/remove에서 OFF 요청, resume 자동 재시작 없음.
- CLI `poll()` + self-pipe와 분할 `read()`로 입력을 처리하고 250ms 이하 간격으로 heartbeat.
- 부분 명령/초과 길이/NUL을 완성된 ON 명령으로 오인하지 않음. EOF와 SIGINT/TERM/HUP에서 OFF 요청 및 close.
- Worker는 hard real-time 정지를 보장하지 않는다. Kernel hang/전원 문제에 대한 보장은 별도 회로 영역이다.
- `--dry-run`은 userspace 모사이며 실제 Driver의 workqueue/GPIO/정지 시험을 대체하지 않는다.
- 사용자 pad 조회로 high-Z 설정을 확인하여 GPIO output용 default pinctrl state를 추가했다.
