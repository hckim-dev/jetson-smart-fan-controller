# EC11 소프트웨어 안정 시간 필터 시험

2026-10-12. 새 모드는 encoder-monitor에만 적용하며 모터/LED를 제어하지 않는다.
GPIO 입력을 주기적으로 읽고 A/B 조합이 stable-us 동안 관찰상 유지되면 기존
4전이 Gray decoder에 전달한다. IRQ 이벤트를 요청하지 않아 event FIFO에 의존하지 않는다.
하드웨어 debounce 기간은0으로 요청한다. Tegra의 debounce-enable 비트를
끄는 모드는 아니며, 기간0의 controller 동작까지 우회한다고 주장하지 않는다.

## 지금 실행

기존 encoder-monitor/diagnose/fanctl --encoder를 종료한다. 모터는 OFF로 둔다.
빌드는 완료했으며 추가 DT 설치/module 재적재/reboot는 필요 없다.

```sh
cd /home/aidl/work/jetson-smart-fan-controller
sudo ./build/encoder-monitor --stable-us 300 --sample-us 50 --seconds 30 | tee build/encoder-soft-300.log
```

실행마다30초 뒤 자동 종료된다. 첫5초는 손을 떼고, 화면의 phase를 따른다.

| phase             | 동작                             | 볼 값                       |
| ----------------- | -------------------------------- | --------------------------- |
| IDLE_START, 0~5초 | 손을 떼고 정지                   | cw/ccw가 증가하지 않아야 함 |
| CW, 5~15초        | 한 방향으로10딸깍, 약1초에 한 칸 | 한쪽 카운트가 증가해야 함   |
| CCW, 15~25초      | 반대 방향으로10딸깍              | 반대 카운트가 증가해야 함   |
| IDLE_END, 25~30초 | 손을 떼고 정지                   | cw/ccw가 다시 멈춰야 함     |

모듈 장착 방향에 따라 CW/CCW 이름이 반대일 수 있다. 두 방향을 구분하는지 먼저 본다.
4전이 decoder이므로 모델에 따라10딸깍이5 STEP일 수 있다. 이번에는2전이로 변경하지 않는다.

첫 결과가 여전히 폭주하거나 회전을 놓치면 아래 두 값을 같은 방식으로 비교한다.
각 명령은 이전 시험이 자동 종료된 뒤 실행한다.

```sh
sudo ./build/encoder-monitor --stable-us 1000 --sample-us 50 --seconds 30 | tee build/encoder-soft-1000.log
sudo ./build/encoder-monitor --stable-us 2000 --sample-us 50 --seconds 30 | tee build/encoder-soft-2000.log
```

stable-us는 사용자 공간의 실제 ns timestamp 차이로 계산하므로300us가 하드웨어처럼
1ms로 올림되지는 않는다. Linux userspace가50us 주기로 정확하게 실행된다는 뜻은 아니다.
--stable-us는 --debounce-us/--no-debounce/--poll-levels/--raw와 함께 쓰지 않는다.
--sample-us와 --seconds는 이 모드에서만 사용한다.

## 출력 해석

매초 FILTER 한 줄과 최종 CLOSED FILTER 통계를 출력한다. 잡음마다 출력하지 않는다.

| 항목                        | 의미                                                                       |
| --------------------------- | -------------------------------------------------------------------------- |
| raw_changes                 | 샘플링한 A/B 조합의 변화 횟수, IRQ edge 수와 다름                          |
| accepted                    | 안정 시간 필터를 통과한 조합 변화 횟수, 회전 STEP 수와 다름                |
| cw / ccw                    | 완성된4전이 회전 카운트                                                    |
| invalid                     | 두 비트 동시 변화가 decoder에 들어와 부분 회전을 버린 횟수                 |
| gap_resets                  | 실제 sample 간격이 안정 시간보다 길거나 timestamp가 역행해 재동기화한 횟수 |
| ready                       | 최초 또는 gap 이후 안정된 시작 상태를 확보했는지                           |
| mean_sample_us / max_gap_us | 실제 평균 샘플 간격과 최대 간격                                            |

gap 뒤에는 관찰하지 못한 시간도 안정 시간으로 계산하지 않는다. 후보 상태와
부분 회전을 초기화하고 안정된 상태부터 다시 시작하므로 일부 실제 회전을 잃을 수 있다.
ready=0이 계속되거나 gap_resets가 많을 때 카운트0을 잡음 제거 성공으로 보지 않는다.

**합격:** 정지 구간에서 STEP 증가 없음 + 양방향 회전 인식 + 같은 동작의 카운트가
반복 가능한 비율. 출력이 조용하거나 accepted=0인 것만으로 성공이 아니다.
짧은 펄스는 sampling 사이에서 누락될 수 있다. 잡음이 정상 Gray cycle과 같으면
이 필터도 실제 회전과 구분할 수 없다. 실물 합격 전 fanctl에는 적용하지 않는다.

## 소프트웨어 검증

Build, C7개/CLI25회귀, 필터 ASan/UBSan 및 변경 소스의 GCC analyzer를 통과했다.
Mock GPIO/clock으로300/1000/2000us에서 잡음 뒤CW/CCW 각각1회 집계와
샘플 gap 처리, 잘못된/충돌 옵션14건 거부를 확인했다. 실물 GPIO는 실행하지 않았다.

필터 단위 테스트로 짧은 잡음, 안정된 CW/CCW, 대각 전이, startup/reseed,
샘플 지연과 역행 timestamp를 검사한다. 하드웨어 출력은 실행하지 않는다.

