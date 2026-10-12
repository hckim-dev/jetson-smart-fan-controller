# 최종 통합 실물 테스트

현재 정상 배선과 전원을 유지합니다. 전원 한계와 연결표는 [hardware.md](hardware.md)를 먼저 확인합니다. 발표 전 반복할 수 있는 최소 시나리오이며, 확인한 항목만 성공으로 기록합니다.

## 1. 빌드와 센서 단독 확인

기존 fanctl/encoder monitor/bmp180-monitor를 종료하고 저장소 root에서 실행합니다.

```sh
make all
make test
./build/bmp180-monitor --once
```

C9개, CLI29개, build cleanup1개 검사가 통과하고 BMP 온도·기압 한 줄이 나온 뒤 종료돼야 합니다. 통합 실행 중 별도 sensor reader를 띄우지 않습니다.

Driver를 변경한 경우 기존 controller를 종료하고 module을 재적재합니다. App만 변경했으면 재적재할 필요가 없습니다. Module이 없으면 `sudo insmod driver/smartfan.ko`를 실행합니다. 적재 실패 이후 그대로 진행하지 않습니다. 현재 DT는 적용돼 있어 추가 GPIO/PWM export·DT 설치·재부팅은 필요 없습니다.

## 2. 엔코더 단독

```sh
sudo ./build/encoder-monitor
```

시작의 debounce_us=0을 확인합니다. 정지5초 → 한 방향10딸깍 → 반대10딸깍 → 정지5초 → Ctrl+C로 종료합니다. 정지 중 STEP이 없고 양방향 카운트가 증가해야 합니다. 반복적인 sequence_gaps/invalid는 조사 대상입니다. EC11 변형에 따라 두 딸깍당 한 STEP일 수 있으므로 일정한 비율과 방향을 봅니다.

이 프로그램을 종료해야 통합 controller가 같은 GPIO를 요청할 수 있습니다.

## 3. 통합 MANUAL 시험

```sh
sudo ./build/fanctl --encoder --bmp180 --lcd --lcd-address 0x27
```

이 장비에서 외부tee 파이프 사용 시 입력 무응답을 관찰했으므로 위처럼 직접 실행합니다. 방향이 반대라면 `off`, `quit`로 종료하고 --reverse-encoder를 추가해 다시 실행합니다.

이후 명령은 **fanctl 안에 한 줄씩 입력**하고 결과를 관찰합니다. 각 명령의 CMD 표시와 상태 출력을 확인합니다. 정상 BMP 값은 반복 출력하지 않으며 status에서 현재 값을 봅니다.

| 순서 | 명령/동작                  | 정상 결과                             |
| ---- | -------------------------- | ------------------------------------- |
| 1    | `mode manual`, `status`    | MANUAL/OFF, sensor 정상               |
| 2    | `led 1`, `led 8`, `led 0`  | 모터 OFF에서 해당 칸 점등/소등        |
| 3    | `speed 2`, 로터리 양방향   | 선택 풍속0~5, 모터 정지 유지          |
| 4    | `speed 5`, `on`, `status`  | STATE ON, duty100%, LED8칸, 실제 회전 |
| 5    | 로터리/`speed 1`~`speed 5` | 선택 단계와 LED2/4/5/7/8칸, LCD 갱신  |
| 6    | 로터리로0까지 내리기       | 모터/LED 정지                         |
| 7    | 로터리로1이상 선택         | 선택만 변경, OFF 유지                 |
| 8    | `on`, `off`                | 명시적 기동/정지                      |

30초가 지나면 정상적으로 자동 정지합니다. 풍속 변경이나 반복 ON으로 이 제한을 연장하지 않습니다. PWM duty와 실제RPM을 같다고 기록하지 않습니다.

## 4. AUTO와 최대 구동

정상 SENSOR 값을 확인하고 `mode auto`, `on`, `status`를 입력합니다. 초기 구간은 <24°C=0, 24/26/28/30/32°C 이상에서1/2/3/4/5이며 하강 hysteresis는1°C입니다. 같은 온도 구간에서 로터리는 AUTO 목표를 바꾸지 않아야 합니다.

