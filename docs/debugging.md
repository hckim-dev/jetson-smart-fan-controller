# 1단계 수동 구동 성공 / CLI ON 미회전 분석

2026-10-08. 사용자 보고와 읽기 전용 software 조사 결과를 구분한다.

## 현재 확인 결과

| 계층 | 결과 |
|---|---|
| Hardware 수동 | 사용자: ENA cap을 씌우고 IN1/IN2를 전원/GND에 연결하면 motor 회전 |
| CLI 시험 배선 | 사용자: 수동 시험 후 cap 제거, ENA→물리32 / IN1→15 / IN2→29로 복귀 |
| CLI / ioctl | `/dev/smartfan` live backend에서 OFF→ON→OFF 성공 로그 확인 |
| Driver ON 구현 | IN2 LOW → IN1 HIGH → ENA HIGH; OFF는 ENA LOW부터 처리 |
| DT mapping | Live provider phandle243, GPIO cells G6=54/N1=105/Q5=125, polarity active-high 일치 |
| GPIO consumer | PG6/PN1/PQ5가 enable/in1/in2 output으로 요청됨 |
| Kernel log | smartfan probe OFF/lease2000/max30000 정상 초기화. unsigned module taint 경고가 있으나 적재/probe는 성공; 이를 미회전 원인으로 판단하지 않음 |
| Pad configuration | smartfan에 pinctrl default state 없음. 기존 header state는 UART11/36만 설정 |
| PWM | 현재 ON/OFF 단계에 필요 없음. PWM controller enable로 GPIO output 문제가 해결된다고 가정하지 않음 |

## 가설

주요 가설은 PADCTL tristate/input 설정이 output 용도로 바뀌지 않았다는 것이다.
Kernel GPIO controller의 output 설정과 pad enable은 별도다.
`gpioinfo output` 또는 pinmux의 `UNCLAIMED`만으로 실제 Header output 전압을 판정하지 않는다.

공식 [NVIDIA L4T36.5.2 문서](https://docs.nvidia.com/jetson/archives/r36.5.2/DeveloperGuide/HR/JetsonModuleAdaptationAndBringUp/JetsonOrinNxNanoSeries.html)는
GPIO output에서 PADCTL bit10=0, bit4=0, bit6=0 설정을 설명한다.
현재 DTS에 해당 pad 설정이 없다는 것은 확인했으나, 실제 register가 잘못됐는지는 아직 읽지 못했다.

Local5.15.185 reference에서는 GPIO request가 SFSEL만 바꾸고 direction_output은 GPIO controller만 설정한다.
이 source를 실행 Kernel5.15.199와 동일하다고 주장하지 않는다. Installed NVIDIA source tree는 해당 C source가 없다.
최종 판정은 현재 provider의 debugfs configuration 또는 verified PADCTL read로 한다.

## 다음 read-only 확인

Jetson-IO도 `pinconf-groups`에서 function/tristate/enable-input/gpio-mode를 읽는다.
앞서 사용한 `pinconf-pins`의 빈 줄을 실제 pad configuration 결과로 해석하지 않는다.

```sh
sudo cat /sys/kernel/debug/pinctrl/2430000.pinmux/pinconf-groups | grep -Ei -A 14 'soc_gpio19_pg6|soc_gpio39_pn1|soc_gpio32_pq5'
```

이 명령은 설정을 변경하거나 모터를 ON하지 않는다.
출력에 tristate=1 또는 output 조건과 다른 값이 있으면 최소 pinctrl 수정안을 검토한다.
현재 분석에서는 DT/Pinmux/레지스터를 변경하지 않았으며, 재부팅도 하지 않았다.

Pad가 이미 output 조건이면, 수동 전원 신호와 Jetson3.3V GPIO 신호의 실제 전압 차이를 조사한다.
ENA/IN1의 부하 전압, ENA jumper의 실제 signal pin 및 남은 pull 저항을 확인한다.
원인을 확정하기 전에 Driver/timeout을 임의로 변경하지 않는다.

## 실제 pad 출력 비활성화 확인 및 수정

사용자 pinconf-groups 결과: PG6/PN1/PQ5 모두 `tristate=1`, `enable-input=1`, `gpio-mode=0`.
GPIO controller는 output으로 요청됐지만 pad는 high-Z/입력 상태였다.
이는 CLI ON이 실제 pin을 구동하지 못하는 원인이 될 수 있는 확인된 설정 오류다.

DTS에 별도 config-only `smartfan-output/outputs` state를 추가하고,
smartfan consumer의 `pinctrl-names=default`, `pinctrl-0`로 연결했다.
세 pad의 tristate/enable-input/gpio-mode만0으로 지정한다.
function/pull/open-drain/io-hv는 변경하지 않는다.

같은 BSP의 NVIDIA `jetson_36.5.2` source에서 function optional/config-only mapping 및
probe 전 default state 선택을 확인했다.
[NVIDIA pinctrl parser](https://gitlab.com/nvidia/nv-tegra/3rdparty/canonical/linux-jammy/-/raw/jetson_36.5.2/drivers/pinctrl/tegra/pinctrl-tegra.c),
[default 선택](https://gitlab.com/nvidia/nv-tegra/3rdparty/canonical/linux-jammy/-/raw/jetson_36.5.2/drivers/base/pinctrl.c).
State node의 자식 `outputs`에 속성을 두어 parser 구조를 맞췄다.

검증: `make prepare-dt`, installer `--check` 성공.
Offline DT의 pinctrl phandle, 세 pad 이름과0 설정을 확인했다.
Motor GPIO mapping/timeout 및 provider의 기존 pinctrl-0은 그대로 보존됨을 비교했다.
기존 boot 항목을 보존하고 별도 `smartfan-output` entry 및 새 DTB 경로를 준비했다.
아직 실제 수정안 설치/reboot/pin 전압·회전 검증은 수행하지 않았다.
