# 진행 상태 및 검증 기록

업데이트: 2026-10-08 (Asia/Seoul).

## 현재 단계

**최신 사용자 실물 결과:** LCD를3.3V로 공급하고 contrast 가변저항을 조정했을
때 화면 문자가 표시되지만 잘 보이지 않는다고 보고했다. 기본 통신/초기화/표시가
동작한 것으로 보고했고, live DT의 `smartfan-lcd` 및100kHz 적용도 확인했다.
대비·시인성 검증은 미완료다. 모듈의5V 설계에 따른
LCD 구동/대비 전압 부족 가능성이 남아 있다. 레벨 시프터 없는5V I²C 직결로
되돌리지 않는다. 기존3.3V 유지 또는5V+검증된I²C 전압 변환 구성을 선택한다.

**최신 요청: 레벨 시프터 없이 LCD 시험.** LCD+PCF8574 전체3.3V 공급 시험으로
절차를 추가했다. PCF logic 동작 범위와 LCD 모듈의 실제 contrast 동작을 구분한다.
현재 live DT는 여전히 LED Bar 구성/400kHz여서 LCD100kHz 설치가 적용되지 않았음을
확인했다. `--dry-run`이 실물 송신을 하지 않는 점을 안내하고100kHz 관련 오류
메시지를 구체화했다. 이번 작업에서도 실제 I²C 송수신은 하지 않았다.

**최신: 사용자 LED Bar 시험 성공 보고. LCD1602/PCF8574T 구현으로 진행.**
현재 header I²C가 400kHz임을 live DT로 확인해 100kHz 별도 boot proposal을 준비했다.
사용자는 LCD 5V+PCF8574T/HW-061, 별도 level shifter 없음을 보고했다. 이는
변환기가 없는 5V 직결 구성으로 해석하며 LCD SDA/SCL 분리와 I²C 레벨 변환이 필요하다.
이번 작업에서는 LCD bus scan/read/write를 실행하지 않았다.
LCD 전용 worker, 16자 두 행 갱신, ON/OFF·풍속·LED 칸수 표시와 dry-run preview를
추가했다. 실제 주소와 backpack mapping은 미검증이다. Build, C protocol/worker
검사, CLI 18 tests, DT 합성/설치 전 검사 통과. [LCD 실행 안내](lcd-run.md).
아래 LED 구현/Encoder 기록은 각 시점의 이력이다.

**최신 사용자 지시: Encoder 진단을 보류하고 LED Bar를 먼저 구현.**
Driver에 optional GPIO 배열과 추가 ioctl6/7, CLI에 `led 0..8`/`led auto`를
추가했다. 자동 표시에서 OFF=0칸, 풍속1..5=2/4/5/7/8칸이고 startup boost와
무관하게 선택 단계를 표시한다. 독립 점등 시험은 motor OFF에서만 허용한다.
close/timeout/remove/suspend에서 LED도 끄며 기존 motor 정책과 ABI는 보존한다.
`make all`, module W=1 build, encoder C tests, CLI16 tests, DT 합성 및 installer
preflight 통과. Compiler 경고는 GCC11.4.0의 Ubuntu package revision 차이이며
숨기지 않았다. 실제 LED GPIO 출력/점등·부하 전류는 미확인이다.
별도 `/boot/dtb/smartfan-ledbar.dtb`와 `smartfan-ledbar` 항목을 준비했고 기존
encoder/Stage2/Stage1 항목을 보존한다. LED와 겹치는 SPI1만 비활성화하며
SPI0/UART/내장 fan state 보존 검사도 통과했다. 실제 `/boot` 설치는 아직 전이다.
[지금 LED Bar 실행할 순서](ledbar-run.md). 아래는 Encoder 진단 시점 기록이다.

