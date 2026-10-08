# LCD1602 / PCF8574T 실행 안내

**사용자 결과:**3.3V 전체 공급에서 문자는 표시되지만 contrast를 조정해도
잘 보이지 않는다고 보고했다. 기본 표시 경로는 동작했고 시인성 개선은 남아 있다.
가변저항의 끝 위치가 항상 최적 대비는 아니므로 양방향으로 천천히 조정한다.
모듈이5V 구동/대비를 요구할 가능성이 있으나,5V I²C 직결로 바꾸지는 않는다.

## 최신: 레벨 시프터 없는 3.3V 전체 공급 시험

사용자 요청에 따라 추가 converter 없이 **LCD와 PCF8574 backpack 전체를
3.3V로 공급하는 시험안**을 사용한다. I²C 코드와 핀 매핑은 동일하다.
PCF8574T는 2.5~6V 범위에서 동작하므로 3.3V 논리 조건은 맞지만, 실제 LCD
모듈의 glass 구동/contrast와 controller clone까지 검증된 것은 아니다.
3.3V에서 문자 표시가 정상임을 확인하기 전에는 완성된 연결로 기록하지 않는다.

2026-10-08 재확인: 실제 DEFAULT는 `smartfan-ledbar`, live I²C는400kHz다.
LCD용100kHz DT가 아직 설치되지 않았다. 따라서 기존 코드도 현재 bus에서
LCD 송신을 거부한다. `--dry-run --lcd`는 terminal preview이며 실물 LCD에 쓰지 않는다.

1. Jetson과 LCD 전원을 끈다. LCD VCC를 **5V rail에서 완전히 분리**한 뒤
   3.3V rail로 옮긴다. LCD와 backpack을 서로 다른 전압으로 나누지 않는다.
   SDA/SCL은 LCD를3.3V로 공급했을 때, Jetson에서 분리한 상태로 각 신호의
   idle 전압이3.3V 부근이고5V가 아님을 확인한 뒤 연결한다.

   | Jetson J12 물리 핀 / 빵판 | LCD backpack |
   |---|---|
   | 물리1에서 공급한 **3.3V rail** | **VCC** |
   | 물리20 또는 공통GND | **GND** |
   | 물리3 | **SDA** |
   | 물리5 | **SCL** |

2. LCD 신호를 분리한 상태에서도 아래 DT 설치는 가능하다. 다음을 실행하고
   재접속한다. `DT_READY` 또는 `DT_INSTALLED` 검사 오류가 있으면 진행하지 않는다.

   ```sh
   cd /home/aidl/work/jetson-smart-fan-controller
   sudo bash scripts/install-dt.sh
   sudo reboot
   ```

3. 재접속 후100kHz 적용을 확인하고, 위3.3V 신호 조건에서 주소를 확인한다.

   ```sh
   sudo fdtget -t u /sys/firmware/fdt /bus@0/i2c@c250000 clock-frequency
   i2cdetect -y -r 7 0x20 0x27
   ```

   첫 출력은 `100000`이어야 한다. 두 번째 표에서 실제 응답 주소를 사용한다.
   예를 들어 `27`이 보이는 경우에만 아래0x27로 실행한다. 주소가 없으면 그대로
  0x27을 가정해 쓰지 않는다.

   ```sh
   cd /home/aidl/work/jetson-smart-fan-controller
   sudo insmod driver/smartfan.ko
   sudo ./build/fanctl --lcd --lcd-address 0x27
   ```

   module이 이미 적재됐다면 insmod는 생략한다. `--dry-run`은 붙이지 않는다.
   모터를 켤 필요 없이 `speed 2`, `led 1`, `status`로 화면을 시험한다.

4. 파란 contrast 가변저항을 천천히 조정한다. ACK가 있고 초기화 오류가 없는데
   문자만 안 보이면 contrast, 실제 backpack mapping,3.3V에서의 LCD 동작 범위를
   구분해야 한다. 이 시험만으로 전원 문제 하나를 확정하지 않는다.
   모듈이5V 동작/대비 전압을 필요로 하면 소프트웨어로3.3V 구동을 보장할 수 없다.

