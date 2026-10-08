# BMP180 측정 / AUTO 제어 실행

2026-10-08. 사용자 확인: VIN=3.3V, SDA/SCL idle≈3.3V, 공통GND 및 J12 3/5 배선.
에이전트가 실제 chip ID0x55와 calibration을 읽어 온도·기압 측정에 성공했다.
첫 측정은27.0°C/1021.55hPa, 이후 실제 sensor process의32-byte IPC에서도
27.4°C/1021.42hPa와 sample age34ms를 확인했다. 정밀 교정 기준과 대조한 값은 아니다.

## 기존 배선 / 등록

| Jetson J12 물리 핀 | BMP180(GY-68) |
|---|---|
| 1번에서 공급한3.3V rail | VIN |
| 9번 또는 공통GND | GND |
| 3번 | SDA |
| 5번 | SCL |

LCD의3.3V 시험 구성과 SDA/SCL bus를 공유한다. BMP180 주소는0x77이다.
기존 `/dev/i2c-7`과100kHz 구성을 사용하므로 추가 DT 설치·GPIO export·reboot는
필요 없다. 원자적 SET_AUTO와 보호 정지 차단을 반영하려면 새 smartfan module을
다시 적재한다. 전원/핀 mapping은 [기존 연결표](pin-connections.md)를 따른다.

## 실행 순서

빌드부터 종료/오류 검증까지는 [최종 테스트 순서](final-test.md)를 따른다.
구형 module에서는 AUTO 진입을 거부하므로 새 module을 반드시 재적재한다.

현재 실행 중인 fanctl/bmp180-monitor를 Ctrl+C로 종료한다. standalone monitor와
통합 sensor process를 동시에 실행하지 않는다. 다른 I²C tool도 측정 중에는 쓰지 않는다.

```sh
cd /home/aidl/work/jetson-smart-fan-controller
./build/bmp180-monitor --once
```

온도와 기압이 정상 범위로 출력되는지 확인한다. 센서 단독 측정은 모터를 요청하지
않으며 `aidl`의 i2c 그룹 권한으로 실행할 수 있다.

통합 제어는 다음 순서다. 현재 module이 적재돼 있음을 확인한 상태의 명령이다.
`rmmod`가 busy라면 실행 중인 controller를 종료한 뒤 진행한다.

```sh
sudo rmmod smartfan
sudo insmod driver/smartfan.ko
sudo ./build/fanctl --bmp180 --lcd --lcd-address 0x27
```

LCD 주소는 이전에 실제 표시된 주소를 사용한다. LCD를 쓰지 않으려면 해당 두
option을 빼고 `sudo ./build/fanctl --bmp180`으로 실행한다.
시작 상태는 MANUAL/OFF다. SENSOR 결과가 출력된 뒤 **CLI 안에** 입력한다.

```text
status
mode auto
on
status
off
mode manual
quit
```

`mode auto`는 모터를 OFF로 두고 현재 온도의 목표 풍속만 선택한다. `on`이
AUTO 제어를 명시적으로 arm한다. 온도가 낮아 목표0이면 OFF를 유지하고,
그 뒤 온도가 상승하면 arm된 상태에서 자동 기동한다. `off`는 arm을 해제하므로
이후 온도가 올라가도 다시 켜지지 않는다. 모드 변경도 항상 OFF/disarm부터 처리한다.

## 기본 AUTO 온도 정책

처음 선택할 때의 구간이며, 냉각 시에는 각 경계보다1°C 낮아져야 하강한다.

| 온도 | 목표 풍속 | ON 중 LED Bar |
|---|---|---|
| 24°C 미만 | 0 / 정지 | 0칸 |
| 24~26°C 미만 | 1 | 2칸 |
| 26~28°C 미만 | 2 | 4칸 |
| 28~30°C 미만 | 3 | 5칸 |
| 30~32°C 미만 | 4 | 7칸 |
| 32°C 이상 | 5 | 8칸 |

예:2단계에서25.5°C는2단계 유지,25°C 미만이면1단계로 내린다.
1단계에서23°C 미만이면 정지한다. 이 정책은 프로젝트 시연용 기본값이며
센서 자체의 온도와 실제 주변 온도를 같은 것으로 보장하지 않는다.

AUTO에서 `speed/up/down/led N` 및 encoder 입력은 풍속을 덮어쓰지 않는다.
직접 제어하려면 `mode manual`로 돌아간다. `led auto`는 허용한다.
기존60/70/80/90/100% PWM과200ms boost,2초 lease,30초 최대ON 정책은 유지한다.

## 오류 / 재시작 정책

- 센서 NACK, identity/calibration 오류, 범위 밖 값, IPC 오류/child 종료 또는
  sample age3초 이상이면 AUTO를 OFF/disarm한다. 정상값이 돌아와도 자동
  재시작하지 않는다. fresh sample을 확인한 뒤 명시적인 `on`이 필요하다.
- 센서 process는 측정 오류 후 종료한다. 연결을 복구하고 CLI를 다시 실행한다.
  MANUAL에서는 센서 오류를 표시하되 사용자의 모터 제어를 유지한다.
- 최대ON30초/lease 만료/PWM 오류/suspend 정지 후 AUTO는 재시작하지 않는다.
  sample freshness는 CLOCK_BOOTTIME으로 절전 시간도 포함한다.
- 센서 conversion/I²C는 child가 수행하므로 blocking I²C가 main heartbeat를
  막지 않는다. Main은 고정길이 메시지의 partial read/sequence/범위를 검사한다.
- 종료 시 motor fd를 먼저 닫아 정지시키고 child에 SIGTERM/필요 시 SIGKILL,
  waitpid로 정리한 뒤 LCD worker를 마친다. Parent가 죽으면 child도 종료하도록
  PR_SET_PDEATHSIG를 설정한다. Kernel 작업이 끝나지 않는 child 정리에는 제한이 있다.
- LCD join은2초 뒤 반환한다. timeout 시 worker 객체는 해제하지 않고 CLI가
  오류로 종료한다. Kernel I²C 작업의 실제 종료 시간까지 보장하는 상한은 아니다.

## 화면 / 출력

```text
FAN ON AUTO
T:27.4C S:2/5
```

센서 option을 쓰면 두 번째 LCD 행은 온도·풍속을 표시한다. 센서 오류/값 만료는
`T:ERR`로 표시한다. 기압은 terminal의 SENSOR 출력에 hPa로 표시한다.
BMP180은 습도를 측정하지 않는다. LCD의3.3V 대비 부족은 아직 개선 항목이다.

## 하드웨어 없이 시연 로직 시험

```sh
./build/fanctl --dry-run --bmp180 --lcd
```

CLI에서 다음을 입력한다. 이 모드의 온도와 기압은 **가짜 값**이며 GPIO/I²C를 쓰지 않는다.

```text
temp 27
mode auto
on
temp 25.5
temp 24.9
temp 22
temp 28
temp error
temp 32
status
on
off
quit
```

`temp error` 뒤 정지하고, `temp 32`만으로 재시작하지 않는지 확인한다.
아무 새 temp도 주지 않고3초 기다리면 stale 정지도 확인할 수 있다.
온도 주입은 **dry-run 전용**이다. 실제 모터에 가짜 온도를 넣어 구동하지 않는다.

보정식 기준: [Bosch BMP180 DS000-09 §3.5](https://cdn-shop.adafruit.com/datasheets/BST-BMP180-DS000-09.pdf).
예제 UT27898/UP23843/OSS0을15.0°C/69964Pa로 검증했다. 실제 측정은OSS3,
temperature conversion5ms와 pressure conversion26ms 이후 SCO 완료를 확인한다.