**Encoder 핀 변경 적용 확인:** 기존 물리7/PAC.06(offset144), 31/PQ.06(offset106)에서
물리12/PH.07(offset50), 38/PI.01(offset52)로 옮긴 별도 DTB와
`smartfan-encoder-alt` 부팅 항목을 오프라인 생성했다. 기존 `smartfan-speed` DTB와
부팅 항목은 복구용으로 보존한다. 현재 live DT도 PH7/PI1 및 입력 속성1/1/0/0을
가리키고 smartfan driver가 bind되어 있다. 사용자는 새 핀에서도 무응답을 보고했다.
독립 진단에서 GPIO50/52 IRQ 등록을 확인했다. 1초 agent 시험은 A/B=1/1,
samples934, event0이었지만 사용자 회전과 동기화한 시험이 아니므로 원인 확정
자료로 사용하지 않는다. `encoder-diagnose`의20초 sudo 회전 시험으로 실제
pinconf·debounce·raw sample·IRQ·event를 동시에 수집한다.
[최신 조사 결과와 실행 명령](encoder-audit.md).

**사용자 보고: 1단계 완료 및 commit `e2da01d` 완료. 현재 2단계 설치 완료, Encoder 실물 무응답 조사 중.**

| 항목 | 현재 상태 |
|---|---|
| 1단계 ON/OFF | 사용자 완료 보고; GPIO 모터 회전 확인 |
| Stage2 Driver | PWM1·5단계·startup boost·기존 정지 정책 구현 |
| Encoder | GPIO v2 두 edge·2ms per-line debounce·Gray decoding·방향 반전 구현 |
| CLI | speed/up/down, optional encoder, hardware 없는 회전 simulation 구현 |
| Software test | encoder C unit 및 CLI integration 13개 통과 |
| Build / offline DT | native app/module/overlay 및 Stage2 boot proposal 검증 |
| Stage2 실물 | 새 입력 DT/PWM 적용 확인; 사용자는 핀 변경 후에도 회전 무응답 보고. 동시 raw/IRQ/event 기록 대기 |
| PWM 부하 적합성 | 사진의 M7 flyback diode recovery 규격 및 모터 전류/발열 미확인 |
| 시스템 변경 / Git | 사용자가 Stage2 설치·load·reboot 수행. 에이전트는 Git staging/commit/push 미수행 |

사용자 Stage2 입력 점검(2026-10-08): live DT `pwm-names=motor`, module/device 존재.
Encoder monitor는 gpiochip0 offsets144/106을 입력으로 요청했으나 raw level은 1/0 고정,
50회 polling 동안 변화 없음, monitor 종료 시 `events=0`/invalid=0.
Pinmux PAC6/PQ6 모두 tristate=1, enable-input=1, gpio-mode=0; gpioinfo는 PAC.06/PQ.06 input/unused.
이는 GPIO edge decoding보다 앞 단계에서 신호가 도달하지 않는 상태를 가리킨다.
추가 진단: 탈착 시 두 GPIO의 edge를 수신했으나 축 회전에는 반응하지 않았다. 참고 kernel 5.15.185 Tegra GPIO 구현은 debounce 비트가 line 해제 후 유지될 수 있어, debounce 속성 생략을 해제로 해석하지 않는다. 진단 monitor는 기간 0을 명시적으로 요청하고 IRQ 없는 1 ms level polling 옵션을 추가했다. 5.15.199-tegra에서 두 요청과 초기 level 읽기는 성공했으며, 실물 회전 중 결과는 사용자 실행 대기다.
사용자는 멀티미터 점검이 정상이라고 보고했다. 측정 위치·Jetson 연결 여부별 수치는
제공되지 않아 부하가 연결된 header 입력 전압/파형은 별도 미확인이다. 원인은 확정되지 않았다.
prepare-dt 재실행 overlay 누적/installer 기존파일 거부 문제는 fallback 기반 idempotent 생성으로 수정했다.
기존 `smartfan-speed` 구성은 복구용으로 보존돼 있다. 새 `smartfan-encoder-alt`
설치·live DT 적용·prepared/installed DTB 일치를 확인했다. 추가 재부팅은 필요 없다.

[지금 실행할 순서](stage2-run.md). 아래 기록은 각 시점의 관찰을 보존한 이력이다.

## 이전 조사 및 1단계 이력