```sh
make build/encoder-monitor build/test_encoder_filter
./build/test_encoder_filter
```

실제 시험 로그는 build/encoder-soft-\*.log로 남긴다. 이 문서는 실물 성공 기록이 아니다.

## 300us 실제 결과와 다음 분리 시험

사용자30초 로그는 raw_changes=19319, accepted=19143, invalid=6260,
cw=0/ccw=0, gap_resets=70이었다. 실제 평균sample 간격113.3us, 최대2268.7us.
처음5초 정지에서도3203개 전이가 통과하고 invalid1074회가 발생했다.
300us 필터로 입력 불안정이 제거되지 않았다. 유효한 회전 순서를 얻지 못했으며,
전기적 원인 또는 controller의 기간0 동작 중 어느 쪽인지 아직 확정하지 않았다.
invalid는 두 비트 동시 변화의 관찰이며 실제 동시 전이와 sampling 누락을 구분하지 않는다.

다음 시험은 encoder 신호를 분리하고 Jetson 입력을 확실한LOW로 고정한다.

1. 모든 encoder 프로그램을 종료하고 모터OFF 후 Jetson 전원을 끈다.
2. 물리12번에서 엔코더S1 선, 물리38번에서 엔코더S2 선을 제거한다.
3. 엔코더 출력이 분리된 **Jetson 물리12번과38번을 각각 공통GND에 연결**한다.
   두 핀은 조사된 GPIO 입력이다. 엔코더 S1/S2 단자 자체는 GND에 연결하지 않는다.
4. Jetson 전원을 켜고 다음 입력 전용 진단을 실행한다.

```sh
cd /home/aidl/work/jetson-smart-fan-controller
if [ ! -d /sys/module/smartfan ]; then
    sudo insmod driver/smartfan.ko
fi
sudo ./build/encoder-monitor --stable-us 300 --sample-us 50 --seconds 10 | tee build/encoder-ground.log
```

smartfan module은 기존 DT의 encoder 입력 pinctrl 적용을 위해 적재하며 모터ON은
요청하지 않는다. insmod 오류가 있으면 그 결과를 먼저 기록하고 시험을 중단한다.

정상 고정 입력이라면 A=0/B=0, raw_changes=0, accepted=0, cw=0/ccw=0이어야 한다.
gap_resets는 sample 지연 때문에 증가할 수 있으므로0을 필수 조건으로 두지 않는다.
이 결과가 안정적이면 엔코더/신호선/translator를 포함한 연결 상태에 집중한다.
불안정하면 실제header LOW 전압과 GPIO mapping/controller/filter 동작을 조사한다.
GND 시험의 성공만으로 TXB0108이나 엔코더 고장을 확정하지 않는다.
시험 뒤에는 Jetson 전원을 끄고 두 GND 시험선을 제거한 뒤 기존 S1/S2를 복구한다.

### 고정 LOW 실제 결과

encoder-ground.log: 10초간samples88806, raw_changes=0, accepted=0,
cw=0/ccw=0, invalid=0, A/B=0/0을 확인했다. gap_resets=51은sampling 지연이며
입력 변화나 가짜 회전을 만들지 않았다. 평균sample 간격112.6us, 최대1821.6us.
입력LOW가고정된조건에서sample/decoder는안정적이다. HIGH 조건과encoder의
실제출력구동은아직검증하지않았으며모든driver/controller동작의정상증명은아니다.

다음은 고정HIGH 비교다. Jetson 전원을 끄고 **12/38번의GND시험선을모두제거**한
뒤, 엔코더S1/S2분리를유지하며 **Jetson 물리12/38을각각확인된3.3V rail에연결**한다.
5V rail은사용하지않는다. 전원을켠뒤기존smartfan module을적재해input pinctrl을
유지하고아래를실행한다. Module적재오류가있으면진행하지않는다.

```sh
cd /home/aidl/work/jetson-smart-fan-controller
if [ ! -d /sys/module/smartfan ]; then
    sudo insmod driver/smartfan.ko
fi
sudo ./build/encoder-monitor --stable-us 300 --sample-us 50 --seconds 10 | tee build/encoder-high.log
```

기대값은A=1/B=1, raw_changes=0, accepted=0, cw=0/ccw=0이다. 고정LOW/HIGH
둘다안정적이면encoder가연결된신호경로의조건에집중한다. 이것만으로개별부품
고장을확정하지않는다. 복구전에는전원을끄고3.3V시험선을제거한다.

### 고정 HIGH 실제 결과 / 판단

encoder-high.log: 10초간samples87993, raw_changes/accepted/invalid/CW/CCW모두0,
A/B=1/1을확인했다. gap_resets=41, 평균sample간격113.6us, 최대12591.8us였다.
LOW/HIGH 양쪽고정입력시험을통과했으므로강하게고정된입력에서software가
신호변화/회전을스스로만드는문제는관찰되지않았다. 임의로필터값을더조정하거나
4전이를2전이로바꾸기보다encoder연결시신호경로/부하/출력구동에집중한다.
이시험만으로동적IRQ동작전체,교차채널독립성또는개별부품고장을증명하지않는다.
단순NPN+10k pull-up을해결회로로채택하지않는다. 보유NPN/PNP부품명확인후
양쪽레벨을능동구동하는회로가능성을검토하거나검증된logic buffer를사용한다.
