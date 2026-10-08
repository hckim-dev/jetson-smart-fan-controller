# L298N 전원 연결과 소프트웨어 없는 하드웨어 검사

2026-10-08. **아직 수행하지 않은 사용자 시험 절차**다.
현재는 motor 정격/기동전류가 미확인이다. Motor는 빼고 Driver와 저항 부하만 검사한다.

## +12V를 비워 두어도 되는가?

Motor 출력 기능을 검사하려면 **비워 두면 안 된다**.
`+12V` 표기는 출력 bridge 전원 VS이며 반드시12V를 넣으라는 뜻은 아니다.
`+5V`는 logic VSS의 경로다. 두 전원 경로가 모두 공급되어야 정상적인 출력 시험을 할 수 있다.
[ST L298 pin description / electrical characteristics / truth table](https://www.st.com/resource/en/datasheet/l298.pdf).

같은5V를 쓴다면 **두 terminal에서 각각5V rail로 한 선씩** 연결할 수 있다.
Terminal 사이를 직접 bridge할 필요는 없다. 같은 rail에 연결하면 전기적으로 같은 전압이다.

```text
검증된 5V rail ──┬── L298N +12V (VS)
                 └── L298N +5V  (VSS 경로)
공통 GND rail ─────── L298N GND
```

**전제:** onboard5V regulator를 사용하는 전원 점퍼의 기능을 확인하고,
외부에서+5V를 공급하는 mode로 설정한 뒤 위 연결을 한다. ENA/ENB jumper와 혼동하지 않는다.
3.3V rail을+5V logic supply로 사용하지 않는다.

## 0. 지금 먼저 할 무전원 확인

**[내가 할 작업]**

1. Motor, L298에 연결된 전원 및 모든 Jetson GPIO signal 선을 분리한다.
2. Jetson Header 배선을 바꿀 때는 Jetson 정상 종료 및 어댑터 분리 후 진행한다.
3. [모듈 앞면 사진](img/20261008_103657.jpg)에서 방열판을 위로 놓는다.
   ENA/ENB는 아래6핀 Header 양끝 점퍼다.
   전원용 점퍼 후보는 **왼쪽2구 출력 terminal과 아래3구 전원 terminal 사이의 독립2핀 점퍼**다.
4. 후보 점퍼를 제거하고 multimeter continuity/Ω mode로 각 노출 pin과 `+12V` terminal의 연결을 확인한다.
5. regulator의 부품 문자 및 해당 jumper가 regulator 입력 경로를 끊는지 확인한다.
   기능이 불명확하면 측정값/부품 문자를 전달하고 통전은 보류한다.
6. +5V↔GND, +12V↔GND에 지속적인 거의0Ω short가 없는지 확인한다.
   Capacitor 충전 때문에 처음 잠깐 낮게 보이는 값만으로 short라고 판정하지 않는다.

**[정상 결과]**

- 전원용 jumper와 ENA/ENB jumper를 구분한다.
- 흔한 regulator 입력-enable 구성이라면 후보 jumper의 한쪽이 VS(+12V)와 연속이다.
- 실물의5V 공급 mode가 확인되고 rail-to-GND hard short가 없다.

**[문제가 있을 경우]**

- 후보 jumper가 예상과 다르면 다른 점퍼를 임의로 제거하며 전원을 넣지 않는다.
- Jumper 한쪽이 VS와 이어진다는 사실만으로 전체회로를 확인했다고 판단하지 않는다.

[Handsontec module schematic p.2](https://handsontec.com/dataspecs/module/L298N%20Motor%20Driver.pdf)의
regulator enable은 VS→regulator 입력 경로를 전환한다. 이는 참고 회로이며 HW-095의 동일 회로를 보장하지 않는다.
이 구성에서는 regulator 출력이+5V rail에 계속 연결될 수 있다.
이전의 `점퍼 제거 = regulator 출력선 자체 분리` 설명은 일반화하지 않는다.

## 1. Motor 없는 저항 부하 시험 배선

0번 확인을 통과한 뒤에만 진행한다. 필요한 저항은 **1kΩ 2개, 10kΩ 1개**다.
OUT용1kΩ는 최소1/8W 정격을 쓴다(5V에서 최대25mW).
저항이 부족하거나 정격이 불명확하면 다른 선으로 대체하지 않는다.

**[내가 할 작업]** Motor는 계속 분리하고 전원이 꺼진 상태에서 다음을 연결한다.

| 출발점 | 도착점 | 역할 |
|---|---|---|
| Jetson 물리4 —5V | 5V rail | Driver 단독 시험 전원 |
| Jetson 물리1 —3.3V | 3.3V rail | 수동 logic HIGH 공급, GPIO 출력이 아님 |
| Jetson 물리6 —GND | GND rail | 공통 기준 |
| 5V rail | L298 **+12V** | VS 공급 |
| 5V rail | L298 **+5V** | VSS 경로 공급, 전원 jumper mode 확인 필수 |
| GND rail | L298 **GND** | 전원 return |
| 3.3V rail | L298 **IN1** | A channel 방향 HIGH |
| GND rail | L298 **IN2** | A channel 방향 LOW |
| GND rail | L298 **ENB / IN3 / IN4** | 미사용 B channel 비활성화 |
| L298 **ENA** | **10kΩ를 거쳐 GND rail** | 기본 LOW |
| L298 **OUT1** | **1kΩ를 거쳐 OUT2** | Motor 대신 사용하는 작은 저항 부하 |

ENA/ENB enable jumper는 제거하고, 각 pair 중 **실제 EN 신호 pin**을 사용한다.
반대편5V jumper pin을 EN이라고 가정하지 않는다. 무전원 continuity로 silk와 IC signal을 대조한다.
Jetson GPIO32/15/29는 이번 시험에서 **연결하지 않는다**. LED/LCD/Encoder도 이번 부하에서 제외한다.

## 2. 전원 및 수동 ON/OFF 측정

**[내가 할 작업]**

1. Multimeter를 DC voltage mode로 설정한다. Power rail 측정의 검정 probe는 GND에 둔다.
2. 전원을 켜고 L298의+5V↔GND, +12V↔GND를 각각 측정한다. 둘 다 약5V여야 한다.
3. ENA↔GND를 측정한다. 기본 LOW이며1.5V 이하인지 확인한다.
4. 출력은 **빨강 probe OUT1 / 검정 probe OUT2**로 차동 측정한다.
5. 준비한 두 번째1kΩ 저항을 사용해 **3.3V rail→1kΩ→ENA** 경로를 연결한다.
   기존 ENA→10kΩ→GND는 유지한다. ENA 전압이2.3V 이상인지 측정한다.
6. OUT1−OUT2 전압을 측정한다.
7. 3.3V→1kΩ→ENA 공급 경로만 분리해 OFF한다. OUT1−OUT2를 다시 측정한다.
8. 한 번 성공한 뒤 ON/OFF를 반복해 변화가 일관되는지 확인한다.

수동 조작은 소전류 ENA 시험 경로만 바꾼다. 통전 중 Jetson Header와5V power 배선을 꽂거나 빼지 않는다.
3.3V rail은5V rail/GND와 직접 접촉시키지 않는다. IN1/IN2나 기타 배선을 바꾸려면 전원을 먼저 끈다.

**[정상 결과]**

| 상태 | 예상 관찰 |
|---|---|
| 기본 OFF | ENA LOW. 1kΩ 부하 양단의 차동 전압은0V에 가까움 |
| ON | ENA≥2.3V. OUT1−OUT2에 양의 전압이 나타남 |
| 다시 OFF | 저항 양단 차동 전압이 다시0V에 가까워짐 |

ON 전압은 bridge drop 때문에 정확히5V나3.3V라고 지정하지 않는다.
ENA LOW는 output high-Z다. 각 OUT↔GND가 반드시0V라는 뜻은 아니다.
원하면 전원을 끄고 IN1/IN2의3.3V/GND를 바꾼 뒤 반복하여 output 차동 전압의 음수 방향을 확인한다.
이는 Motor를 실제 역회전시키는 시험이 아니다.

**[문제가 있을 경우]**

- Rail 전압이 내려가거나 module이 빠르게 뜨거워지면 전원 공급을 중단한다.
- 출력이 변하지 않으면 VS/VSS/ENA/IN1/IN2 전압과 jumper/배선을 확인한다. 바로 module 불량이라고 판정하지 않는다.
- 전달할 값: **+5V, +12V, IN1, IN2, ENA OFF/ON, OUT1−OUT2 OFF/ON**.
- 전류계 A mode로5V와GND를 병렬 측정하지 않는다. 통전 중 continuity/Ω mode도 사용하지 않는다.

## 이 시험으로 검증되는 범위

이 절차는 전원 공급과 저전류 저항 부하에서의 H-bridge/ENA 응답만 확인한다.
Motor의 기동전류/정격/회전, GPIO 구동 전압, PWM, SSH 장애 정지는 검증하지 않는다.
Jetson Header5V 전류 예산은0.5A이며, 모터를 추가하면 다시 전체부하를 평가해야 한다.
[NVIDIA Carrier 표5-3](https://developer.nvidia.com/downloads/assets/embedded/secure/jetson/orin_nano/docs/jetson_orin_nano_devkit_carrier_board_specification_sp.pdf).
**저항 시험 성공 후에도 모터는 정격·전원 적합성 확인 전 연결하지 않는다.**