아래5V+converter 설명은 별도 대안이며, 이번3.3V 전체 공급 시험과 섞지 않는다.

LED Bar 실물 시험은 사용자 성공 보고로 확인했다. LCD 코드 및100kHz DT는
준비했으나 실제 LCD 주소·backpack bit mapping·문자 표시 성공은 아직 미확인이다.

## 현재 배선에서 먼저 필요한 조치

사용자 답변은 **5V 공급 + PCF8574T/HW-061 backpack, 별도 시프터 없음**이다.
PCF8574T는 I²C GPIO expander이며 3.3↔5V level shifter가 아니다. 현재5V
직결 구성으로 추정되므로 LCD 전원을 끄고 SDA/SCL을 Jetson에서 분리해 둔다.
이 상태에서 I²C scan/write는 하지 않는다.

[NXP PCF8574T 사양](https://www.nxp.com/docs/en/data-sheet/PCF8574_PCF8574A.pdf)은
100kHz I²C와 supply 기준 input threshold를 규정한다. Jetson header의 신호는
3.3V이므로,5V backpack의 pull-up을 그대로 직결하지 않는다. SDA/SCL pull-up만
3.3V로 바꾸는 것도 PCF8574T의5V 공급 시 high 입력 조건을 보장하지 않는다.

5V LCD를 사용할 구성은 **I²C용 양방향3.3↔5V level shifter**를 거치는 것이다.
converter와 두 bus 측의 pull-up 병렬값/low sink 전류도 실제 모듈에 맞춰 확인한다.
실물 converter가 없으면 아래 hardware 명령은 진행하지 않고 software 시험만 한다.

| Jetson / 빵판 | Converter / LCD 단자 |
|---|---|
| 물리1에서 공급한3.3V rail | converter LV |
| 물리4에서 공급한5V rail | converter HV 및 LCD VCC |
| 공통GND | converter GND 및 LCD GND |
| 물리3 SDA | converter LV측 channel1 |
| converter HV측 channel1 | LCD SDA |
| 물리5 SCL | converter LV측 channel2 |
| converter HV측 channel2 | LCD SCL |

이 표의 LV/HV channel 표시는 converter의 실제 silk와 맞춰 사용한다.
LCD 전체를3.3V로 공급하는 대안은 PCF IC 동작 범위만으로 LCD glass/contrast와
module 전체 동작까지 보장하지 않으므로, 별도의 module 확인이 필요하다.

## 지금 가능한 software 시험

```sh
cd /home/aidl/work/jetson-smart-fan-controller
./build/fanctl --dry-run --lcd
```

CLI에서 `speed 2`, `led 1`, `status`, `quit`를 입력한다. Terminal에LCD 두 행의
preview가 나오며 GPIO/PWM/I²C를 사용하지 않는다.

## 100kHz DT 설치

조사 당시 `/proc/device-tree/bus@0/i2c@c250000/clock-frequency`는400000이었다.
PCF8574T의100kHz 사양에 맞춰 같은 bus를100000으로 변경한다.
새 `smartfan-lcd` 항목과 `/boot/dtb/smartfan-lcd.dtb`를 설치하고 기존 LED Bar
boot 항목/DTB는 복구용으로 남긴다. GPIO/LED/motor/encoder mapping은 유지한다.
LCD를 분리한 상태에서도 DT 설치는 가능하며, 설치 자체는 I²C 송수신을 하지 않는다.

```sh
cd /home/aidl/work/jetson-smart-fan-controller
bash scripts/install-dt.sh --check
sudo bash scripts/install-dt.sh
sudo reboot
```

재접속 후 다음 출력이100000인지 확인한다.

```sh
sudo fdtget -t u /sys/firmware/fdt /bus@0/i2c@c250000 clock-frequency
```

## 주소 확인 및 실제 표시 — 전압 변환 배선 완료 후

확인된 header bus는`/dev/i2c-7`이다. PCF8574T 후보 주소0x20~0x27만 읽는다.
다른 bus/주소를 무차별 탐색하지 않는다. 이 명령도 실제 bus 통신이다.

```sh
i2cdetect -y -r 7 0x20 0x27
```

예를 들어`27`이 응답하면 아래 명령을 사용한다. 다른 주소면 해당값으로 바꾼다.
PCF8574A variant는0x38~0x3f이므로 실제 chip이A라면 그 범위를 별도로 확인한다.
ACK는 expander가 응답한다는 뜻이며 HD44780 문자 표시 성공을 뜻하지 않는다.
`UU`이면 kernel consumer가 점유한 주소이므로 강제로 덮어쓰지 않는다.

```sh
cd /home/aidl/work/jetson-smart-fan-controller
sudo insmod driver/smartfan.ko
sudo ./build/fanctl --lcd --lcd-address 0x27
```

module이 이미 적재돼 있으면 insmod는 생략한다. `--lcd`의실제 구동에는 주소를
명시해야 한다. 기본 bus는7이며 다른 검증된 bus를 쓸 때만`--lcd-bus PATH`를
지정한다. GPIO export/추가 LCD kernel module 등록은 필요 없다.

기본 화면은 다음과 같다.

```text
FAN OFF MANUAL
SPEED:5/5 LED:0
```

`speed 2`는 OFF를 유지하면서 선택풍속2를 표시한다. `led 1`은 LED독립 시험의
점등칸 수1을 표시한다. ON/OFF와풍속,자동정지/timeout도 약50ms 관찰 주기로
반영한다. 선택풍속을 표시하므로 startup boost 중에도 선택단계는 바뀌지 않는다.
현재는 [BMP180 / AUTO 구현](bmp180-run.md)도 포함한다. `--bmp180`을 켜면
LCD 두 번째 행은 온도·풍속으로 바뀌며 `mode auto`에서AUTO 모드를 표시한다.

## 구현 및 제한

- `app/lcd.c`는 PCF8574 port write로 HD44780의 4-bit instruction 초기화를 한다.
  P0=RS, P1=RW, P2=E, P3=backlight, P4..7=D4..7의 흔한 backpack mapping을
  **가정**했다. 실물 mapping은 아직 검증되지 않았다. RW는 항상 LOW이며 busy
  flag 확인 대신 instruction별 delay를 사용한다. [HD44780U 사양](https://www.sparkfun.com/datasheets/LCD/HD44780.pdf)
- 변경된 행만 고정 16자로 덮어써 잔여 문자와 반복 clear에 따른 깜빡임을 줄인다.
- I²C 전용 worker가 fd를 소유한다. Main은 최신 snapshot만 queue하며 I²C를
  기다리지 않아 motor heartbeat를 막지 않는다. LCD 오류는 warning 후 표시를
  중단하고 motor 제어는 계속한다. 실패한 transaction을 부분 재전송하지 않는다.
- 종료 시 motor를 먼저 정지/close한 뒤 LCD에 최종 OFF를 best-effort로 표시한다.
  worker join은 최대2초 기다린다. 시간 초과 시 살아 있는 객체를 해제하지 않고
  CLI가 오류로 종료한다. Kernel I²C 작업의 종료 시간은 별개다. SIGKILL/통신 오류 때는 LCD에
  이전 문자가 남을 수 있다. LCD를 물리적 motor 정지의 증거로 사용하지 않는다.
- I2C_SLAVE_FORCE는 쓰지 않는다. 기본 bus의 live DT가 100kHz가 아니면 송수신
  전에 거부한다. [Linux i2c-dev API](https://www.kernel.org/doc/html/v5.15/i2c/dev-interface.html)

Backlight만 켜지고 문자가 없으면 파란 contrast 가변저항을 조정한다. 검은
블록만 보이면 주소/초기화/mapping을 구분한다. `WARN LCD disabled: Operation not supported`
는 기본 bus의 100kHz DT 미적용 가능성이 있으며, `Remote I/O error`는 주소/NACK/
전원/배선 경로를 확인한다. 글자가 깨지면 실물 bit mapping과 신호 전압을 대조한다.
