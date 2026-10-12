# 빌드와 실행

## 검증 환경과 전제

Jetson Orin Nano Engineering Reference Developer Kit Super, aarch64, Ubuntu22.04, L4T36.5.2, kernel5.15.199-tegra, GCC11.4.0, glibc2.35에서 native build했습니다. 필요한 도구는 GCC/Make, matching kernel headers, cpp/dtc/fdtoverlay/fdtget/fdtput, Python3입니다. Runtime C 프로그램은 Python 라이브러리나 Jetson.GPIO를 사용하지 않습니다.

KDIR 기본값은 `/lib/modules/$(uname -r)/build`입니다. 준비된 headers의 `.config`, generated headers, Module.symvers와 실행 kernel 버전이 맞아야 합니다. 다른 버전의 수업 kernel tree로 대체하지 않습니다. Build compiler package revision 경고와 실제 C warning/error를 구분합니다.

```sh
uname -r
gcc --version
make all
make test
```

현재 장비는 DT/pinmux/PWM/LED/I²C100kHz 설정이 적용돼 있습니다. 소스 clone만으로 다른 장비에 같은 설정이 생기지는 않습니다. Fresh board 설치는 [hardware.md](hardware.md) 및 기존 boot entry와 맞는 base DTB 검토가 먼저입니다.

## 개발 검증

| 명령              | 범위                                                                                                      |
| ----------------- | --------------------------------------------------------------------------------------------------------- |
| `make app`        | C 사용자 프로그램4개                                                                                      |
| `make module W=1` | matching kernel module, 추가 compiler 검사                                                                |
| `make dt`         | overlay compile                                                                                           |
| `make test`       | C11개 + CLI37개 + clean1개 + DT 제안3개, GPIO/I²C 출력 없음                                               |
| `make analyze`    | GCC analyzer, 사용자 소스12개                                                                             |
| `make sanitize`   | encoder/filter/BMP math/sensor lifecycle/AUTO guard/LCD shutdown/process/log/LCD snapshot의 ASan/UBSan9개 |
| `make clean`      | 알려진 binary/object/overlay 삭제, 로그·사용자 메모·boot proposal 보존                                    |

CC/CFLAGS/LDFLAGS/LDLIBS를 지정할 수 있습니다. C11/Wall/Wextra/Wpedantic/Werror는 유지하며 test에서는 NDEBUG를 해제해 assert 검사가 사라지지 않게 합니다. Makefile 변경도 binary 의존성에 포함합니다. `analyze`는 GCC의 -fanalyzer가 있는 compiler를 사용합니다.

```sh
make analyze
make sanitize
make module W=1
python3 scripts/profile-dry-run.py
```

## Dry-run

```sh
./build/fanctl --dry-run --encoder --bmp180 --lcd
```

`speed 2`, `on`, `cw`, `ccw`, `off`, `temp 27`, `mode auto`, `on`, `temp error`, `status`, `quit`를 입력합니다. temp 주입과 cw/ccw 시뮬레이션은 dry-run 전용입니다. 실제 온도나 GPIO 결과로 기록하지 않습니다.

## 실제 실행

현재 실행 중인 controller/encoder monitor를 종료합니다. Module이 없으면:

```sh
sudo insmod driver/smartfan.ko
```

이미 현재 module이 적재됐고 변경이 userspace뿐이면 재적재하지 않아도 됩니다. Driver 변경 뒤에는 controller를 종료하고 rmmod/insmod로 소스와 module을 맞춥니다. 실패한 insmod/rmmod 이후 그대로 시험을 진행하지 않습니다.

```sh
sudo ./build/fanctl --encoder --bmp180 --lcd --lcd-address 0x27
```

파이프 없는 실행에서 명령 입력이 동작하는 것을 확인했습니다. 이 환경의 외부 `| tee` 실행은 입력 무응답을 일으켰으므로 interactive fanctl에 붙이지 않습니다. TTY 설정을 강제로 바꾸거나 sudoers를 수정한 것은 아닙니다.

시작은 MANUAL/OFF입니다. READY, INPUT, ENCODER(debounce_us=0), SENSOR 첫 결과를 확인하고 `mode manual`, `speed 5`, `on`, `status`, `off`를 입력합니다. 상세 순서는 [final-test.md](final-test.md)를 따릅니다. 방향이 반대면 종료 후 --reverse-encoder를 추가합니다. KEY 버튼은 현재 사용하지 않습니다.

