# 2단계 — Encoder / PWM 실행 안내

이 문서의 Encoder 7·31번 배선 및 `smartfan-speed` 설치 절차는 이전 구성이다.
현재 준비된 12·38번 핀 변경 시험에는 [encoder-alt-pin-trial.md](encoder-alt-pin-trial.md)를 사용한다.
최신 [조사 결과](encoder-audit.md)에서 L4T36.5.2의 debounce 기간0 요청이
hardware enable 비트를 해제하지 않음을 확인했다. 아래의 `--no-debounce`는
기간0 요청이며 실제 하드웨어 필터 우회가 검증됐다는 뜻이 아니다.

1단계는 사용자 완료 보고와 commit `e2da01d`를 기준으로 완료 처리했다.
2단계 code/Build/software test는 완료했다. 아래 설치와 실물 시험은 아직 수행하지 않았다.

## 내가 할 작업 — 지금 실행할 순서

### 1. 모터 전원을 분리하고 Encoder 배선 확인

배선을 바꿀 때는 Jetson/L298N 전원을 끈다. 모터 전원은 아래 입력/PWM 신호 시험이 끝날 때까지 분리한다.
Jetson의 물리 핀 번호 기준이다. 모터 신호 세 선의 위치는 1단계와 동일하다.

| Jetson 물리 핀 / 빵판 | 연결할 부품 단자 | 역할 |
|---|---|---|
| 1번에서 공급한 **3.3V 빵판 +** | Encoder의 **5V라고 인쇄된 전원 핀** | 실제 공급은 **3.3V** |
| 14번 또는 공통 GND 빵판 − | Encoder **GND** | 전원 기준 |
| **7번** | Encoder **S1** | 입력 A, PAC.06, main GPIO line144 |
| **31번** | Encoder **S2** | 입력 B, PQ.06, main GPIO line106 |
| 연결하지 않음 | Encoder **KEY** | 누름 버튼 미사용 |
| **32번** | L298N **ENA** | GPIO enable; **ENA 캡 제거 유지** |
| **15번** | L298N **IN1** | GPIO 대신 **PWM1** 출력 |
| **29번** | L298N **IN2** | LOW 고정 GPIO |
| 6번 / 공통 GND | L298N **GND** | 신호 기준 |

Encoder의 `5V` 표시가 있어도 **5V 빵판에 연결하지 않는다**.
기존 모터 전원/logic 전원 연결을 이번 단계에서 새로 변경하지 않는다.

### 2. DT 설치 및 재부팅

```sh
cd /home/aidl/work/jetson-smart-fan-controller
make all
make test
make prepare-dt
bash scripts/install-dt.sh --check
sudo bash scripts/install-dt.sh
sudo reboot
```

`DT_READY` → preflight `DT_READY` → `DT_INSTALLED` 출력 순서다. 오류가 있으면 다음 명령을 진행하지 않는다.
설치 script는 extlinux 원본을 timestamp backup하고, 새 stage2 DTB와 `smartfan-speed` 항목을 추가한다.
기존 `smartfan-output`/JetsonIO 항목과 UART11·36 overlay, 내장 cooling fan PWM은 보존한다.
`make`는 설치나 모터 구동을 하지 않는다. sudo와 reboot는 사용자가 실행한다.

### 3. 재접속 후 새 module과 Encoder 시험

모터 전원 분리 상태를 유지한다.

```sh
cd /home/aidl/work/jetson-smart-fan-controller
sudo fdtget -t s /sys/firmware/fdt /smartfan pwm-names
sudo insmod driver/smartfan.ko
ls -l /dev/smartfan
sudo ./build/encoder-monitor
```

`pwm-names` 정상 출력은 `motor`다. module은 GPIO/PWM을 자동 요청한다.
GPIO export, 수동 GPIO 등록, PWM sysfs export, Jetson-IO 추가 설정은 필요 없다.
`insmod: File exists`라면 기존 CLI를 종료하고 `sudo rmmod smartfan` 후 `sudo insmod driver/smartfan.ko`를 실행한다.

Encoder를 천천히 양방향으로 돌린다. `STEP delta=+1` 또는 `STEP delta=-1`이 출력된다.
두 방향이 반대라면 Ctrl+C 후 다음 명령으로 시험한다.

```sh
sudo ./build/encoder-monitor --reverse
```

입력 확인 후 **Ctrl+C로 monitor를 종료한다**. monitor와 fanctl이 Encoder line을 동시에 요청할 수 없다.

### 4. 모터 전원 분리 상태에서 통합 동작 / PWM 신호 확인

```sh
sudo ./build/fanctl --encoder
```

방향 반전이 필요했다면 `sudo ./build/fanctl --encoder --reverse-encoder`를 사용한다.
CLI에 순서대로 입력한다.