`off` 이후 온도가 높아도 정지를 유지해야 합니다. `on`으로 다시 arm하고 온도가 구동 구간에서 유지되는 동안30초 연속 관찰합니다. max_on으로 정지한 뒤 자동 재시작하지 않아야 합니다. 새 정상 sample을 확인한 명시적 ON에서만 재시작합니다. 목표0으로 냉각돼 정지한 경우는 arm을 유지하므로 이후 온도 상승 시 기동할 수 있습니다.

실제 모든 온도 구간을 시험하지 못하면 해당 항목은 소프트웨어 정책 검증으로 구분합니다. 가짜 온도 주입은 --dry-run 전용입니다.

## 5. 센서 중단/회복

AUTO에서 구동한 뒤 별도 터미널을 엽니다. 시작 시 출력된 **SENSOR_PROCESS pid**를 사용합니다. 아래12345는 실제 sensor PID로 바꿉니다.

```sh
sudo kill -STOP 12345
```

마지막 sample으로부터 약3초 뒤 AUTO disarm, 모터/LED 정지, LCD T:ERR를 확인합니다.

```sh
sudo kill -CONT 12345
```

정상 온도가 돌아와도 모터는 정지를 유지해야 합니다. 상태 확인 후 명시적 `on`으로 재시작합니다. 이는 sample 공급 중단 시험이며 실제 NACK/전기적 I²C 고장 시험과는 다릅니다. 실제 통신 오류로 worker가 종료됐으면 연결 복구 뒤 fanctl을 재실행합니다.

## 6. Controller 중단/lease

AUTO에서 구동한 뒤 `pgrep -a -x fanctl`로 **controller PID**를 확인합니다. Sensor PID와 다릅니다. 아래12345를 실제 controller PID로 바꿉니다.

```sh
sudo kill -STOP 12345
```

프로그램이 멈춰 heartbeat를 보내지 못해도 kernel lease가 약2초 뒤 모터/LED를 꺼야 합니다. 2.5초 이상 지난 뒤:

```sh
sudo kill -CONT 12345
```

프로그램이 돌아와도 자동 재기동하지 않아야 합니다. 새 SENSOR 값과 disarm 상태를 확인합니다. LCD는 STOP 동안 갱신되지 않으므로 실제 모터를 관찰합니다.

## 7. 종료와 전기적 확인

구동 중 Ctrl+C 또는 `quit`로 종료하고 모터/LED 정지, shell 복귀를 확인합니다.

```sh
pgrep -a -x fanctl
pgrep -a -x bmp180-monitor
```

별도 인스턴스가 없다면 출력이 없어야 합니다. LCD final OFF는 best-effort입니다. Worker join은2초를 기다리지만 uninterruptible kernel I²C 작업까지 절대 종료 상한을 보장하지 않습니다.

LCD VCC/5V rail의 정지·기동 전압, 모터/L298N 발열, 전원 정격과 전류 여유도 확인합니다. 실제 suspend는 SSH가 끊길 수 있어 이 기본 시나리오에서 제외했으며 별도 실물 미검증입니다.

## 성공 기록

| 항목                 | 현재 증거                                  | 발표 전 재확인             |
| -------------------- | ------------------------------------------ | -------------------------- |
| 기본 입출력·로터리   | 사용자 성공 보고                           | 위 MANUAL 순서             |
| AUTO 정지·복구       | 사용자 성공 보고, 구체적 fault 방식 미기록 | 복구만으로 기동하지 않는지 |
| 30초 제한            | 사용자 확인                                | max_on 상태/재시작 조건    |
| controller lease     | 사용자 확인                                | 중단/복귀 상태             |
| Ctrl+C 정지          | 사용자 확인                                | sensor child 종료 포함     |
| LCD 시인성·전원 여유 | 미완료                                     | 측정/대비 확인             |

사용자 확인 날짜·동작·터미널 출력·전압 측정값을 함께 남기면 재현성과 발표 근거가 됩니다. 실물 관찰 없이 checkbox를 채우지 않습니다.
