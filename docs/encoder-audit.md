# Encoder 무응답 조사 — 2026-10-08

현재 회전 무응답의 단일 원인은 아직 확정하지 못했다. 아래는 코드·live DT·실행
중인 kernel의 GPIO 요청과 공개 BSP source를 다시 대조한 결과다. 실제 회전과
동기화되지 않은 짧은 agent 시험을 회전 실패 증거로 사용하지 않는다.

## 확인한 사실

| 계층 | 확인 결과 | 의미 |
|---|---|---|
| 실행 환경 | L4T36.5.2, kernel5.15.199-tegra, gpiochip0=`tegra234-gpio` | 타 플랫폼 예제의 전역 GPIO 번호를 사용하지 않음 |
| 부팅 | DEFAULT=`smartfan-encoder-alt`; **live DT** encoder-inputs=`soc_gpio41_ph7`, `soc_gpio43_pi1` | 새 12·38번 구성이 실제 적용됨 |
| DT 입력 속성 | tristate=1, enable-input=1, gpio-mode=0, pull=0 | 입력 의도는 정상. 실제 pad readback은 sudo 진단으로 수집 |
| Driver bind | `/sys/bus/platform/devices/smartfan/driver`가 smartfan에 연결됨 | consumer의 default pinctrl 적용 경로가 존재 |
| GPIO mapping | 물리12=PH.07/offset50, 물리38=PI.01/offset52 | 설치된 NVIDIA Jetson.GPIO mapping 및 kernel line 이름과 일치 |
| IRQ 요청 | 독립 진단 실행 중 GPIO50·52 각각 `Edge smartfan-diag` 등록 | IRQ 등록 누락은 확인되지 않음. 실제 회전 edge 도착 여부는 별도 |
| 디코더 | 이벤트 수는 Gray decoding/sequence 재동기화보다 **먼저** 증가 | `events=0` 전체를 Gray decoder 버그로 설명할 수 없음 |
| CLI | event fd를 poll하고 batch당 최대32개 처리, 매 loop lease 관리 | 별도 encoder kernel module이나 PWM 추가 등록은 입력에 불필요 |
| software 검증 | C encoder tests 및 CLI13 tests 통과 | 실물 pulse 수신을 보장하지는 않음 |
| 수업 예제와 충돌 | practice의 devtest MMIO 예제 확인, 현재 devtest module/관련 앱 미실행 | 현재 관찰 시점의 예제 코드 동시 구동 근거 없음 |

기존 7·31번은 탈착으로 edge가 발생했으나 회전에는 반응하지 않았다는 사용자
관찰이 있다. 새 12·38번에서도 무반응이라는 보고를 받았지만, 해당 회전 시점의
pinconf·controller input·IRQ·event를 동시에 기록한 결과는 아직 없다.

## 확정된 진단상의 문제: debounce=0의 해석

