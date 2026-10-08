# Jetson 핀 → 부품 단자 연결표

**현재 작업은 LED Bar**다. 아래 LED1~8 매핑을 그대로 사용하는 코드와 새 DT를
준비했다. [LED Bar 설치·시험 순서](ledbar-run.md)를 따른다. 실제 출력 적용은
새 DT 설치·재부팅·새 module 적재 후이며, 이전 `예약안` 표현은 조사 이력이다.

**Encoder 현재 소프트웨어/DT 구성**: S1→물리12, S2→물리38. 새 DT의 실제 부팅 적용을 확인했지만 회전 입력은 아직 무응답이다. [최신 조사와 진단 명령](encoder-audit.md), [변경·복구 순서](encoder-alt-pin-trial.md)를 따른다. Encoder 전원=3.3V, KEY미사용; Motor ENA→32, IN1→15(PWM1), IN2→29. 아래 초기 후보 표는 조사 이력이다.

2026-10-08 상세 사진 반영. **여기의 Jetson 번호는 모두 J12의 물리 핀 번호**다.
Linux GPIO 번호나 gpiochip offset으로 세지 않는다.

**현재 확인 수준:** 부품 이름·단자 표시를 확인했다. 전원·buffer 및 실제 신호 전압 검증은 남아 있다.
표의 `확인 후 연결` 항목은 아직 꽂지 않는다. 통전/통신/모터 동작 시험은 수행하지 않았다.
보드 pin1 위치를 확인하고 전원을 끈 상태에서 배선한다.
[새 보드 사진](img/20261008_105050.jpg)에서 J12의 USB-C 쪽 끝이 pin1/2임을 확인했다.
silk `1` row는 홀수, `2` row는 짝수이며 반대 끝은39/40이다.
강의 자료에 따른 전기적 판단 정정은 [재검토 문서](lecture-review.md)를 따른다.

## 1. BMP180 — 네 선

사진에서 `BMP180`, `GY-68`, `VIN / GND / SCL / SDA`가 읽힌다.
인쇄된 이름을 기준으로 꽂는다. 앞면·뒷면은 좌우가 반전되므로 사진의 왼쪽부터 외워 연결하지 않는다.

| Jetson 물리 핀 | BMP180 모듈에서 찾을 단자 | 선의 역할 |
|---|---|---|
| **1번 — 3.3V** | **VIN** | 센서 전원 후보; 아래 전원 확인 필요 |
| **9번 — GND** | **GND** | 전원 기준 |
| **3번 — SDA** | **SDA** | I²C 데이터; 센서 내부 I/O 전압 확인 후 연결 |
| **5번 — SCL** | **SCL** | I²C clock; 동일 조건 |

단자 확인 사진: [모듈 뒷면](img/20261008_103730.jpg), [모듈 앞면](img/20261008_103724.jpg).
이것이 BMP180의 **연결 목적지 표**다. 실제 GY-68 regulator/pull-up 공급 경로가 확인되지 않아 통전 보장은 아니다.

- VIN은 3.3V 공급을 우선 검토한다. `VIN`이라는 이름만으로 5V를 넣지 않는다.
- onboard regulator가 있으면 VIN과 sensor의 VDD/VDDIO가 다를 수 있다. Header의 3.3V pull-up과 sensor I/O 전압이 맞는지 확인한다.
- 센서 전원과 SDA/SCL 전압 확인 전에 I²C 선을 연결해 전원을 인가하지 않는다. 저항/pull-up을 추가하지 않는다.
- BMP180 확인 주소는 7-bit **0x77**, 이 Header bus는 현재 **`/dev/i2c-7`**이다.
- 모듈 사진에서 확인한 사실과 실제 chip identity/통신 성공은 구분한다.