## DT 준비와 복구

```sh
make prepare-dt
bash scripts/install-dt.sh --check
```

위 명령은 build/의 offline proposal을 준비·검사하며 /boot를 설치하지 않습니다. Installer는 proposal/current config hash, PWM/LED/I²C와 fallback을 검사하고 다른 DTB 덮어쓰기를 거부합니다. 현재 적용 장비에서 재설치·재부팅할 이유는 없습니다.

실제 설치 명령 `sudo bash scripts/install-dt.sh`는 boot 설정을 바꾸므로 새 장비/DT 변경에서는 백업과 fallback을 검토한 후 사용합니다. `/boot/modified.dtb`와 기존 JetsonIO/Stage1 entry를 보존합니다. 이 스크립트는 boot DTB와 extlinux 구성이 이미 준비된 개발 장비를 기준으로 하며 범용 자동 설치 프로그램은 아닙니다.

## 오류를 구분하기

- stdin이 닫혔거나 write-only이면 자원을 열기 전에 거부합니다.
- CMD가 없으면 모터 출력 이전의 stdin/실행 경로를 확인합니다. 먼저 파이프 없는 실행을 사용합니다.
- CMD on / STATE ON / duty>0인데 무회전이면 전원·ENA·PWM/모터 출력 경로를 구분합니다. 상태 출력만으로 실제 회전은 증명하지 못합니다.
- EBUSY GPIO는 중복 monitor/controller를 종료합니다. GPIO export/PWM export로 provider 자원을 중복 점유하지 않습니다.
- AUTO capability 오류는 현재 driver가 적재됐는지 확인합니다. 구형 driver에 일반 ON으로 우회하지 않습니다.
- BMP helper 오류가 있으면 별도 `./build/bmp180-monitor --once`로 검사합니다. 통합 실행의 센서와 동시에 실행하지 않습니다.
- LCD 주소/100kHz 조건은 실제 확인한 값을 사용합니다. LCD 실패는 warning으로 알리며 motor control은 계속됩니다.

## 명령·로그·통계

선택형 파일 기록은 통합 명령에 `--log build/fan-events.log`를 추가합니다. 기존 파일에 append하고 새 파일은 mode0600으로 생성합니다. sudo로 생성한 파일은 `sudo tail -n 20 build/fan-events.log`로 확인합니다. 종료 시 LOG_CLOSED의 dropped/error를 봅니다. 로그 없이 실행하면 log worker도 생성하지 않습니다.

| 명령                             | 용도                              |
| -------------------------------- | --------------------------------- |
| `on` / `off`                     | 명시적 기동/정지                  |
| `speed 0..5`, `up`, `down`       | MANUAL 풍속 선택; 0은 정지        |
| `mode manual` / `mode auto`      | 모드 전환 시 OFF/disarm           |
| `led 0..8` / `led auto`          | 모터 OFF에서 LED 시험 / 자동 표시 |
| `status`                         | 논리 상태·풍속·센서 확인          |
| `stats`                          | Session counter와 log snapshot    |
| `monitor` / `monitor 500..10000` | 기본1000ms / 지정 주기 상태 출력  |
| `monitor off`                    | 주기 출력만 중단                  |
| `help` / `quit`                  | 도움말 / 정지·정리 후 종료        |

Stats의 commands는 빈 줄을 제외한 모든 명령을 세며 잘못된 명령과 stats/quit도 포함합니다. sensor_frames는 검증된 IPC frame(error 포함), injected_samples는 dry-run temp입니다. Fault는 error/stale 장애 구간의 진입, recovery는 valid 복귀이며 모터 재기동 횟수가 아닙니다. Encoder counter는 방향 반전이 적용된 step으로 AUTO에서 무시한 실물 입력도 포함하고 CLI up/down은 제외합니다. Counter는 main 소유이며 재실행하면 초기화됩니다. 종료 시 SESSION_STATS를 log에 넣지만 큐 포화/SIGKILL/쓰기 오류에는 보존을 보장하지 않습니다.

`make thread-check`는 TSan의 log/LCD snapshot 스트레스 검사이며 모든 하드웨어 경로의 race 부재를 증명하지는 않습니다. `make test`에는 임시 extlinux 구성으로 fallback base 선택·누락·overlay 불일치 거부를 검사하는 Python3 테스트도 포함합니다.