```text
speed 1
status
on
status
```

- 시작은 `STATE OFF`, `SPEED level=5/5 duty=0% pwm=1 period_ns=4000000`이다.
- `speed 1`은 OFF 상태에서1단계만 선택한다.
- `on`은 PWM 출력을 켠다. 200ms 동안100% 기동 boost, 이후60%다. **현재 모터 전원은 분리돼 있어 회전하지 않는다.**
- Encoder를 돌리면 선택 단계가0~5 범위에서 변한다.
- 단계0은 `STATE OFF`로 정지한다. OFF에서 양의 단계로 돌려도 자동으로 켜지지 않는다. `on`으로 재시작한다.
- `off`, `quit`, Ctrl+C는 OFF 처리한다. 조작 중에도30초 최대ON 제한이 유지된다.

| 단계 | 목표 duty |
|---|---|
| 0 | 정지 |
| 1 | 60% |
| 2 | 70% |
| 3 | 80% |
| 4 | 90% |
| 5 | 100% |

출력의 duty는 요청값이며 측정된 RPM/전압이 아니다. 실제 PWM period/duty 검증은 오실로스코프 또는 logic analyzer로15번↔GND를 관찰한다. 장비가 없다면 평균 전압은 보조 관찰만 가능하며 파형 검증 완료로 기록하지 않는다.

## 정상 결과

- Encoder 양방향 입력이 일관되고, 연속 회전에서 단계가0~5로 제한된다.
- `pwm=1`, `period_ns=4000000`으로 새 PWM mode가 확인된다.
- OFF에서 풍속 선택만 변하고 모터를 자동 재시작하지 않는다.
- 새 DT 적용 후 PN1은 function=gp/gpio-mode1/output, PG6/PQ5는 GPIO/output, PAC6/PQ6는 GPIO/input이다.

```sh
sudo cat /sys/kernel/debug/pinctrl/2430000.pinmux/pinconf-groups | grep -Ei -A 14 'soc_gpio19_pg6|soc_gpio39_pn1|soc_gpio32_pq5|soc_gpio59_pac6|soc_gpio33_pq6'
```

CLI 실제 ioctl/PWM 요청 성공은 물리 PWM 파형이나 모터 풍속 성공을 뜻하지 않는다.

## 문제가 있을 경우

| 증상 | 확인할 것 / 전달할 결과 |
|---|---|
| `pwm=0` / speed에서 Operation not supported | Stage1 DT 또는 이전 module 상태. `pwm-names`, 재부팅 여부, `sudo dmesg \| tail -n 60` 확인 |
| Encoder Device or resource busy | encoder-monitor/다른 GPIO 프로그램 종료 |
| Encoder Invalid argument / Operation not supported | GPIO v2/debounce 또는 pinmux 설정 오류. 오류 전문과 dmesg 전달. 필터를 몰래 제거하지 않음 |
| 돌려도 STEP 출력 없음 | 아래 GPIO raw-level 및 pinconf 진단 실행. events=0이면 Gray decoder 전 단계에서 edge가 안 들어온 상태 |
| sequence gap / invalid transition 증가 | bounce/배선/빠른 회전 및 신호 품질 확인. partial cycle은 버리고 재동기화함 |
| module PWM 오류 / `/dev/smartfan` 없음 | `sudo dmesg \| tail -n 60` 전달. GPIO/PWM을 다른 프로그램으로 export하지 않음 |
| 새 DT로 부팅 문제 | 부팅 menu에서 보존된 `smartfan-output` 또는 JetsonIO 선택; 복구 후 backup extlinux 사용 |

### 엔코더 입력이 0 event일 때 분리 시험

한 번에 한 프로그램만 GPIO 144/106을 점유할 수 있다. 실행 중인 `encoder-monitor`/`fanctl --encoder`를 먼저 Ctrl+C로 종료한다. 모터 구동은 중지하고, encoder만 시험한다.

**2026-10-08 실물 관찰:** 이전 `--no-debounce --raw`에서 encoder 선을 뺐다 꽂을 때 EDGE가 22개까지 기록되고 우연한 `STEP` 1개도 출력됐다. 연결된 상태로 축을 돌릴 때는 EDGE가 없다. 따라서 GPIO event fd, poll/read 및 두 line의 IRQ 경로는 최소한 탈착에 따른 전압 변화에는 반응한다. 이 STEP은 실제 회전 검증으로 세지 않는다. 이후 참고 Tegra GPIO 5.15.185 소스에서 hardware debounce enable bit가 GPIO 해제 시 유지될 수 있음을 발견했다. 이전 `--no-debounce`는 debounce 속성을 생략했으므로 실제 필터 해제를 입증하지 못한다. 현재 진단 버전은 기간 0을 명시적으로 요청한다. 실행 중인 커널은 5.15.199-tegra이므로 같은 구현인지 검증 결과를 구분한다.