현재 릴리스의 공개 [gpio-tegra186.c](https://gitlab.com/nvidia/nv-tegra/3rdparty/canonical/linux-jammy/-/raw/jetson_36.5.2/drivers/gpio/gpio-tegra186.c)
`tegra186_gpio_set_config()`는 기간0 요청에서도 threshold를0으로 쓰고
`GPIO_ENABLE_CONFIG`의 debounce-enable bit5를 **설정**한다.
[gpiolib-cdev.c](https://gitlab.com/nvidia/nv-tegra/3rdparty/canonical/linux-jammy/-/raw/jetson_36.5.2/drivers/gpio/gpiolib-cdev.c)
는 debounce attribute가 있으면 이 provider 함수를 호출한다.

따라서 `--no-debounce`를 실행했으니 하드웨어 필터를 완전히 우회했다고
단정했던 이전 설명은 부정확하다. 지금의 옵션은 **기간0 요청**이며,
`debounce_enable=0`을 입증한 것이 아니다. threshold0의 실제 영향과
회전 무응답의 인과관계는 별도로 확인해야 한다. 이 발견만으로 kernel 교체나
MMIO 쓰기를 해결책으로 실행하지 않는다.

## 가장 유력하게 남은 전기적 가설

사진 `img/20261008_103635.jpg`에는 여러 `103`(10kΩ) 저항과 capacitor가 있고,
push-pull buffer IC는 보이지 않는다. 저항 각각의 pull-up/직렬 연결 관계는
실물 회로도나 무전원 측정 없이 확정할 수 없다.

[NVIDIA carrier 사양](https://developer.nvidia.com/downloads/assets/embedded/secure/jetson/orin_nano/docs/jetson_orin_nano_devkit_carrier_board_specification_sp.pdf)은
해당 GPIO들의 TXB0108 경유를 명시한다. [TI TXB0108 datasheet §§7.1,7.3.2,7.3.5](https://www.ti.com/lit/ds/symlink/txb0108.pdf)는
push-pull CMOS source, 최소±2mA 입력 구동 능력, 외부 pull 저항50kΩ 초과를
요구한다. **50kΩ 초과 pull-up만 추가하면 접점 입력이 해결된다는 뜻은 아니다.**

접점+10kΩ/RC 출력은 이 입력 조건을 보장하지 않는다. 예를 들어10kΩ을 통해
3.3V를 공급하는 경로의 전류는 최대0.33mA이며 실제 threshold 부근에서는 더
작다. 무부하 멀티미터/일반 MCU/FPGA에서는 정상이어도 TXB0108에 연결하면
high/low가 유지되거나 전이가 무너질 수 있다. 탈착 때만 edge가 보이고 핀을
바꿔도 같은 증상이라는 관찰과 맞지만, 아직 실제 파형으로 확정한 것은 아니다.

## 지금 실행할 진단 하나

다른 encoder-monitor/fanctl --encoder/gpiomon을 종료하고 다음을 실행한다.
**20초 동안 손잡이만 양방향으로 천천히 계속 돌린다.** 선을 탈착하거나 모터를
켤 필요는 없다.

```sh
cd /home/aidl/work/jetson-smart-fan-controller
sudo ./build/encoder-diagnose 20 | tee build/encoder-diagnosis.log
```

`encoder-diagnose`는 `encoder.c`에 링크하지 않고 GPIOv2를 직접 요청한다.
Gray decoding과 초기 timestamp/sequence 필터를 사용하지 않는다. 같은 request
fd에서 A/B 값을 약1ms 간격으로 읽으면서 rising/falling event도 각각 센다.
`/proc/interrupts`의 시작·종료 snapshot, actual pinconf, controller 설정도 모은다.

`/dev/mem`은 **O_RDONLY, PROT_READ**로만 연다. live DT의 compatible 및 GPIO
resource가 예상값과 일치할 때만 read-only mapping을 사용한다. 제어 bank는
`0x2210000`이며 `0x2200000` security bank와 구분한다. PH7은 bank4/port1/pin7,
PI1은 bank4/port2/pin1이다. 레지스터 쓰기/DT 변경/출력 GPIO 요청은 하지 않는다.

실행 중 예상 설정은 `enable=1`, `output=0`, `trigger=3`(both edges),
`irq_enable=1`, `output_floated=1`이다. `debounce_enable`과 threshold는
실제 결과를 읽어 판단한다. pinconf는 위 DT 의도와 대조한다.

| 회전 중 결과 | 다음 조치 |
|---|---|
| pinconf의 input/tristate/gpio-mode가 다름 | 실제 pad 적용·다른 consumer의 재설정을 조사하고 해당 state를 수정 |
| sampled_changes 증가, IRQ/event=0 | GPIO filter/IRQ 전달 경로 우선. 정상 입력을 배선 문제로 단정하지 않음 |
| IRQ 증가, 독립 진단 event=0 | GPIO cdev 전달 경로/오류 기록 조사 |
| 독립 진단 event 증가, 기존 monitor만 실패 | 기존 request/read·debounce·decoder 경로를 비교해 수정 |
| 양쪽 sampled_changes=0, IRQ/event=0, pad 정상 | controller 이전 입력 경로에 집중. 아래 연결 상태 전압 또는 buffer 비교 |
| 한 채널만 edge 증가 | 나머지 채널의 pad/신호 경로를 분리 조사 |

1ms sampling은 짧은 pulse를 놓칠 수 있다. event=0과 함께 보더라도 멀티미터나
오실로스코프 없이 모든 물리 pulse 부재를 증명하지는 않는다.

## 결과에 따른 해결 방향

입력 경로 문제가 남으면 확인할 값은 **Jetson에 연결된 상태의 물리12·38번에서
공통GND 기준 low/high 전압**이다. encoder를 Jetson에서 분리한 상태의 측정값과
구분한다. 연결 시 전압이 무너지면 단순 pinmux 변경으로 고칠 수 없다.

TXB0108 입력 구동 문제가 확인되면 S1/S2 각각을 3.3V **Schmitt-trigger
push-pull buffer**에 통과시키는 구성이 적합하다. 예시는
[SN74LVC2G17](https://www.ti.com/lit/ds/symlink/sn74lvc2g17.pdf)이며3.3V에서
±24mA 출력 구동과 두 Schmitt 입력을 제공한다. SMD 부품은 빵판용 adapter가
필요하므로 실물 패키지 확인 없이 pin 번호로 바로 배선하지 않는다.

```text
Encoder S1 → buffer 1A → 1Y → J12 물리12
Encoder S2 → buffer 2A → 2Y → J12 물리38
buffer VCC → 3.3V, GND → 공통GND, VCC–GND 근처100nF
```

이 구성은 전압을5V로 올리는 방법이 아니라 입력 부하를 분리하고 edge를
재생성하는 방법이다. 현재 모듈의5V 실크만 보고5V rail로 옮기지 않는다.

## LED 첫 칸 깜빡임에 대한 정정

물리13번은 PY.00/offset122, SPI 겸용이다. 현재 프로젝트는 이 핀의 LED
출력을 구현하지 않았다. `gpioinfo`의 `unused input`만으로 actual padmux나
header 전압을 알 수 없으므로, 흔들 때 깜빡인다는 이유만으로 접촉 불량을
확정할 수 없다. 부동 신호, 다른 기능, 배선의 비의도적 경로 모두 남는다.
진단은 가능하면13번의 pinconf도 출력한다. LED 동작과 encoder 무응답의
공통 원인 여부는 아직 확인되지 않았다.
