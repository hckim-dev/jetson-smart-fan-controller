# 진행 상태 및 검증 기록

업데이트: 2026-10-08 (Asia/Seoul).

## 현재 단계

**1단계 software/DT/GPIO 구동 및 사용자 motor 회전 확인 완료. 종료·반복·재적재 검증은 남아 있다.**
최신 사용자 지시에 따라 hardware 저항 부하 시험을 코딩의 선행 조건에서 분리했다.
0단계 전원/배선 검증 및1단계 실물 확인은 아직 완료되지 않았다. 2단계로는 진행하지 않는다.

| 항목 | 상태 |
|---|---|
| Board/OS/L4T/Kernel/headers/toolchain 조사 | 확인 |
| GPIO/PWM/I²C controller와 기존 DT 상태 | 읽기 전용 확인; pad 전압/파형은 미확인 |
| Repository/수업 참고자료 | 확인; 기존 파일 변경 없음 |
| Architecture/Build | Driver/CLI/DT 구현, 교차 검토 및 native Build 성공 |
| Motor/L298N 전원 적합성 | 미확인 |
| GPIO 입력 적합성 및 초기 OFF 회로 | 실측 미검증 |
| 실물 pinout/전체 배선 | 미확정; hardware.md 표는 후보 |
| Driver/CLI 구현 및 Build | 완료; module vermagic 실행 Kernel과 일치 |
| CLI software test | dry-run integration9개 통과 |
| Hardware Test | 사용자 GPIO 제어 motor 회전 확인; 종료/만료/재적재 검증 대기 |
| Module load/출력/I²C probe/boot 변경/reboot | 미수행 |
| Git add/commit/push | 미수행; 사용자가 수행 |

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

1. 모터 라벨·모듈 앞뒷면·전원 정격·현재 배선을 확인한다.
2. L298N 입력 High/Low를 검증하고 직접 연결 가능 여부와 초기 OFF 회로를 확정한다. 추가 buffer 확보를 필수 전제로 두지 않는다.
3. Pinmux는 root-only debugfs 상태와 공식 pin 설정으로 추가 확인한다. 비밀번호는 전달받지 않는다.
4. 모터 기동/정상 전류, L298 전압 강하/발열, 회생·플라이백 경로 및 전원 용량을 확인한다.
5. 정확한 Breadboard 연결표를 확정하고 0단계 결과를 보고한다.
6. 사용자 확인 후 1단계 소스/Makefile/DT/CLI를 구현하고 build한다.

## 사용자 실물 확인

**[내가 할 작업]**

1. 현재 배선 상태를 먼저 기록하고, 추가 배선/전원 인가 없이 부품 표시를 확인한다.
2. 모터 라벨·연결 단자, L298N 앞뒷면·점퍼·터미널 표시, 전원 공급장치 출력 표기를 전달한다.
3. Encoder, LCD backpack chip, BMP180 모듈, LED BAR와 저항 network의 표시/방향을 확인한다.
4. 멀티미터 보유 여부와 현재 Header 연결 내역을 알려준다.

**[정상 결과]**

- 정확한 부품 모델/전압/단자/전원 정보가 확인된다. 이번 단계에서 모터 동작은 정상 결과에 포함하지 않는다.

**[문제가 있을 경우]**

- 표시가 없거나 회로가 불명확하면 사진 또는 구입 링크/교수님 부품 자료로 식별한다.
- 공급 전류나 모터 정격을 모르면 안전한 구동 검증이 끝날 때까지 전원을 인가하지 않는다.

## 1단계 검증 계획 (아직 실행하지 않음)

| 시험 | 확인할 동작 |
|---|---|
| Build | CLI warning 및 Kbuild/module ABI; compile 성공과 load 성공 구분 |
| 모터 분리 초기 검사 | load/probe/open/close/오류에서 EN OFF 실제 전압 |
| OFF/ON 반복 | 방향 고정, EN 전이, 공급 전류/모터 단자 전압/발열 |
| 종료/SIGTERM/SIGKILL | 마지막 fd close 정지, 재연결 시 OFF |
| heartbeat 중단/SIGSTOP | lease 만료 정지 및 최대 ON 제한 |
| 두 번째 controller | EBUSY, 기존 상태 손상 없음 |
| Module load/unload | open 시 unload 거부, OFF 후 정상 unload |
| platform remove/probe 실패 | OFF와 resource cleanup, stale fd 접근 거부 |
| SSH disconnect | CLI 생존 여부 포함하여 정지시간 실제 확인 |

실행 명령과 정상 Terminal 출력을 구현 완료 후 실제 CLI에 맞춰 작성한다.
Hardware 결과는 사용자의 관찰/측정 근거가 있을 때만 성공으로 기록한다.

## 이틀 일정

- 1일 차: 안전 검증 → ON/OFF 최소 완성 및 오류/종료 시험. 통과하면 Encoder/PWM, 여유 시 LCD.
- 2일 차: LCD/BMP180 중 가능한 확장 → 충분한 통합/회귀 시험 → 발표용 안정 버전 확보.
- LED BAR는 현재 LED+저항 array 회로의 실제 표시 성능을 확인해 결정. Multi-process/thread는 실제 필요와 남은 시간에 따라 선택.
- 안정 버전 확보 후 신규 기능 중단 시점을 정한다. 실제 시작/발표 날짜는 아직 사용자와 확정하지 않았다.

현재 추천 Commit Message: `docs: record stage 0 environment and hardware constraints`
