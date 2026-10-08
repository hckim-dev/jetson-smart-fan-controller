# LED BAR / Rotary Encoder 강의 자료 재검토

2026-10-08. [사용자 제공 PDF](LEDBAR_로터리엔코더.pdf)의 9페이지를 이미지로 확인했다.
Basys3 pin/Verilog 구현은 프로젝트에 적용하지 않는다.

## 정정

이전의 `GPIO ±20µA → LED 직결 불가 → 외부 LED Driver 필수` 판단은 과도했다.
TXB0108의 ±20µA는 VOH/VOL 보장 시험 조건이며, 초과 즉시 손상되는 절대 한계가 아니다.
**현재 LED BAR + 저항 network로 표시 기능을 시험할 수 있다.** 추가 Driver는 밝기·신뢰성 측정 결과로 결정한다.

[TI TXB0108 Rev.L](https://www.ti.com/lit/ds/symlink/txb0108.pdf) p.3 note D는 출력 전압 변화를 허용하는 경우
작은 외부 저항도 검토할 수 있다고 설명한다. DC 출력이 약하므로 밝기·High 전압은 별도 확인이 필요하다.
절대최대값을 정상 구동 권장 전류로 사용하지 않는다.

같은 이유로 L298 input High current 100µA와 20µA를 비교한 것만으로 buffer 필수를 증명할 수 없다.
L298 직결 여부는 실제 High/Low 전압, board 점퍼, 기본 OFF 회로 및 전원 순서로 검증한다.
Motor 정격/기동전류/전원 용량 미확인은 계속 구동 전 확인할 사항이다.

## PDF에서 실제 확인한 내용

| 페이지 | 적용할 내용 |
|---|---|
| 1 | LED BAR + 저항 network 배선과 점등 사진, 모델 인쇄된 쪽이 anode(+), array 점이 common 위치라는 안내 |
| 2–5 | Encoder S1/S2 quadrature 원리 및 이전/현재 상태 변화로 방향 판정 |
| 6–9 | Basys3/Artix7/Pmod/Verilog 자료. Jetson pin 번호로 사용하지 않음 |

1페이지의 제어 board는 사진 밖에 있어 Jetson 시연이라고 확인한 것은 아니다.
하지만 추가 구동 IC가 없는 LED+resistor 구성과 점등은 자료에서 확인했다.

## LED 연결 원리

```text
Jetson GPIO ── LED anode(+) ── LED cathode(−) ── 330Ω branch ── common GND
```

8개 LED마다 저항 branch가 하나씩 있어야 한다. array의 common은 GND에 연결한다.
LED 인쇄된 면의 8개 lead가 +라는 강의 설명은 실제 부품 diode 검사로 한 번 대조한다.
LED BAR는 독립 LED 8개이며 내부 common cathode를 가정하지 않는다.

9A331G는 9핀이다. 점 표시가 있는 끝을 임시 pin1로 세고, 반대쪽까지 2–9로 센다.
이 번호는 부품 확인용 번호이며 Jetson 물리 번호와 무관하다.

### 지금 할 수 있는 무전원 검사

**[내가 할 작업]**

1. 저항 array와 LED를 어떤 전원/보드에도 연결하지 않은 상태로 놓는다.
2. Multimeter Ω mode에서 array의 점 있는 pin1 ↔ pin2–9를 각각 측정한다.
3. pin2 ↔ pin3도 측정한다.
4. Diode mode에서 LED 한 칸의 인쇄면 쪽 lead에 빨강 probe, 반대 lead에 검정 probe를 댄다.

**[정상 결과]**

- Bussed 330Ω array라면 pin1 ↔ 각 나머지는 약330Ω, pin2 ↔ pin3은 약660Ω.
- LED는 정방향 전압이 표시되거나 약하게 발광할 수 있다. Meter 시험 전압에 따라 발광하지 않을 수도 있다.

**[문제가 있을 경우]**

- 330/660Ω 관계가 아니면 측정값을 전달하고 배선을 보류한다.
- LED 양방향 모두 OL이면 meter 특성/단자 짝을 확인한다. 즉시 5V를 인가하지 않는다.

### 측정 후 LED 표시 기능 시험

모터를 연결하지 않고, 검증된 330Ω branch 및 3.3V GPIO 경로만 사용해 한 segment부터 확인한다.
GPIO를 동시에 다른 논리 입력에 연결하지 않는다.
실제 current는 branch 저항 양단의 DC voltage를 측정하여 `I = V_R / 실제 R`로 계산한다.
예를 들어 330Ω에서 0.10V가 측정되면 약0.30mA다. 이는 예시이며 실측 기록이 아니다.
밝기·OFF·반복 점등·8칸 동시 점등은 단계적으로 평가한다.
GPIO 출력 명령은 pinmux/ownership 확인 후 해당 구현 단계에서 제공한다. 이번 조사에서는 출력하지 않았다.

## Encoder 재검토

실물은 접점·저항·capacitor의 수동 module로 보인다. `5V` 실크만으로 3.3V 공급 불가능을 단정하지 않는다.
추가 level shifter를 확보할 수 없으므로 **3.3V rail만 공급하는 시험안**을 검토한다.
이때 module의 `5V`라고 적힌 전원 pin에 실제로 공급하는 전압은 3.3V다.
5V rail은 Encoder 시험에서 연결하지 않는다.
S1/S2 High/Low와 RC/접점 bounce에 따른 edge를 확인한 후 정상 회전 입력 여부를 판정한다.
5V 공급 상태의 S1/S2를 Jetson에 직결하는 배선은 채택하지 않는다.

## 새 실물 사진과 제약

[보드/Breadboard 사진](img/20261008_105050.jpg)에서 J12 silk의 1/2, 반대 끝39/40을 확인했다.
제공 사진 방향에서 pin1/2는 USB-C connector에 가까운 끝이다. `1` 표시 쪽 row가 홀수, `2` 표시 쪽 row가 짝수다.
보드를 돌리면 화면상의 좌우가 바뀌므로 silk 번호를 기준으로 센다.
빵판 rail은 일부 중앙에 표시가 끊어져 있다. 내부 단절 여부는 무전원 continuity로 확인한 후 같은 전압끼리 bridge한다.

사용자 확인: 외부 전원5V, 최대 A는 모름. 추가 buffer/level shifter 확보 불가.
따라서 추가 부품 구매를 전체 작업의 필수 전제로 두지 않고, 현 부품으로 가능한 검증부터 진행한다.
모터 통전과 5V LCD I²C 직결은 별도 안전 검증 없이 실행하지 않는다.