추가 사진 검토: `20261008_102440.jpg`, `20261008_102501.jpg` 확인.
LCD1602A/I²C backpack 및 Encoder 신호 label, Motor R300 2선 구조를 확인하고
[상세 연결표](wiring.md)를 추가했다. 실제 Carrier/전원/단자 점퍼/buffer 확인은 남아 있다.
사용자 답변: 외부 전원 5V 입력 가능, 멀티미터 사용 가능, R300 3.3V 구동 예정.
계획 전압과 정격을 구분하며, 공급장치 출력 전류는 추가 확인 필요하다.

`docs/img/`의 상세 사진 11장을 추가 확인하여 [간단 연결표](pin-connections.md)를 작성했다.
확인: BMP180/GY-68 및 VIN/GND/SCL/SDA, L298 출력/전원/Header label,
Motor IN+/IN−, Encoder 5V 표시와 103 저항, WCNLB8-SR12 및 9A331G 9pins.
통전/통신 검증은 하지 않았다. BMP180의 regulator 뒤 I/O 전압, 외부 전원 전류,
buffer/level shifter, 실제 J12 pin1/Breadboard 위치 확인이 남아 있다.

사용자는 3.3V용/5V용 빵판을 별도로 사용한다고 추가 전달했다.
간단 연결표에 두 voltage 영역 배치와 공통 GND, + rail 분리, 별도 regulator 출력 병렬 연결 금지를 반영했다.
사용자가 공급원을 Jetson의 3.3V·5V 핀으로 확인했다.
간단 연결표에서 pin1→3.3V + rail, pin4→5V + rail, pin6→공통 GND로 정리했다.
이 5V logic rail을 모터 power rail로 사용하지 않는다.

추가 확인: 외부 전원5V/최대A모름, 추가 buffer/level shifter 확보 불가.
`20261008_105050.jpg`에서 J12 pin1/2/39/40 및 Breadboard rail을 확인했다.
강의 `LEDBAR_로터리엔코더.pdf` 9pages를 확인했고 Basys pin/Verilog는 제외했다.
20µA를 절대 한계처럼 해석하여 추가 LED Driver를 필수로 요구했던 판단은 정정했다.
LED+330Ω array 측정/한 칸 시험, 3.3V 공급 Encoder 시험 및 L298 직결 입력 검증안을 반영했다.
실제 GPIO 출력/통전/모터 동작은 여전히 미수행이다.

사용자 요청에 따라 L298 전원/jumper 무전원 검사와 motor 대신1kΩ 부하를 사용하는
[소프트웨어 없는 검사 절차](l298-hardware-test.md)를 작성했다.
계획/예상 결과이며 사용자가 아직 수행·측정한 결과는 없다.

사용자 보고: 배선 작업만 완료했다. 현재 완성 배선/OUT 부하/전원 인가 상태는 아직 확인하지 않았다.
배선 완료 보고는 통전 안전성이나 hardware 동작 성공으로 기록하지 않는다.
다음은 완성 배선·전원 jumper 확인 → 무전원 검사 → motor 미연결 저항 부하 검사 순서다.

## 1단계 software 작업 결과

- 완료 파일: `driver/smartfan.c`, `driver/Kbuild`, `include/smartfan_uapi.h`,
  `app/fanctl.c`, `dts/smartfan.dts`, `Makefile`, `.gitignore`, `tests/test_fanctl.py`, `docs/build-run.md`.
- 실행한 검사: `make all`, module `W=1` build, `make test`, `modinfo`/ELF 확인.
- Driver vermagic: `5.15.199-tegra SMP preempt mod_unload modversions aarch64`.
- DT compile 및 기존 boot DTB에 대한 offline 합성 확인. `/boot` 원본은 수정하지 않음.
- CLI9 tests: ON/OFF, EOF, 불완전 입력, NUL/길이 초과, 부분 입력 중 heartbeat,
  반복 ON 최대시간 유지, SIGSTOP 후 만료 처리, SIGINT/TERM/HUP 종료, 잘못된 옵션 거부.
