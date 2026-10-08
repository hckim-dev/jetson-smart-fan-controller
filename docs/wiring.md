# 사진 확인 결과와 상세 연결표

갱신: 2026-10-08. 사용자 사진 두 장을 원본 해상도로 확인하고 Hardware reviewer와 교차 검토했다.

**추가 상세 사진 반영:** 현재 연결 안내는 [Jetson 핀 → 부품 단자 표](pin-connections.md)를 우선한다.
아래는 첫 두 사진 기준 조사 기록이다. 새 사진으로 BMP180/GY-68 단자와 motor IN+/IN−,
L298 Header/terminal label, Encoder `5V`, LED/저항 network 표시가 추가 확인됐다.
특히 아래 Encoder 3.3V 후보를 확정 배선으로 사용하지 않는다.

**현재는 배선 설계안이다. 아직 이 표대로 연결하거나 전원을 인가하지 않는다.**
모터 정격/전류, L298 단자·점퍼, 신호 buffer 및 Carrier 실물 확인이 남아 있다.
미확정 항목은 아래 표에 표시했다. 연결 확정 후 단계별 무전원 배선과 통전 검사 순서를 추가한다.

## 1. 사진으로 확인된 내용

| 부품 | 읽히는 표시 / 관찰 | 아직 부족한 정보 |
|---|---|---|
| Motor | `YwRobot`, `EZ MOTOR R300`, 빨강/검정 2선과 2극 connector | motor 본체 정격·단자 극성·기동/정상 전류 |
| LCD | `1602A`, I²C backpack, 4핀 `GND / VCC / SDA / SCL` | backpack chip 문자·전압·pull-up·LCD bit mapping |
| Encoder | 5핀 `GND / S1 / S2 / KEY / +` | 뒷면 회로/pull-up, 공급전압, 출력 특성 |
| LED BAR | 교차 검토에서 발광창 8칸으로 관찰 | 측면 모델·총 pin 수·pin1·극성 |
| 저항 network | SIP 부품 형태 | 인쇄·pin 수·공통 pin·내부 topology |
| L298 계열 module | 방열판, 2극 terminal 2개, 3극 terminal 1개, 제어 header 양 끝 jumper | 각 terminal/ENA/ENB/regulator jumper 라벨 및 뒷면 회로 |
| Sensor | 파란색 4핀 module과 금속 덮개 형태 sensor | 모델 및 VCC/GND/SDA/SCL 표시. BMP180으로 확정하지 않음 |

원본 사진: [앞뒤 부품 배치 1](img/20261008_102440.jpg), [앞뒤 부품 배치 2](img/20261008_102501.jpg).
사진의 좌우 위치/선색 대신 **부품에 인쇄된 단자 이름**으로 연결한다.

## 2. 사용자 확인 정보와 전원 판단

- 외부 전원: 사용자 답변 `5V 입력 가능`. 실제 공급장치의 **출력 5V 여부와 최대 A**는 추가 확인 필요.
- 멀티미터: 사용 가능.
- Motor: 사용자 답변 `R300 3.3V 예정`. **계획 전압**이며 정격 전압 확인으로 기록하지 않는다.

Jetson Header pin1/17의 3.3V는 여기서 모터 전원으로 사용하지 않는다.
GPIO HIGH의 3.3V와 모터 power supply의 3.3V는 역할과 전류가 다르다.
L298N은 3.3V regulator가 아니다. 5V motor supply에서 모터 양단 전압은 bridge drop·전류에 따라 변한다.
PWM 평균값 역시 기동전류/순간 단자전압의 안전성을 대신 검증하지 않는다.

