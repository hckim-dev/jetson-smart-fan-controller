# 현재 하드웨어 구성

2026-10-12 기준입니다. 아래 Jetson 번호는 **J12 물리 핀 번호**이며 gpiochip offset과 다릅니다. 기존 정상 배선을 보존하며 새로운 전원 구성을 지시하는 문서가 아닙니다.

## 연결표

| Jetson / 공급원          | 부품 단자                           | 역할                                               |
| ------------------------ | ----------------------------------- | -------------------------------------------------- |
| 물리1 또는17 / 3.3V rail | BMP180 VIN, EC11 전원, LCD 모듈 VCC | 현재 실제 공급3.3V. EC11의5V 실크와 구분           |
| 물리6/9/14 등 공통 GND   | 센서/LCD/EC11/NPN/L298N GND         | 공통 신호 기준                                     |
| 물리3                    | BMP180 SDA, LCD backpack SDA        | header I²C, 현재 bus7                              |
| 물리5                    | BMP180 SCL, LCD backpack SCL        | 100kHz                                             |
| 물리32                   | L298N ENA                           | GPIO enable interlock, line41/PG.06                |
| 물리15                   | L298N IN1                           | Tegra PWM1 SFIO, PN1                               |
| 물리29                   | L298N IN2                           | GPIO fixed LOW 방향, line105/PQ.05                 |
| 물리12                   | S1용 NPN 컬렉터 C                   | GPIO 입력 line50/PH.07                             |
| 물리38                   | S2용 NPN 컬렉터 C                   | GPIO 입력 line52/PI.01                             |
| Jetson 5V rail(물리2/4)  | L298N +12V 표기 단자(Vs)            | **현재 사용자가 확인한 모터 전원 공급원**          |
| L298N OUT1/OUT2          | R300 모터 두 선                     | A channel, GPIO에서 모터 전류를 직접 공급하지 않음 |

Motor ENA cap은 GPIO 제어 때 제거한 상태입니다. L298N의+12V는 인쇄 이름이며 현재12V 전원을 넣는다는 뜻이 아닙니다. +5V 단자는 모듈 regulator jumper에 따라 입력/출력 역할이 달라질 수 있습니다. 동작한 현재 jumper/공급 구성을 임의로 변경하지 않습니다.

| LED segment | Jetson 물리 핀 | GPIO line |
| ----------- | -------------- | --------- |
| 1           | 13             | 122/PY.00 |
| 2           | 16             | 126/PY.04 |
| 3           | 18             | 125/PY.03 |
| 4           | 22             | 123/PY.01 |
| 5           | 33             | 43/PH.00  |
| 6           | 35             | 53/PI.02  |
| 7           | 37             | 124/PY.02 |
| 8           | 40             | 51/PI.00  |

LED는 각 분기의 기존 전류 제한 저항/network를 유지합니다. Kernel led-gpios 배열의 순서와 대응하며 SPI1 겸용 pad를 GPIO로 사용합니다. 관련 SPI1만 DT에서 비활성화하고 내장 fan PWM과 다른 SPI provider를 보존합니다.

## EC11 입력 보강

```text
3.3V -- 1kΩ --+-- Jetson 12 또는38
              |
              C
EC11 S1/S2 --10kΩ-- B  NPN
              E
              |
             GND
```

각 채널에 같은 회로를 사용합니다. 보유 소자의 MPS2/222A marking은 MPS2222A 제조사 자료와 맞는 후보입니다. 정확한 제조사/패키지를 확인하기 전 pin 순서를 다른2222 계열에서 복사하지 않습니다. KEY는 미사용입니다.

직결 때 양 채널의 정지 중 변화가 있었고, 고정 GND/3.3V 시험은 안정적이었습니다. 사용자 NPN+1kΩ 회로에서 회전 성공을 확인했습니다. 기본 debounce 기간0과 Gray cycle 검사를 적용합니다. 이 비교는 연결 조건의 영향을 보여주지만 translator 단독 고장이나 모든 전기 특성을 증명하지 않습니다.

[상세 NPN 전류/전압 검토](encoder-npn-review.md). TXB0108의50kΩ은 보수적 권장 조건이며 Rev.L p.3 Note D는 전압 변화가 허용될 때 작은 pull 저항의 조건부 사용을 설명합니다. 이번1kΩ의 동작을 다른GPIO 부하에 일반화하지 않습니다. [TI 사양](https://www.ti.com/lit/ds/symlink/txb0108.pdf)

## I²C 구성

BMP180은 address0x77, ID0x55, 온도·기압을 측정하며 습도를 측정하지 않습니다. LCD backpack은 현재0x27, RS=P0/RW=P1/E=P2/backlight=P3/data=P4..7 mapping으로 문자 표시를 확인했습니다. 두 장치는 bus7을 공유하며 sensor와 LCD의 fd는 별도입니다.

LCD 전체3.3V에서 문자는 표시되지만 대비 부족/최근 흐려짐이 보고됐습니다. 원인은 확정되지 않았으며 마지막 CLI 변경은 backlight/초기화 코드를 바꾸지 않았습니다. 5V LCD pull-up 신호를 레벨 변환 없이 Jetson/BMP180에 연결하지 않습니다.

## 전원 검증의 현재 한계

모터 Vs는 Jetson 5V 핀에서 공급 중이며 **전원 최대 전류, 모터 기동/정지 전류, 전체 부하의 전압 저하를 측정하지 않았습니다.** R300의 정격/사용점, L298N 전압 강하 및 header/shared rail 여유도 정량 판정하지 않았습니다. 기능 동작 보고와 전원 설계 완료를 구분합니다.

최종 발표 전 확인할 항목은 LCD VCC–GND와5V rail의 정지/기동/최대 풍속 전압, 모터/L298N 발열, 전원 정격과 실제 전류 여유입니다. 멀티미터 전압 측정은 병렬이며 전류 측정은 적절한 직렬/클램프 방식이 필요합니다. 전류 모드로 rail을 병렬 단락하지 않습니다. 자료가 부족하면 별도 전류 제한 공급원 등 전원 개선을 검토하되 이번 작업에서 배선을 바꾸지 않았습니다.

초기 후보 핀과 수동 시험 원본은 로컬 보관 자료입니다. 주요 시행착오와 해결은 [개발 기록](development.md)에 정리했습니다. 현재 연결은 이 표와 [NPN 회로](encoder-npn-review.md)를 기준으로 합니다.