- SIGSTOP dry-run 검사는 **resume 시 만료 검사**를 확인한 것. 정지된 process 밖에서 실제로 motor를 끄는 kernel workqueue는 시험하지 않음.
- 테스트 중 중복 OFF 출력 순서를 고정 가정했던 fixture를 수정하고9개 재실행 통과.
- Static review로 입력 batch의 종료 신호 처리와 work 예약 시각을 보완함.
- Kbuild compiler 경고: headers/local GCC 모두11.4.0이지만 Ubuntu package revision 문자열 차이.
  경고를 숨기거나 headers를 변경하지 않음. 실제 Kernel 제작11.3.0과 module load 호환성도 구분해 기록.
- 실제 module load, device open/ioctl, GPIO 전압, 모터 기동/정지는 미수행.

현재 다음 작업은 사용자가 software 결과를 확인한 뒤,
실제 적용 전에 모터 전원 분리/배선/전원 jumper/초기 OFF 및 Pinmux를 확인하는 것이다.
저항 부하 시험은 선택 가능한 진단 방법이며 software 개발의 필수 선행 조건은 아니다.
추천 Commit Message: `feat: add stage 1 smart fan driver and CLI`

실행 안내 재확인: `build/fanctl`, module 및 DT 산출물이 없어 `make all`로 다시 생성했다.
현재 Kernel은5.15.199-tegra, live DT custom node 없음, smartfan module 미적재, `/dev/smartfan` 없음.
현재 extlinux JetsonIO entry의 FDT는 `/boot/dtb/kernel_tegra234-p3768-0000+p3767-0005-nv-super.dtb`다.
이전 조사에 기록한 `/boot/modified.dtb` entry와 다르므로 실제 적용 계획은 현재 설정으로 다시 작성해야 한다.
부팅 파일을 이번 작업에서 수정하지 않았다. 다음 준비는 root-only Pinmux 확인과 DT 적용 계획이다.

사용자 Pinmux 출력: PG6/PN1/PQ5는 MUX/GPIO UNCLAIMED. 점유가 없다는 관찰이며 actual pad mode 검증은 아니다.
사용자가 DT 적용·재부팅을 승인했다. 모터 전원 분리 상태 유지 조건이며 module load는 아직 하지 않는다.
sudo password가 필요해 에이전트의 privileged 실행은 불가했다.
검증/backup/atomic install을 수행하는 `scripts/install-dt.sh`를 작성해 사용자 Terminal 실행 단계로 넘겼다.
현재 시점에 실제 `/boot` 설치 및 reboot 완료 결과는 아직 없다.

사용자 재부팅 후 `edu,jetson-smartfan` 출력 전달. 에이전트도 live DT node와 Kernel5.15.199-tegra를 재확인했다.
Module vermagic이 일치하며 현재 smartfan module은 미적재, `/dev/smartfan`은 아직 없다.
다음은 모터 전원 분리/ENA jumper 제거 및 실제 EN signal 배선 확인 후 module 적재와 CLI ioctl 확인이다.
GPIO는 Driver probe에서 자동 요청하므로 sysfs export/mknod는 사용하지 않는다.

사용자 runtime 로그 확인: insmod 성공, `/dev/smartfan` 생성(10:122), CLI initial OFF→ON→OFF→quit 처리.
에이전트도 platform bind 및 GPIO PG6/PN1/PQ5의 enable/in1/in2 output consumer 점유를 확인했다.
이는 GPIO pad 전압이나 실제 회전 성공을 뜻하지 않는다.
사용자는 모터와 VS 전원을 모두 연결했으나 회전하지 않는다고 보고했다.
현재 원인 미확정: 모터를 분리한 상태에서 VS/VSS/ENA/IN1/IN2 전압을 측정해 전원/신호/pinmux 문제를 구분한다.
Motor 정상 동작 및1단계 hardware 완료로 기록하지 않는다.

