# 최종 통합 테스트 순서 — 로터리 포함

기존에 회전/점등/표시를 확인한 배선을 유지한다. LCD 전체3.3V 공급도 유지한다.
엔코더는S1/S2 각각NPN회로를사용한다: EC11→B=10kΩ, C→3.3V=1kΩ,
E→공통GND, C→Jetson 물리12/38. 전압검토는 [NPN 회로](encoder-npn-review.md)를 따른다.
**추가 DT 설치, GPIO/PWM export, 재부팅은 필요 없다.** 이번debounce 변경은
userspace이므로그변경만을위한module재적재는필요없다. 아래최종시험에서는
기존AUTO보완도포함한현재module과실행파일을맞추기위해재적재한다.

## 1. 기존 실행 종료 → 빌드 → 새 module 적재

현재 실행 중인 fanctl 터미널에서 `off`, `quit`를 차례로 입력한다.
별도 bmp180-monitor가 있으면 Ctrl+C로 종료한다.

```sh
cd /home/aidl/work/jetson-smart-fan-controller
make all && make test
```

C 테스트8개 PASS와 Python `Ran 28 tests / OK`를 확인한다. 이후:

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

## 2. 엔코더 단독 최종 확인

```sh
sudo ./build/encoder-monitor 2>&1 | tee -i build/encoder-final.log
```

시작출력의debounce_us=0을확인한다. 별도의--no-debounce는이제필요없다.
정지5초 → 한방향천천히10딸깍 → 반대방향10딸깍 → 정지5초 → Ctrl+C.
정지중STEP이없고양방향cw/ccw가증가하며sequence_gaps/invalid가반복해서
늘지않는지확인한다. Gray cycle4전이를1STEP으로유지하므로EC11변형에따라
두딸깍이1STEP일수있다. --reverse로방향표기를바꿀수있다.
tee -i는Ctrl+C때monitor의CLOSED통계까지저장하기위해사용한다.
이프로그램을종료해야fanctl이같은GPIO를요청할수있다.

## 3. 통합 실행

```sh
sudo ./build/fanctl --encoder --bmp180 --lcd --lcd-address 0x27
```

이전에 표시됐던 LCD 주소가 다르면 그 주소를 사용한다. `SENSOR_PROCESS pid=...`를
기록한다. `SENSOR temperature=... pressure=...`가 출력된 후 아래 시험을 한다.
이후 명령은 shell이 아니라 **fanctl 안에 한 줄씩 입력하고 결과를 확인**한다.
ENCODER줄의debounce_us=0과S1=50/S2=52를확인한다(물리핀번호12/38과구분).
정상BMP주기측정은터미널에매번출력하지않는다. 첫성공/오류/복구/만료는알리고,
현재온도·기압은status명령에서확인한다. LCD/측정/AUTO는계속동작한다.
입력명령은CMD on처럼출력되므로기동명령수신과STATE ON을구분해확인한다.
INPUT줄에tty/canonical/icrnl설정을출력한다. CR/LF/CRLF줄끝을모두처리하며,
CRLF명령은중복실행하지않는다. 원시PTY에서Enter가CR인경우도회귀검증했다.
실제sudo+tee실행은INPUT canonical=1/icrnl=1에서도명령미수신보고가있었다.
이는CR-only 재현과다른상황이다. 파이프없는직접실행에서명령입력복구와
로터리통합동작을사용자가확인했다. interactive fanctl은위와같이직접실행한다.
sudo/TTY 입력중계의내부원인은확정하지않았다.
방향이반대라면off/quit로종료한뒤같은명령에--reverse-encoder를추가해재실행한다.

## 4. MANUAL / LED / LCD / 로터리

| 입력 순서                                     | 확인할 결과                                 |
| --------------------------------------------- | ------------------------------------------- |
| `status`                                      | OFF / MANUAL, 온도·기압 정상                |
| `led 1`                                       | 첫 칸만 점등, 모터 정지                     |
| `led 8`                                       | 8칸 점등, 모터 정지                         |
| `led 0`                                       | 모두 소등                                   |
| `speed 1`                                     | 선택 단계1, 모터 정지                       |
| `on`                                          | 모터 회전, LED2칸, LCD ON / S:1/5           |
| `speed 2` → `speed 3` → `speed 4` → `speed 5` | LED4/5/7/8칸, LCD 단계 반영, 모터 구동 변화 |
| `off`                                         | 모터 정지, LED 소등, LCD OFF                |
| `speed 2` 후 로터리 양방향 회전               | 모터OFF를유지하면서선택풍속0~5변경, LCD반영 |
| 계속올리기/내리기                             | 상한5/하한0을넘지않음                       |
| `speed 2` → `on` 후 로터리 회전               | PWM단계·LED칸수·LCD선택풍속이함께변경       |
| 로터리로0단계까지내리기                       | 모터/LED정지                                |
| 정지뒤로터리로1이상선택                       | 선택풍속만변경, 모터는정지유지              |
| `on` → `off`                                  | 명시적인ON에서만재시작하고OFF에서정지       |