[ST L298 datasheet p.5](https://www.st.com/resource/en/datasheet/l298.pdf)의 logic supply는 4.5–7V이며,
motor supply의 operative 최소 조건은 `VIH + 2.5V`다. L298 supply를 3.3V로 넣는 설계를 채택하지 않는다.
실제 정격이 3.3V이고 L298 손실이 부적합하면 regulated 3.3V motor supply와
전류에 맞는 MOSFET Driver 등 대안을 검토한다. 부품 선정 전에는 교체 배선을 확정하지 않는다.

## 3. Jetson 물리 핀 찾기

아래는 **번호 배열을 설명하는 그림**이다. 보드를 어느 방향으로 놓을지 지정하는 그림이 아니다.
실제 J12의 pin1 표기/보드 사진으로 방향을 먼저 확인한다. pin1을 찾은 뒤 홀수와 짝수 열을 따라 센다.

```text
       홀수 열             짝수 열
3.3V       1  o   o   2    5V
SDA        3  o   o   4    5V
SCL        5  o   o   6    GND
ENC S1     7  o   o   8    사용 안 함
GND        9  o   o  10    사용 안 함
사용 안 함 11  o   o  12    사용 안 함
사용 안 함 13  o   o  14    GND
MOTOR IN1 15  o   o  16    사용 안 함
3.3V      17  o   o  18    사용 안 함
사용 안 함 19  o   o  20    GND
사용 안 함 21  o   o  22    사용 안 함
사용 안 함 23  o   o  24    사용 안 함
GND       25  o   o  26    사용 안 함
사용 안 함 27  o   o  28    사용 안 함
MOTOR IN2 29  o   o  30    GND
ENC S2    31  o   o  32    MOTOR ENA
사용 안 함 33  o   o  34    GND
사용 안 함 35  o   o  36    사용 안 함
사용 안 함 37  o   o  38    사용 안 함
GND       39  o   o  40    사용 안 함
```

`MOTOR` 표시는 buffer의 입력으로 가는 **예약 신호**다. L298N 직결 표시가 아니다.
핀 기능 근거는 [하드웨어 분석](hardware.md)의 Carrier 자료와 로컬 NVIDIA mapping이며 실제 padmux는 아직 미확인이다.

| 목적 | 물리 pin | SoC port | 현재 gpiochip0 line offset |
|---|---:|---|---:|
| ENA 출력 후보 | 32 | PG.06 | 41 |
| IN1 출력 / 향후 PWM1 후보 | 15 | PN.01 | 85 |
| IN2 출력 후보 | 29 | PQ.05 | 105 |
| Encoder S1 입력 후보 | 7 | PAC.06 | 144 |
| Encoder S2 입력 후보 | 31 | PQ.06 | 106 |

`물리 32`와 `line offset 41`은 같은 신호를 다른 번호 체계로 표현한 것이다.
Breadboard에는 **물리 번호**를 사용한다. DT specifier 숫자는 cdev offset과 또 다를 수 있다.
2026-10-08 재조회에서 위 GPIO consumer는 `unused`, 방향은 input이었다. 이는 실제 pad routing/전압 검증이 아니다.

## 4. 1단계 모터: 선 하나당 한 행

**아래 전체 연결은 보류. 추가 buffer 회로와 전원/단자 확인 후 실행한다.**

| 선 ID | 출발점 | 도착점 | 역할 / 확정 조건 |
|---|---|---|---|
| M01 | Jetson 물리 32 | 외부 buffer EN 채널 입력 | 모터 ON/OFF. buffer 없으면 연결하지 않음 |
| M02 | buffer EN 채널 출력 | L298N `ENA` 신호 pin | ENA와 5V 점퍼 pin을 구분해야 함 |
| M03 | Jetson 물리 15 | buffer IN1 채널 입력 | 방향 및 향후 PWM |
| M04 | buffer IN1 채널 출력 | L298N `IN1` | 해당 IN1 label 확인 |
| M05 | Jetson 물리 29 | buffer IN2 채널 입력 | 한 방향 제어에서 LOW 유지 |
| M06 | buffer IN2 채널 출력 | L298N `IN2` | 해당 IN2 label 확인 |
| M07 | Motor의 확인된 + 단자 lead | L298N `OUT1` | 색상만으로 +를 확정하지 않음 |
| M08 | Motor의 확인된 − 단자 lead | L298N `OUT2` | OUT2는 bridge 출력; 공통 GND에 묶지 않음 |
| P01 | 확인된 motor 전원 + | L298N motor supply `VS` terminal | 보드에 `12V` 표기가 있어도 12V 입력 지시가 아님; 전압은 별도 확정 |
| P02 | Motor 전원 − | L298N `GND` terminal | 모터 전류용 적절한 전선, 신호 Breadboard를 경유하지 않음 |
| G01 | Jetson 물리 6 | 공통 신호 GND 접점 | driver GND와 기준 공유 |
| G02 | buffer GND | 공통 신호 GND 접점 | buffer 기준 전압 |
| G03 | 공통 신호 GND 접점 | L298N `GND` | 신호 기준 연결, motor current return과 구분 |
| L01 | 확인된 5V logic source | L298N logic `5V/VSS` 경로 | regulator jumper/단자 방향 확인 전 연결하지 않음 |
| L02 | 확인된 5V logic source | buffer VCC | 모터 supply와 다를 수 있음; supply 선택 미확정 |

Motor의 두 lead는 OUT1/OUT2 사이에 연결하며, 한 lead를 Jetson GPIO나 Jetson 3.3V/5V에 연결하지 않는다.
사진상의 왼쪽/오른쪽 2극 terminal을 OUT1/2라고 추정하여 배선하지 않는다.
L298 **IC 자체의 pin 번호**와 **module Header/terminal 위치**도 구분한다.

### 점퍼·저항·buffer

| 항목 | 계획 | 현재 실행 여부 |
|---|---|---|
| ENA jumper | GPIO 제어를 위해 5V 고정 연결을 해제해야 함 | ENA label/회로 확인 전 제거·신호 연결 보류 |
| ENB jumper | 쓰지 않는 B channel은 비활성화하도록 설계 | label/회로 확인 후 결정 |
| regulator jumper | motor supply와 onboard regulator 조건에 따라 설정 | ENA jumper와 혼동 금지, 변경 보류 |
| ENA pull-down | buffer 출력/L298 ENA측 → GND에 10kΩ 후보 | Jetson측에 연결하지 않음; board pull-up 확인 필요 |
| buffer 입력 default | GPIO측 제약에 맞는 weak pull/OE/default-OFF 검토 | >50kΩ 등 추가 부품 및 최종 회로 필요 |
| bypass | buffer VCC/GND 가까이에 0.1µF 후보 | capacitor 확보 및 회로 확정 필요 |

SN74AHCT125는 [TI datasheet](https://www.ti.com/lit/ds/symlink/sn74ahct125.pdf) 기준 외부 buffer 후보다.
확보한 실제 chip/package를 확인한 뒤 **chip pin 번호, OE, unused 입력, 저항 위치**를 확정한다.
현재는 chip 없이 버퍼를 생략하거나 신호끼리 바로 연결하는 대체를 허용하지 않는다.

```text
Jetson GPIO ── buffer ── L298N ENA/IN1/IN2
                         │
외부 전원 + ── VS        ├── OUT1 ── Motor ── OUT2
외부 전원 − ── GND       │
                  │      │
Jetson GND ────────┴── buffer GND
```

위 구성도는 연결 관계만 표현한다. Motor 전원 −와 L298 GND 사이에 motor return 경로를 별도로 확보한다.
전원 양극을 서로 묶는 그림이 아니다.

## 5. 이후 단계: Encoder 연결 후보

사진에서 Header 이름은 읽혔지만 출력 회로가 미확인이다. 다음은 역할 예약이며 직결 허가가 아니다.

| Encoder 인쇄 | 연결할 곳 후보 | 확인할 내용 |
|---|---|---|
| `GND` | 공통 신호 GND | pin 위치/label 확인 |
| `+` | 확인된 module 논리 전원, 3.3V 우선 검토 | 3.3V 동작 및 pull-up 경로 확인 |
| `S1` | 적합한 3.3V 입력 회로를 거쳐 Jetson 물리 7 | board pull-up/접점, TXB 구동 적합성 |
| `S2` | 같은 입력 회로를 거쳐 Jetson 물리 31 | 회전 방향 및 debounce |
| `KEY` | 초기에는 미연결 | push switch는 1/2단계 필수 아님 |

Encoder board의 10kΩ pull-up이 있다면 TXB와의 적합성을 따로 확인한다.
모듈을 5V로 공급해 S1/S2가 5V가 되는 배선은 채택하지 않는다.

## 6. 이후 단계: LCD / Sensor 연결 후보

### LCD1602A backpack

| 출발점 | 중간 경로 | LCD 인쇄 단자 | 상태 |
|---|---|---|---|
| Jetson 물리 3 (SDA) | 필요한 경우 I²C 전용 양방향 level shifter | `SDA` | chip/5V pull-up 확인 전 보류 |
| Jetson 물리 5 (SCL) | 동일 shifter의 다른 channel | `SCL` | 동일 |
| 공통 GND | 공통 신호 GND | `GND` | 최종 연결 때 공유 |
| LCD용 정격 logic supply | 정격 확인 후 지정 | `VCC` | 1602A 표기만으로 전압 확정 금지 |

Jetson 쪽 bus는 현재 `/dev/i2c-7`이다. SDA/SCL을 서로 교차하지 않는다.
5V PCF8574 회로라면 단순히 pull-up을 3.3V로 옮기는 것으로 High threshold까지 보장되지 않는다.
[NXP PCF8574 datasheet](https://www.nxp.com/docs/en/data-sheet/PCF8574_PCF8574A.pdf).

### 파란색 sensor module

| 확인된 module label | 연결 후보 | 상태 |
|---|---|---|
| `VCC/VIN` | Jetson 물리 1의 3.3V 또는 확인된 정격 전원 | model/regulator/pull-up 확인 전 보류 |
| `GND` | 공통 신호 GND | label 위치 미확인 |
| `SDA` | Jetson 물리 3 | bus 전압/pull-up 확인 전 보류 |
| `SCL` | Jetson 물리 5 | 동일 |

이 표에서 label은 **기대하는 신호 이름**이다. 사진에서 4개 pin의 이름/순서를 판독했다는 의미가 아니다.
BMP180으로 확인되면 7-bit 0x77을 사용한다. LCD와 같은 bus에 놓을 수 있지만 주소 충돌·전압·pull-up을 확인한다.

## 7. LED BAR / 저항 network

아직 Jetson pin을 할당하지 않는다. 외부 LED output Driver와 세그먼트별 전류 제한이 필요하다.
8칸으로 관찰했지만 모델·pin1·극성이 확인되지 않아 기존 datasheet의 pin쌍을 배선에 적용하지 않는다.
SIP 저항 network도 공통 pin이 확정되지 않아 Breadboard에 꽂아 배선하지 않는다.

## 8. Breadboard 배치와 다음 확인

최종 Breadboard hole 좌표는 **실제 Breadboard 사진 + 추가 buffer package** 확인 후 작성한다.
현재 부품 사진만으로 `a10` 같은 hole 번호나 rail의 내부 연결을 지정하지 않는다.

- motor current path: 외부 전원 ↔ L298 screw terminal ↔ Motor. 신호 rail과 분리.
- 논리 영역: buffer, logic supply, bypass 및 신호 GND. 3.3V/5V rail을 구분.
- Encoder/LCD/Sensor: 각 단계 실제 테스트가 끝난 뒤 한 부품씩 추가.
- 무전원 rail continuity 검사 시 연결 장치/전원을 분리한다. 저항/continuity 모드로 전압을 측정하지 않는다.
- 전류 측정은 회로에 직렬로 넣는 작업이다. 멀티미터 A 모드 probe를 supply +/−에 병렬로 대지 않는다.

**추가로 필요한 자료**

1. 외부 전원 출력 라벨(5V/최대 A), `3.3V 예정`의 근거가 되는 모터 자료 또는 motor 본체 표시.
2. L298 앞면 정면 근접 사진: 3극 terminal label, OUT label, ENA/IN/ENB, regulator jumper 표시가 읽히게. 뒷면도 필요.
3. Jetson 40-Pin Header와 pin1 표기, 현재 연결 상태, Carrier 표시 및 Breadboard 사진.
4. buffer/level shifter를 추가 확보할 수 있는지와 보유 부품의 정확한 모델.
5. 다음 확장 전: LCD chip 문자, sensor 양면과 pin label, Encoder 뒷면, LED/저항 network 측면 표시.

**[내가 할 작업]** 현재는 표시/사진/자료를 확인하고 전달한다. 추가 배선·점퍼 변경·통전은 보류한다.

**[정상 결과]** 단자·전원·buffer 조건이 확정되어 이 표의 보류 항목을 실제 연결 지시로 바꿀 수 있다.

**[문제가 있을 경우]** label이 안 보이면 정면 가까이에서 찍거나 인쇄 문자를 그대로 전달한다.
모터 정격을 알 수 없으면 교수님 지급 자료를 확인한다. 임의 통전/전압 상승으로 식별하지 않는다.