1. **GPIO debounce 기간 0을 명시적으로 요청**해 본다. 손잡이를 천천히 여러 칸 돌린다. `EDGE`는 실제 GPIO event 읽기 성공이며 `STEP`은 완전한 4-edge cycle이다. 2026-10-08 실행 중인 커널에서 요청 성공을 확인했다.

   ```sh
   cd /home/aidl/work/jetson-smart-fan-controller
   make app
   sudo ./build/encoder-monitor --no-debounce --raw
   ```

2. 여전히 `EDGE`가 없다면 Ctrl+C로 종료하고, **같은 두 GPIO를 1 ms 간격으로 직접 읽어** 본다. 이 모드는 IRQ/edge 이벤트 경로를 사용하지 않는다. 회전 시 `LEVEL change`가 나타나는지 확인한다. 실행 중인 커널에서 입력 요청과 초기 값 읽기 성공을 확인했다.

   `--poll-levels` 실행 중에는 두 encoder IRQ가 등록되지 않으므로 `/proc/interrupts` 카운터가 증가하지 않는다. IRQ 카운터 비교는 1번 `--no-debounce --raw`를 실행하는 동안에만 한다.

   ```sh
   ./build/encoder-monitor --poll-levels
   ```

3. 프로젝트 코드와 별개로 비교가 필요하면 Ctrl+C로 종료하고 libgpiod v1 감시기를 실행한다. 회전할 때 `rising edge`/`falling edge`가 출력되어야 한다.

   ```sh
   sudo gpiomon --bias=as-is --line-buffered gpiochip0 144 106
   ```

4. 아래는 추가 물리 분리 시험이 가능한 경우에만 한다. Jetson을 끄고 encoder **S1/S2 두 선을 물리 7/31번에서 분리**한다. 다시 켜서 물리 31번을 1 kΩ 저항을 통해 물리 1번(3.3 V)에만 연결한 상태와 물리 30번(GND)에만 연결한 상태를 번갈아 만든다. **두 전원 선을 동시에 연결하지 않는다. 5 V는 사용하지 않는다.** 각 상태에서 아래 명령을 실행하면 각각 `1`, `0`이 나와야 한다. 같은 방법으로 물리 7번/offset 144도 시험한다.

   ```sh
   sudo gpioget --bias=as-is gpiochip0 106
   sudo gpioget --bias=as-is gpiochip0 144
   ```

   레벨 `1/0`이 정상이라면 같은 직접 입력을 `gpiomon`을 켜 둔 채 번갈아 가며 rising/falling edge도 확인한다. `gpioget`과 `gpiomon`은 같은 line을 동시에 점유할 수 없으므로 차례로 실행한다.

| 관찰 결과 | 좁혀지는 위치 |
|---|---|
| 기본 monitor는 0, 새 `--no-debounce --raw`는 EDGE 출력 | 2 ms GPIO debounce/filter 설정 또는 기존 설정 잔류 |
| `--poll-levels`에서 회전 시 LEVEL 변화, edge monitor는 0 | GPIO IRQ/edge 또는 debounce 경로 |
| `--poll-levels`에서도 회전 시 변화 0 | 커널이 읽는 입력값이 회전으로 변하지 않음; 디코더 이전 단계 |
| `gpiomon`은 출력, debounce 없는 monitor는 0 | GPIO v2 요청/읽기 경로; 출력 전문 필요 |
| 직접 3.3 V/GND 입력에서는 정상, encoder 연결 시 0 | encoder 모듈/배선/입력 부하 및 Jetson carrier level translator 적합성 |
| 직접 입력 `gpioget`도 고정 | 물리 핀 위치, header/level translator/핀mux 경로; encoder 교체로 해결되지 않음 |
| 직접 입력 레벨은 바뀌는데 `gpiomon`만 0 | kernel IRQ/edge 설정 경로 |