30초가 지나 자동 정지했다면 다음 구동 시험에는 on을 다시 입력한다.
PWM 단계와 실제 RPM은 같지 않으며, 이 시험은 정량 RPM 교정이 아니다.

## 5. AUTO / 로터리 충돌 / 명시적 OFF / 최대ON

| 입력 / 동작                      | 확인할 결과                                                                  |
| -------------------------------- | ---------------------------------------------------------------------------- |
| `mode auto`                      | 모터 OFF, armed=0, 온도에 맞는 target 선택                                   |
| `on`                             | armed=1; 24°C 이상이면 회전, 그 미만이면 대기                                |
| `status`                         | AUTO / 온도 / 목표 단계와 실제 상태 확인                                     |
| 로터리양방향회전                 | 같은온도구간에서는목표풍속을덮어쓰지않음; encoder는AUTO에서무시됨            |
| `off` 후 수초 대기               | 온도가 높아도 OFF 유지, armed=0                                              |
| `on` 후 30초 이상 관찰           | 연속 구동 중 온도가 유지되면 reason=max_on으로 정지하고 자동 재시작하지 않음 |
| 새 정상 SENSOR 출력 확인 후 `on` | 명시적인 재시작 가능                                                         |
| `off`                            | 다음 시험 전 정지                                                            |

처음 선택 시 온도별 단계는 다음과 같다. 냉각 시에는 각 경계보다1°C 낮아져야
단계를 내린다. 예:2단계는25.0°C까지 유지하고25.0°C 미만에서1단계로 내려간다.

| 온도         | 단계 | 구동 중 LED |
| ------------ | ---- | ----------- |
| 24°C 미만    | 0    | 0칸         |
| 24~26°C 미만 | 1    | 2칸         |
| 26~28°C 미만 | 2    | 4칸         |
| 28~30°C 미만 | 3    | 5칸         |
| 30~32°C 미만 | 4    | 7칸         |
| 32°C 이상    | 5    | 8칸         |

현재 온도 때문에 시험할 수 없는 구간은 make test로 정책을 검사한 상태다.
실제 모든 구간을 확인했다고 기록하지 않는다. 목표0으로 냉각된 정지는 arm을
유지하므로 이후 온도가 상승하면 다시 구동될 수 있다.

## 6. 배선 변경 없이 센서 만료 / 복구 시험

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

## 7. controller 멈춤 / lease 정지

센서복구뒤정상sample을확인하고AUTO에서on을입력한다. 별도터미널에서
`pgrep -a -x fanctl`로실제통합controller PID를찾는다. **sensor PID가아니다.**
아래12345를그PID로바꾼다.

```sh
sudo kill -STOP 12345
```

약2초lease만료후모터/LED가꺼져야한다. 2.5초이상지난뒤:

```sh
sudo kill -CONT 12345
```

프로그램이복귀해도모터가자동재시작하지않아야한다. 상태출력에서lease정지와
AUTO disarm을확인한다. LCD는STOP동안갱신되지않으므로실제모터를관찰한다.
새정상SENSOR값을확인한뒤on으로재시작하고off로정지한다.

## 8. 종료 확인

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

- [ ] 엔코더정지중가짜STEP없음, 양방향회전과종료통계확인
- [ ] MANUAL ON/OFF·단계 변경, LED1/8/0, LCD 상태·온도 표시
- [ ] 로터리풍속0~5상하한, 0정지, 정지뒤회전만으로재시작하지않음
- [ ] AUTO 선택/ON, 명시적 OFF 뒤 정지 유지
- [ ] AUTO에서로터리가온도목표를덮어쓰지않음
- [ ] 온도가 유지되는 연속 구동에서30초 제한 후 정지 유지
- [ ] sensor STOP 후 stale 정지, CONT 후 자동 재시작 없음
- [ ] Ctrl+C 후 모터/LED 정지와 sensor child 종료
- [ ] controller STOP동안lease정지, CONT뒤자동재시작없음

이 항목을 확인하면 로터리를 포함한 이번 수정본의 기본 통합 시험을 마친 것이다.
LCD 대비 개선과 장기 내구성/정량 성능 측정은 별도다.
