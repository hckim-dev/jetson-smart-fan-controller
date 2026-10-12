# 통합 테스트

**정상 배선·전원에서 실제 동작을 다시 확인하는 체크리스트입니다.** [실행](build-run.md) 후 명령은 fanctl 안에 입력합니다. 이 문서는 성공 기록이 아니며 확인한 항목만 완료로 표시합니다.

모듈 적재와 배선 준비 후, 저장소 root에서 아래처럼 **로그 옵션을 포함해 실행**하면 표의 전체 항목을 확인할 수 있습니다.

```sh
sudo ./build/fanctl --encoder --bmp180 --lcd --lcd-address 0x27 --log build/fan-events.log
```

| 순서 | 조작                                   | 기대 결과                                                |
| ---- | -------------------------------------- | -------------------------------------------------------- |
| 1    | `status`                               | MANUAL/OFF, 유효 센서값                                  |
| 2    | `led 1` → `led 8` → `led 0`            | 모터 OFF에서 해당 칸 점등/소등                           |
| 3    | `speed 5` → `on` → 엔코더 양방향       | 회전, 풍속·LED·LCD 연동                                  |
| 4    | 단계 0 → 다시 양의 단계 선택           | 정지 유지; 명시적 `on`만 기동                            |
| 5    | `mode auto` → `on`                     | 온도별 목표 선택; AUTO에서 로터리는 목표를 덮어쓰지 않음 |
| 6    | 온도가 구동 구간인 상태로 30초         | max_on 정지; 반복 ON/풍속 변경으로 제한 연장 안 됨       |
| 7    | `stats`, `monitor 1000`, `monitor off` | 통계 확인, 선택형 출력 시작/중단                         |
| 8    | `quit` 또는 Ctrl+C                     | 모터/LED 정지, 자식 회수, LOG_CLOSED의 오류/누락 확인    |

AUTO 초기 경계는 24/26/28/30/32°C, 하강 hysteresis는 1°C입니다. 저온 target0 대기는 정상이며 모든 구간을 실물로 확인하지 않았다면 정책 회귀와 구분합니다. EC11 변형에 따라 두 딸깍당 한 step일 수 있어 방향·일정한 비율·정지 중 가짜 step 여부를 봅니다.

## 장애 시 정지·복구

프로그램을 종료했다면 다시 실행합니다. **각 STOP 시험 전에** 정상 센서값과 `mode auto` → `on` → `status`로 AUTO 구동·실제 회전을 확인합니다. 센서 복귀 시험 후에는 명시적 `on`으로 다시 구동한 뒤 controller lease 시험을 시작합니다. 저온 target0 대기는 모터 정지 시험의 구동 전제를 만족하지 않습니다.

별도 터미널에서 **실제 PID**를 확인합니다. Sensor PID는 시작의 SENSOR_PROCESS, controller PID는 `pgrep -a -x fanctl` 출력입니다.

| 대상            | 명령                                        | 확인                                                       |
| --------------- | ------------------------------------------- | ---------------------------------------------------------- |
| Sensor          | `sudo kill -STOP 실제센서PID`               | 마지막 sample 약 3초 뒤 AUTO OFF/disarm, LCD T:ERR         |
| Sensor 복귀     | `sudo kill -CONT 실제센서PID`               | 정상 sample 복귀만으로 재기동 안 됨; 명시적 ON 필요        |
| Controller      | `sudo kill -STOP 실제제어PID`               | 약 2초 lease 뒤 실제 모터 정지; LCD는 함께 멈춰 갱신 안 됨 |
| Controller 복귀 | 2.5초 이상 뒤 `sudo kill -CONT 실제제어PID` | AUTO 자동 재기동 안 됨                                     |

STOP 시험은 데이터/heartbeat 공급 중단이며 실제 I²C 전기 고장 시험은 아닙니다. Sensor를 `-TERM`으로 종료했다면 유효 데이터 없는 AUTO ON 거부를 확인하고 controller를 재실행합니다.

## 종료 확인

```sh
ps -o pid,ppid,stat,args -C fanctl -C bmp180-monitor
sudo tail -n 20 build/fan-events.log
```

별도 인스턴스가 없다면 process 잔류/STAT=Z가 없어야 합니다. 로그 옵션 사용 시 SESSION_STATS/STOP과 LOG_CLOSED를 확인합니다. LCD 최종 OFF는 best-effort이며 kernel I²C 대기의 절대 종료 시간을 보장하지 않습니다.

확인 날짜·명령·관찰 결과를 기록하세요. 전원 여유·LCD 대비·발열·실제 suspend는 [검증 한계](review.md)에 별도로 남겨둡니다.