기준: [Bosch BMP180 datasheet pp.6,19](https://cdn-shop.adafruit.com/datasheets/BST-BMP180-DS000-09.pdf).
VDD 1.8–3.6V, VDDIO 1.62–3.6V이고 I²C 입력 High의 상한은 VDDIO로 표기된다.
따라서 VIN 3.3V라는 이유만으로 regulator 뒤의 I/O 호환성을 확정하지 않는다.

## 2. LCD1602A — 전원 두 선 + I²C 두 선

LCD 뒤 **검은 backpack의 4핀**을 쓴다. 녹색 LCD 보드의 16핀에는 Jetson을 연결하지 않는다.
검은 보드에 `GND / VCC / SDA / SCL`이 읽히지만 IC 종류는 아직 판독되지 않는다.

| Jetson 물리 핀 | LCD의 단자 | 연결 방법 / 상태 |
|---|---|---|
| **4번 — 5V** | **VCC** | 실제 5V module인지 확인 후 연결 |
| **20번 — GND** | **GND** | 공통 GND |
| **3번 — SDA** | **SDA** | **I²C 전용 양방향 level shifter를 거쳐 연결**; shifter 선정 전 보류 |
| **5번 — SCL** | **SCL** | 같은 shifter의 다른 channel을 거쳐 연결; 보류 |

사진: [LCD backpack](img/20261008_103737.jpg).

5V LCD 회로라면 신호 경로는 아래와 같다.

```text
Jetson 3번 SDA ── shifter 3.3V측 SDA ── shifter 5V측 SDA ── LCD SDA
Jetson 5번 SCL ── shifter 3.3V측 SCL ── shifter 5V측 SCL ── LCD SCL
```

shifter의 `LV/HV/GND` 및 channel pin 이름은 확보한 실제 모듈에 맞춰 지정한다.
5V pull-up 신호를 Jetson에 직결하지 않는다. PCF8574라면 5V 공급에서 3.3V High도 보장되지 않는다.
[NXP PCF8574 datasheet](https://www.nxp.com/docs/en/data-sheet/PCF8574_PCF8574A.pdf).
BMP180과 LCD가 SDA/SCL을 공유할 때 pull-up 병렬값과 bus 전압도 다시 확인한다.

## 3. Rotary Encoder — 5핀 중 회전 제어는 네 핀

새 사진에서 전원 단자는 **`5V`**다. 실물은 수동 접점/RC module로 보이므로,
추가 변환 부품 없이 **실제 공급을 3.3V로 제한한 시험안**으로 변경한다. label과 실제 공급 전압을 구분한다.
뒷면에 `103` 저항(10kΩ)들이 보인다. High/Low 및 edge 인식은 시험해야 한다.

| Jetson 물리 핀 | Encoder 단자 | 연결 방법 / 상태 |
|---|---|---|
| **1번 — 3.3V 빵판 + rail에서 분배** | **5V라고 인쇄된 전원 pin** | 실제 공급은3.3V인 시험안. 5V rail에는 연결하지 않음 |
| **14번 — GND** | **GND** | 공통 GND |
| **7번 — 입력 후보** | **S1** | 3.3V 공급 상태에서 직접 입력 시험 후보. High/Low 확인 후 |
| **31번 — 입력 후보** | **S2** | 동일 조건. 회전 방향·bounce/edge 기능 시험 필요 |
| **연결하지 않음** | **KEY** | 누름 버튼. 초기 회전 제어에서는 사용하지 않음 |

사진: [앞면 단자](img/20261008_103640.jpg), [뒷면 저항](img/20261008_103635.jpg).
S1/S2는 Encoder에서 Jetson으로 들어오는 신호다. 5V 공급 상태 직결은 채택하지 않는다.
I²C용 level shifter 하나를 모든 GPIO의 해결책으로 사용하지 않는다.

## 4. L298N — Jetson 제어 신호 세 선

**Motor와 Jetson 사이에는 L298N이 필요하다. GPIO→L298N 입력의 직결/버퍼 선택은 전압 및 기본 OFF 검증으로 결정한다.**
20µA와100µA 수치 비교만으로 buffer 필수라고 단정했던 이전 판단은 정정한다.
현재 motor 정격/전원 용량/기본 OFF 회로는 미검증이므로 아직 모터를 구동하지 않는다.

| Jetson 물리 핀 | L298N 단자 | 연결 방법 / 상태 |
|---|---|---|
| **32번 — ON/OFF 출력 후보** | **ENA** | 3.3V 직결 후보. 점퍼·기본 OFF 회로·High/Low 검증 후 결정 |
| **15번 — 방향/PWM 출력 후보** | **IN1** | 3.3V 직결 후보. GPIO/PWM 선택 및 padmux/입력 전압 미검증 |
| **29번 — 방향 출력 후보** | **IN2** | 3.3V 직결 후보. 입력 전압 검증 후 결정 |
| **6번 — GND** | **GND** | 신호 기준 GND. 모터 전류 return은 별도 전선으로 구성 |
| **연결하지 않음** | **IN3 / IN4 / ENB** | B channel은 이번 모터에 사용하지 않음; 비활성화 회로 별도 설정 |

사진: [앞면](img/20261008_103657.jpg), [뒷면 단자 표시](img/20261008_103704.jpg).
앞면을 사진처럼 **방열판 위 / 제어 header 아래**로 놓으면 header는 왼쪽부터
`ENA / IN1 / IN2 / IN3 / IN4 / ENB`다.
ENA/ENB에는 점퍼가 꽂혀 있다. 점퍼의 두 pin 중 어느 것이 enable 신호인지 무전원 continuity로 확인한다.
ENA 점퍼를 유지한 채 GPIO를 붙이지 않는다. 제거 작업은 default-OFF 회로와 함께 안내한다.
Module 표시는 `HW-095`이며 diode 8개에 `M7` 표시가 보인다.
제조사/recovery time은 미확인이다. ON/OFF 검증과 PWM 적합성 검증을 분리한다.
[ST의 유도성 부하/PWM diode 요구사항](https://www.st.com/resource/en/datasheet/l298.pdf)을 2단계에서 확인한다.

## 5. Motor와 외부 전원 — Jetson 핀에 꽂지 않음

`+5V`만 넣고`+12V`를 비워 두면 motor 출력 전원을 공급한 것이 아니다.
같은5V 공급원을 쓰는 Driver 단독 시험의 경우 두 terminal에 각각5V를 분배한다.
전원 jumper 확인과 motor 대신 저항을 사용하는 순서는 [하드웨어 검사](l298-hardware-test.md)를 따른다.
이는 Header에서 실제 motor를 구동해도 된다는 확인이 아니다.

새 사진의 실크스크린으로 모터의 **빨강 = IN+**, **검정 = IN−**를 확인했다.

| 출발점 | 도착점 | 상태 |
|---|---|---|
| L298N **OUT1** | Motor **IN+ — 빨간 선** | 모터 정격·전원 검증 후 연결 |
| L298N **OUT2** | Motor **IN− — 검정 선** | 같은 조건; OUT2를 공통 GND에 묶지 않음 |
| 검증된 외부 motor 전원 **+** | L298N **+12V** 표기 terminal | `+12V`는 보드 표기. 실제 입력 전압은 모터와 L298 조건으로 결정 |
| 외부 motor 전원 **−** | L298N **GND** terminal | 모터 전류용 별도 return |
| 검증된 logic 5V | L298N **+5V** 경로 | regulator jumper 상태 확인 후 결정. 입력/출력 역할을 단정하지 않음 |

사진: [Motor 극성](img/20261008_103803.jpg).
L298 뒷면 사진의 3극 단자는 왼쪽부터 `+5V / GND / +12V`다.
**앞면으로 뒤집으면 좌우가 반전**된다. 실제 label을 기준으로 사용한다.
같은 뒷면 사진에서 오른쪽 2극 단자는 위 `OUT1`, 아래 `OUT2`; 반대편은 B channel 출력이다.

모터를 Jetson 1번/17번 3.3V 또는 2번/4번 5V supply에 직접 연결하지 않는다.
5V external supply가 있다는 정보만으로 최대 전류나 motor 적합성이 확인되는 것은 아니다.
`R300 3.3V 예정`은 계획이며 정격 확인이 아니다. L298은 5V→3.3V regulator가 아니다.
[ST L298 datasheet](https://www.st.com/resource/en/datasheet/l298.pdf).

## 6. LED BAR와 저항 network

사진에서 **WCNLB8-SR12 / K 2002**, **9A331G / 9pins**를 확인했다.
모델은 식별됐지만 LED pin1 방향/극성 및 저항 network 공통 pin은 추가 확인이 필요하다.

| Jetson 물리 핀 | LED BAR / 저항 network | 현재 계획 |
|---|---|---|
| **아래8개 GPIO 예약안** | LED 8개 | LED+330Ω array 회로 시험 검토. 추가 LED Driver를 필수로 두지 않음 |
| **직접 연결 없음** | 9A331G | 무전원 저항 측정으로 내부 연결 확인 후 사용 |

사진: [부품 표시](img/20261008_103756.jpg).
강의 p.1에서 인쇄된 면은 LED +측, array 점 표시 pin은 common이라는 안내를 확인했다.
실물 무전원 측정으로 330Ω bussed array와 극성을 확인한 후 한 칸부터 평가한다.
[구체적인 검사 방법](lecture-review.md).

다음은 motor/encoder/I²C 후보와 겹치지 않는 **예약안**이다. 아직8칸을 통전하는 지시가 아니다.
LED 발광면을 보고 모델 인쇄 글자가 아래에서 똑바로 읽히도록 놓은 뒤 왼쪽부터1–8칸으로 센다.

| Jetson 물리 pin | LED BAR 도착점 | 해당 LED 반대쪽(−) lead의 도착점 |
|---|---|---|
| **13** | **1칸 + lead** | array pin2 |
| **16** | **2칸 + lead** | array pin3 |
| **18** | **3칸 + lead** | array pin4 |
| **22** | **4칸 + lead** | array pin5 |
| **33** | **5칸 + lead** | array pin6 |
| **35** | **6칸 + lead** | array pin7 |
| **37** | **7칸 + lead** | array pin8 |
| **40** | **8칸 + lead** | array pin9 |
| **GND rail** | **array pin1 — 점 표시 있는 끝** | common GND |

Array pin 번호는 점 있는 끝을1로 세는 부품 확인 번호다. LED lead 번호를 임의로 찍은 표가 아니다.
각 칸의 +/− lead를 diode mode로 확인한다. GPIO를 LED +에, 반대 −를 저항 branch에 연결하는 원리다.
GPIO High이면 ON인 후보이며 5V rail은 LED 신호 회로에 연결하지 않는다.
예약 pin은 현재 gpioinfo consumer가 unused였으나 실제 padmux/출력 시험은 아직 미수행이다.

## 7. Breadboard에서 선을 나누는 방법

사용자 확인: **3.3V용 빵판과 5V용 빵판을 별도로 사용한다.**
사용자가 공급원을 **Jetson의 3.3V·5V 핀**으로 확인했다. 빵판 자체가 전압을 변환하는 것은 아니다.

| Jetson 물리 핀 | 빵판의 rail | 연결할 전원선 |
|---|---|---|
| **1번 — 3.3V** | **3.3V 빵판 + rail** | BMP180 VIN 후보 및 검증된 LV 논리 전원 |
| **4번 — 5V** | **5V 빵판 + rail** | 정격 확인한 LCD VCC / 필요한 logic 전원. Encoder 시험에서는 사용하지 않음 |
| **6번 — GND** | **3.3V 빵판 − rail** | 공통 GND의 시작점 |
| **3.3V 빵판 − rail** | **5V 빵판 − rail** | 두 영역 GND 연결; 각 부품의 GND를 해당 − rail에서 분배 |

위 표에서는 한 핀에서 빵판으로 한 선을 넣고 부품으로 분배한다. LCD와 Encoder 때문에 4번 pin에 선을 여러 개 꽂지 않는다.
2번/17번 power pin은 위 분배에서 사용하지 않아도 된다.

| 영역 | 놓을 부품 / 연결점 | 다른 영역과 이어질 것 |
|---|---|---|
| **3.3V 빵판** | BMP180 후보, 3.3V 공급 Encoder 시험, LED+저항 시험 영역, 필요한 LV 회로 | 공통 GND / 3.3V 신호 |
| **5V 빵판** | 정격 확인한 LCD, 필요한 motor logic 회로, 확보 가능할 때 shifter HV측 | GND 및 검증된 신호 |
| **별도 motor 전력 경로** | 외부 전원·L298 terminal·Motor | 공통 신호 GND, buffer 출력; 빵판 power rail을 motor return으로 사용하지 않음 |

```text
3.3V 빵판 (+) ── 3.3V 논리 부품       5V 빵판 (+) ── 5V 논리 부품
3.3V 빵판 (−) ───────── 공통 GND ───────── 5V 빵판 (−)
                        │
                 Jetson / L298 GND

3.3V 신호 영역 ── 적합한 shifter/buffer ── 5V 신호 영역
```

두 빵판의 **+ 레일끼리는 연결하지 않는다**. − 레일은 실제 GND인지 확인 후 공통으로 연결한다.
별도 regulator가 빵판에 달려 있으면 Jetson 1번/2번 power를 같은 + rail에 동시에 넣지 않는다.
각 rail은 하나의 검증된 전원에서 공급하고, 아래 Jetson 전원 분배는 Jetson을 그 rail의 공급원으로 선택한 경우에만 적용한다.

- **공통 GND rail:** 확정된 후 Jetson GND와 부품 GND를 공유. 위 표의 6/9/14/20번은 모두 GND다.
- **3.3V rail:** Jetson 1번을 기준으로 검증된 sensor/shifter LV 전원에 분배. 모터 전원은 넣지 않는다.
- **5V logic rail:** 4번에서 분배한다(2번도 같은 5V supply지만 여기서는 미사용). 검증된 논리 회로만 사용하며 Encoder/LED 직결 시험에는 쓰지 않는다.
- **SDA 연결점:** 물리 3번 → 검증된 BMP180 SDA 및 LCD shifter의 LV SDA.
- **SCL 연결점:** 물리 5번 → 검증된 BMP180 SCL 및 LCD shifter의 LV SCL.
- Motor current path는 Breadboard 신호 rail을 거치지 않고 외부 전원↔L298↔Motor로 별도 구성한다.

한 Jetson pin에 wire를 여러 개 억지로 꽂지 않고 Breadboard의 같은 연결 구간에서 분기한다.
실제 hole 좌표는 Breadboard 사진과 rail continuity 확인 후 지정한다.

## 남은 확인

1. 외부 전원은5V, 최대A는모름으로 확인됐다. Motor 구동 전에는 전원 용량과 motor 정격 확인이 필요하다.
2. 추가 buffer/I²C level shifter 확보 불가로 확인됐다. 현재 부품 검증안으로 진행하고 5V LCD I²C 직결은 허용하지 않는다.
3. J12 pin1/2와 Breadboard 사진을 확인했다. 실제 rail 내부 continuity와 전원 전압을 확인한다.
4. BMP180 regulator/VDDIO/pull-up 경로를 확인해 실제 I/O 전압과 Header 3.3V bus를 대조한다.
5. LCD chip 문자가 보이면 전달한다. 표시가 없다면 unknown chip으로 두고 5V I²C 보호 회로부터 검증한다.

측정할 때는 DC voltage mode로 해당 GND 기준 전압을 측정한다.
통전 중 resistance/continuity mode를 사용하지 않고, A mode로 supply 양극을 병렬 측정하지 않는다.
소형 sensor pad에 probe를 대는 위치는 regulator 식별 후 지정하여 인접 pin short를 피한다.