DevKit carrier GPIO에는 TXB0108 level translator가 들어간다. 사진 속 encoder 모듈은 10 kΩ 저항과 capacitor를 가진 접점 회로이므로, 모듈에서 멀티미터로 정상 전압을 보더라도 carrier 입력에서 정상 edge를 보장하지 않는다. TI는 TXB0108을 push-pull CMOS 출력용으로 규정하고 외부 pull 저항은 50 kΩ 초과를 권장한다. **이 회로 충돌은 현재 의심 원인이지, 위 직접 입력 시험 전에는 확정 원인이 아니다.** [NVIDIA carrier 사양](https://developer.nvidia.com/downloads/assets/embedded/secure/jetson/orin_nano/docs/jetson_orin_nano_devkit_carrier_board_specification_sp.pdf), [TI TXB0108 datasheet](https://www.ti.com/lit/ds/symlink/txb0108.pdf).

`encoder-monitor`를 종료한 뒤, 아무것도 돌리지 않았을 때 현재 S1/S2 값을 한 번 읽는다.

```sh
sudo gpioget --bias=as-is gpiochip0 144 106
```

다음 명령이 반복되는 동안 Encoder를 천천히 여러 바퀴 돌린다. 출력 두 숫자는 매 회 S1/S2 raw level이다. 회전 중 적어도 하나가 `0↔1`로 바뀌어야 한다.

```sh
sudo bash -c 'for i in $(seq 1 50); do gpioget --bias=as-is gpiochip0 144 106; sleep 0.1; done'
```

그 뒤 monitor를 다시 켜고 돌린다.

```sh
sudo ./build/encoder-monitor
```

raw 값이 바뀌지 않으면 모듈 단자에서 신호를 멀티미터로 확인한다. 전압 측정 모드로 두고 검정 probe는 Encoder의 GND에 둔다.

1. 빨강 probe를 보드의 `5V`라고 인쇄된 단자에 댄다. 실제 공급은3.3V이므로 GND 기준 약3.3V가 나와야 한다.
2. 빨강 probe를 `S1` 단자에 대고 손잡이를 아주 천천히 돌린다. `S2`도 똑같이 측정한다. 각 신호가 LOW 약0V와 HIGH 약3.3V 사이에서 바뀌어야 한다.
3. 앞면 사진의 단자 순서는 `GND, S1, S2, KEY, 5V`다. 보드 뒷면에서는 좌우가 거울처럼 반대로 보이므로, 뒷면 기준으로 선 순서를 판단하지 않는다. KEY는 사용하지 않는다.

모듈 전압이 정상이고 S1/S2가 모듈에서 바뀌는데 Jetson raw 값은 고정이면, 전원을 끄고 모듈 S1→J12 물리7, S2→J12 물리31, GND→공통GND 선의 연속성을 확인한다. 모듈 단자에서 전압이 바뀌지 않으면 모듈 전원/접점/부품 이상을 먼저 확인한다.

raw 값은 바뀌는데 `events=0`이면 다음 pinconf 결과와 함께 알려준다.

```sh
sudo rg -n -A 14 'soc_gpio59_pac6|soc_gpio33_pq6' /sys/kernel/debug/pinctrl/2430000.pinmux/pinconf-groups
gpioinfo /dev/gpiochip0 | rg 'line (106|144):'
```

## 실제 모터 PWM 시험 전 남은 확인

사진의 보드에는 `M7` 다이오드가 보이지만 제조사와 reverse recovery 규격은 확인되지 않았다.
[ST L298 datasheet](https://www.st.com/resource/en/datasheet/l298.pdf)는 chopping inductive load에 fast recovery diode(trr≤200ns)를 요구한다.
**250Hz라는 낮은 주파수만으로 M7의 적합성이 보장되지 않는다.**
1단계 DC ON/OFF 성공을 PWM 적합성 확인으로 간주하지 않는다.
따라서 지금 안내하는 실물 시험은 **모터 전원 분리 상태의 Encoder/제어 신호 시험**이다.

보드 diode 부품 규격 및 PWM flyback 경로가 확인된 뒤에 실제 모터 PWM 시험을 진행한다.
그때 검증할 항목은1~5단계 기동/풍속 차이, 저속 stall 여부, 공급·기동 전류, bridge/diode 발열, 전원 안정성, 정지·재시작이다.
실물 결과에 따라 최소 duty와 boost를 조정한다. 지금60~100% 값은 초기 목표값이며 실제 풍속 검증 결과가 아니다.

## 수행한 software 검증

- 현재 target Kernel5.15.199-tegra에서 app/monitor/module/DT compile.
- C decoder/event test: CW/CCW, fullcycle, bounce, reversal, invalid transition, 경계 clamp, 같은 batch의 반대 방향 순서/0단계 경유, bounded read, stale event 및 sequence gap 처리.
- CLI dry-run13 tests: 기존9개 회귀 + 풍속 선택/0정지/재시작, boost, 속도 변경 중 최대ON 제한, 회전/방향반전/경계, 잘못된 속도.
- Offline DT merge: legacy IN1 GPIO 제거, PWM provider state, GPIO/input state, UART/내장 fan/boot fallback 보존 확인.
- Installer preflight 통과. 실제 설치/reboot/module load/Encoder/PWM 통전은 에이전트가 수행하지 않았다.
- 추천 commit: `feat: add rotary encoder and PWM speed control`.