추가 사용자 hardware 결과: ENA cap을 씌우고 IN1/IN2를 전원/GND에 연결하면 motor가 회전한다.
CLI 시험에서는 cap을 제거하고 ENA32/IN1 15/IN2 29로 복귀했다고 확인했다.
수동 motor/bridge 동작은 확인됐지만 GPIO 경유 회전은 미검증/미회전이다.
전체 code/DT/GPIO 요청 검토에서 ON 논리와 mapping은 일치한다.
현재 smartfan DTS에 pad output pinctrl state가 없는 점을 확인했고, 실제 pinconf-groups를 읽어 가설을 검증할 예정이다.
PWM은1단계 ON/OFF에 필요 없다. [분석 기록](debugging.md).

사용자 pinconf-groups 결과 세pad 모두tristate1/enable-input1/gpio-mode0 확인.
GPIO output 요청과 별도로 pad output이 비활성화된 설정 오류를 확인했다.
DTS에 config-only default state를 추가했다. 변화는 PG6/PN1/PQ5의 tristate/enable-input/gpio-mode=0으로 제한했다.
`make prepare-dt` 및 installer `--check`, offline mapping/timeout/provider pinctrl-0 보존 비교 통과.
별도 smartfan-output boot entry 및 새 smartfan-stage1-output.dtb를 준비했고 기존 boot 항목은 보존했다.
이번 수정의 실제 설치/reboot/회전 결과는 아직 없다. Runtime DT/config를 이번 작업에서 수정하지 않았다.
추천 Commit Message: `fix: configure smart fan GPIO output pads`

수정안 적용/reboot/module 적재 후 사용자 pinconf-groups 결과를 확인했다.
PG6/PN1/PQ5 모두 tristate0/enable-input0/gpio-mode0으로 변경됐다.
Pad 설정 수정은 runtime에서 확인됐으며, 다음 검증은 짧은 motor ON/OFF다.
아직 GPIO 제어에 의한 실제 회전 성공은 보고되지 않았다.

사용자가 pad 수정 후 CLI ON으로 motor가 정상 회전한다고 보고했다.
GPIO 기반 기본 motor 구동 성공을 기록한다. 반복 ON/OFF, ON 중 quit/강제종료,
heartbeat 중단 시 실제 정지 및 module unload/reload 초기 OFF는 별도 관찰이 남아 있다.
Driver/CLI code 검토에서1단계 요구기능의 추가 구현 누락은 확인되지 않았다.
1단계 safety/runtime 검증을 끝낸 뒤2단계 Encoder/PWM로 확장한다.
남은 시험 명령/정상 결과는 [1단계 종료 확인](stage1-checklist.md)에 정리했다.

사용자 지시 갱신: 승인 질문을 반복하지 말고 가능한 작업은 수행하며 사용자 작업을 직접 안내한다.
Pad 수정안의 installer preflight를 다시 검증했다. sudo password 요구로 실제 설치/reboot는 에이전트가 수행할 수 없었다.
사용자에게 motor 전원 분리 → installer → reboot → module 적재 → pad 상태 확인 순서를 안내한다.

## 현재 문제 / 다음 작업

1. 모터 전원을 분리하고 Stage2 DT 설치·재부팅.
2. 새 module load 후 `encoder-monitor`로 방향/연속 회전 입력 확인.
3. 모터 분리 상태에서 PWM 출력과 OFF/timeout 처리 확인.
4. 실제 PWM 부하 적합성을 확인한 뒤 모터 풍속·최소 기동 duty·전류/발열 검증.
5. 사용자 Stage2 정상 동작 확인 후 LCD 단계로 진행. 현재는 3단계를 구현하지 않는다.

## 이틀 일정

- 1일 차: 안전 검증 → ON/OFF 최소 완성 및 오류/종료 시험. 통과하면 Encoder/PWM, 여유 시 LCD.
- 2일 차: LCD/BMP180 중 가능한 확장 → 충분한 통합/회귀 시험 → 발표용 안정 버전 확보.
- LED BAR는 현재 LED+저항 array 회로의 실제 표시 성능을 확인해 결정. Multi-process/thread는 실제 필요와 남은 시간에 따라 선택.
- 안정 버전 확보 후 신규 기능 중단 시점을 정한다. 실제 시작/발표 날짜는 아직 사용자와 확정하지 않았다.

현재 추천 Commit Message: `feat: add rotary encoder and PWM speed control`
