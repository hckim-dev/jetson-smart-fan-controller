# 최종 테스트 순서 — 로터리 제외

기존에 회전/점등/표시를 확인한 배선을 유지한다. LCD 전체3.3V 공급도 유지한다.
**추가 DT 설치, GPIO/PWM export, 재부팅은 필요 없다. 새 module 재적재는 필요하다.**

## 1. 기존 실행 종료 → 빌드 → 새 module 적재

현재 실행 중인 fanctl 터미널에서 `off`, `quit`를 차례로 입력한다.
별도 bmp180-monitor가 있으면 Ctrl+C로 종료한다.

```sh
cd /home/aidl/work/jetson-smart-fan-controller
make all && make test
```

C 테스트6개 PASS와 Python `Ran 25 tests / OK`를 확인한다. 이후:

```sh
if [ -d /sys/module/smartfan ]; then
    sudo rmmod smartfan
fi
sudo insmod driver/smartfan.ko
ls -l /dev/smartfan
./build/bmp180-monitor --once
```

rmmod가 busy이면 기존 fanctl을 먼저 종료한다. insmod가 이미 적재됐다는 오류를
내면 기존 세션 종료/재적재를 완료한다. --once에서 온도와 기압 한 줄이 나오고
종료되면 다음으로 진행한다. 통합 실행 중 별도 sensor reader를 켜지 않는다.

## 2. 통합 실행

```sh
sudo ./build/fanctl --bmp180 --lcd --lcd-address 0x27
```

이전에 표시됐던 LCD 주소가 다르면 그 주소를 사용한다. `SENSOR_PROCESS pid=...`를
기록한다. `SENSOR temperature=... pressure=...`가 출력된 후 아래 시험을 한다.
이후 명령은 shell이 아니라 **fanctl 안에 한 줄씩 입력하고 결과를 확인**한다.

## 3. MANUAL / LED / LCD

| 입력 순서 | 확인할 결과 |
|---|---|
| `status` | OFF / MANUAL, 온도·기압 정상 |
| `led 1` | 첫 칸만 점등, 모터 정지 |
| `led 8` | 8칸 점등, 모터 정지 |
| `led 0` | 모두 소등 |
| `speed 1` | 선택 단계1, 모터 정지 |
| `on` | 모터 회전, LED2칸, LCD ON / S:1/5 |
| `speed 2` → `speed 3` → `speed 4` → `speed 5` | LED4/5/7/8칸, LCD 단계 반영, 모터 구동 변화 |
| `off` | 모터 정지, LED 소등, LCD OFF |

30초가 지나 자동 정지했다면 다음 구동 시험에는 on을 다시 입력한다.
PWM 단계와 실제 RPM은 같지 않으며, 이 시험은 정량 RPM 교정이 아니다.

## 4. AUTO / 명시적 OFF / 최대ON

| 입력 / 동작 | 확인할 결과 |
|---|---|
| `mode auto` | 모터 OFF, armed=0, 온도에 맞는 target 선택 |
| `on` | armed=1; 24°C 이상이면 회전, 그 미만이면 대기 |
| `status` | AUTO / 온도 / 목표 단계와 실제 상태 확인 |
| `off` 후 수초 대기 | 온도가 높아도 OFF 유지, armed=0 |
| `on` 후 30초 이상 관찰 | 연속 구동 중 온도가 유지되면 reason=max_on으로 정지하고 자동 재시작하지 않음 |
| 새 정상 SENSOR 출력 확인 후 `on` | 명시적인 재시작 가능 |
| `off` | 다음 시험 전 정지 |

처음 선택 시 온도별 단계는 다음과 같다. 냉각 시에는 각 경계보다1°C 낮아져야
단계를 내린다. 예:2단계는25.0°C까지 유지하고25.0°C 미만에서1단계로 내려간다.

| 온도 | 단계 | 구동 중 LED |
|---|---|---|
| 24°C 미만 | 0 | 0칸 |
| 24~26°C 미만 | 1 | 2칸 |
| 26~28°C 미만 | 2 | 4칸 |
| 28~30°C 미만 | 3 | 5칸 |
| 30~32°C 미만 | 4 | 7칸 |
| 32°C 이상 | 5 | 8칸 |

현재 온도 때문에 시험할 수 없는 구간은 make test로 정책을 검사한 상태다.
실제 모든 구간을 확인했다고 기록하지 않는다. 목표0으로 냉각된 정지는 arm을
유지하므로 이후 온도가 상승하면 다시 구동될 수 있다.

## 5. 배선 변경 없이 센서 만료 / 복구 시험

AUTO에서 정상 sample 확인 후 on을 입력한다. 별도 SSH 터미널을 열고
위에서 기록한 **sensor child PID 숫자**를 넣는다. 12345는 실제 PID로 바꾼다.

```sh
sudo kill -STOP 12345
```

마지막 sample 시각으로부터 약3초 뒤 `AUTO STOP reason=sensor_fault_or_stale`가
나오고 모터 정지/LED 소등/LCD T:ERR를 확인한다. status에는 armed=0이어야 한다.

```sh
sudo kill -CONT 12345
```

새 SENSOR 값과 LCD 온도 표시가 돌아와도 모터는 정지 상태를 유지해야 한다.
fanctl에서 on을 입력해야 재시작한다. 시험 후 off를 입력한다.
이 시험은 sample 공급 중단 시험이며 실제 NACK/전기적 bus 고장 시험은 아니다.
실제 I²C 오류로 child가 종료됐으면 연결 복구 뒤 fanctl을 다시 실행한다.

## 6. 종료 확인

on으로 구동 중 Ctrl+C를 누른다. 모터/LED가 꺼지고 shell로 복귀하는지 확인한다.

```sh
pgrep -a -x fanctl
pgrep -a -x bmp180-monitor
```

별도 인스턴스를 띄우지 않았다면 둘 다 출력이 없어야 한다. LCD는 최종 OFF 표시를
시도한다. LCD 통신이 막힌 경우 join은2초 뒤 경고로 반환하지만, 실제 kernel I²C
작업 종료 시점과 프로세스 소멸까지의 절대 상한을 보장하는 것은 아니다.
모터 정지는 LCD worker 정리보다 먼저 요청한다.

실제 system suspend 시험은 SSH가 끊길 수 있어 이번 필수 절차에서 제외했다.
PM 경로는 소스 검토와 경쟁 조건 mock을 통과했으며 실물 절전 검증은 별도로 남는다.

## 합격 기록

- [ ] MANUAL ON/OFF·단계 변경, LED1/8/0, LCD 상태·온도 표시
- [ ] AUTO 선택/ON, 명시적 OFF 뒤 정지 유지
- [ ] 온도가 유지되는 연속 구동에서30초 제한 후 정지 유지
- [ ] sensor STOP 후 stale 정지, CONT 후 자동 재시작 없음
- [ ] Ctrl+C 후 모터/LED 정지와 sensor child 종료

이 항목을 확인하면 로터리를 제외한 이번 수정본의 기본 통합 시험을 마친 것이다.
LCD 대비 개선과 장기 내구성/정량 성능 측정은 별도다.
