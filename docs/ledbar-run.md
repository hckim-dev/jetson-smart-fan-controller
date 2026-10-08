# LED Bar 구현 및 시험

**최신상태:** 사용자점등시험성공보고를받았다. 현재DT설치script의proposal은
LCD용100kHz구성으로갱신됐으며LED기능은유지한다. [LCD실행안내](lcd-run.md).

엔코더 실물 무응답 진단은 보류하고 LED Bar를 먼저 구현했다. 엔코더 monitor를
실행하지 않아도 `fanctl`에서 LED를 독립적으로 시험할 수 있다.
현재는 코드·build·software 검증 단계이며 실제 LED 밝기·반복 점등은 미확인이다.

## 배선 기준

아래 번호는 **J12 물리 번호**다. GPIO offset 또는 BCM 번호로 세지 않는다.
LED1~8은 기존 연결표와 같은 방향으로 센다. 배선을 바꿀 때는 전원을 끈다.

| Jetson 물리 핀 | LED Bar + 단자 | LED 반대쪽 − 단자 | GPIO line |
|---|---|---|---|
| 13 | 1칸 | 330Ω array 분기 pin2 | PY.00 / offset122 |
| 16 | 2칸 | array pin3 | PY.04 / offset126 |
| 18 | 3칸 | array pin4 | PY.03 / offset125 |
| 22 | 4칸 | array pin5 | PY.01 / offset123 |
| 33 | 5칸 | array pin6 | PH.00 / offset43 |
| 35 | 6칸 | array pin7 | PI.02 / offset53 |
| 37 | 7칸 | array pin8 | PY.02 / offset124 |
| 40 | 8칸 | array pin9 | PI.00 / offset51 |
| 공통 GND | — | array common pin1(점 표시) | — |

각 LED에330Ω 분기가 하나씩 있는 구성이며 array common은GND다.
저항 없이 GPIO와 LED를 연결하거나 LED 신호를5V rail에 연결하지 않는다.
실물 array가 bussed330Ω인지 확인되지 않았다면 무전원에서 common↔각 분기
약330Ω, 분기↔분기 약660Ω인지 먼저 측정한다.
Carrier GPIO의 LED 부하 구동·밝기는 [강의 재검토](lecture-review.md)의 실제
측정 기준을 따른다. 출력 요청 성공만으로 적합한 밝기/전류를 보장하지 않는다.

## 설치 후 LED만 시험

새 DT는 `smartfan-ledbar` 부팅 항목과 `/boot/dtb/smartfan-ledbar.dtb`를 사용한다.
기존 `smartfan-encoder-alt`/`smartfan-speed`/Stage1/JetsonIO는 복구용으로 남는다.
LED와 겹치는 header SPI1(`spi@3230000`)은 비활성화하고 SPI0/UART/내장 fan은
보존한다. 해당 LED 핀에서는 SPI1/I2S 기능을 동시에 사용하지 않는다.

모터 전원을 끈 상태에서 LED 시험을 진행한다. 에이전트가 실행한 build·검증은
아래 설치를 수행하지 않는다. 사용자 실행 순서는 다음과 같다.

```sh
cd /home/aidl/work/jetson-smart-fan-controller
bash scripts/install-dt.sh --check
sudo bash scripts/install-dt.sh
sudo reboot
```

재접속 후 새 module을 적재한다. reboot 후에는 이전 수동 insmod가 유지되지
않는다. 자동 적재로 `File exists`가 발생하면 CLI를 종료하고 기존 module을
`sudo rmmod smartfan`으로 내린 뒤 새 파일을 적재한다.

```sh
cd /home/aidl/work/jetson-smart-fan-controller
sudo insmod driver/smartfan.ko
sudo ./build/fanctl
```

처음 출력은 `STATE OFF`, `LEDBAR available=1 mode=auto count=0/8`이다.
다음은 **CLI 안에 한 줄씩 입력**할 명령이며 shell 명령이 아니다.

```text
led 1
led 0
led 2
led 3
led 4
led 5
led 6
led 7
led 8
led 0
quit
```

첫 칸에서 밝기·OFF가 정상인지 확인한 뒤 차례로 늘린다. `led N`은 앞에서부터
N칸을 켜며 모터를 시작하지 않는다. 이상한 발열/전압 저하/다른 부품 영향이
있으면 `led 0`으로 끄고 해당 출력/전원 경로를 확인한다.

## 자동 표시 정책

`led auto`는 풍속/모터 상태 표시로 복귀한다. `on`, `off`, `speed N`도 시험
모드를 해제한다. 자동 모드에서 OFF는0칸이며 풍속은 다음과 같이 표시한다.

| 풍속 | ON 중 점등 칸 수 |
|---|---|
| 0 | 0 |
| 1 | 2 |
| 2 | 4 |
| 3 | 5 |
| 4 | 7 |
| 5 | 8 |

Startup boost의 일시적인100% duty 대신 사용자가 선택한 풍속 단계를 표시한다.
풍속 선택만으로 OFF 모터나 LED를 자동으로 켜지 않는다.
ON 중 `led N` 시험은 `EBUSY`로 거부한다. 실제 motor ON은 기존 모터/PWM 검증
조건에서만 실행한다. 엔코더가 없어도 `speed`, `up`, `down`으로 조작한다.

Driver가 GPIO 배열을 소유하고 모든 LED 변경을 기존 mutex 안에서 처리한다.
초기 probe, `off`, `quit`/close, SIGKILL로 인한 fd close, lease/max-on 만료,
remove/shutdown/suspend에서 모두 LOW로 끈다. ioctl6/7을 추가했고 기존 ABI
명령/struct layout은 유지했다. LED DT가 없는 이전 구성도 구동 가능하지만
`available=0`이며 `led` 설정은 지원하지 않는다.

## 점등하지 않을 때

- `available=0`: 새 DT 또는 새 module이 아직 적용되지 않았다.
- GPIO busy/probe 실패: `sudo dmesg | tail -n 50`와 GPIO 소비자를 확인한다.
- `available=1`, count가 바뀌지만 실물은 불규칙/미점등: 출력 pad readback,
  연결된 GPIO 전압, 저항 분기 전류와 LED 극성/순서를 확인한다.
- 순서가 반대: 물리적 LED1 방향을 기존 표와 대조한다.
- 정상 GPIO 출력인데 부하를 연결하면 전압/밝기가 무너짐: 구동 능력에 맞는
  LED buffer/driver가 필요할 수 있다. 성공한 ioctl을 실물 검증으로 세지 않는다.
