# 1단계 종료 확인

기본 GPIO motor 회전은 사용자 보고로 확인했다. 추가 기능 구현은 필요 없지만 아래 runtime 확인이 남아 있다.
시험 결과는 실제 관찰 후 기록한다. 현재 CLI software9 tests 통과를 motor safety 시험으로 대신하지 않는다.

| 시험 | 사용자 작업 | 정상 결과 |
|---|---|---|
| 반복 ON/OFF | CLI에서 on→off를3회 | 매번 회전/구동 중단 |
| 정상 종료 | on으로 회전 중 quit | 앱 종료 후 구동 중단 |
| 비정상 종료 | 다시 CLI를 실행해 on, 다른 terminal에서 `sudo fuser -k /dev/smartfan` | 종료된 controller의 마지막 fd가 닫히며 구동 중단 |
| Module unload/reload | 모든 CLI 종료 후 아래 rmmod/insmod | 오류 없이 재적재, motor 초기 OFF |

```sh
cd /home/aidl/work/jetson-smart-fan-controller
sudo rmmod smartfan
sudo insmod driver/smartfan.ko
```

fuser는 `/dev/smartfan`을 실제 연 process를 대상으로 한다. 다른 임의 PID를 지정하지 않는다.
강제 종료 시험은 실제 motor가 회전하는 동안 수행한다. 최대 ON30초로 이미 멈춘 상태라면 종료 정지 검증이 아니다.
정지는 전기적 구동 중단이며 blade는 관성으로 잠시 돌 수 있다.
열린 fd가 있으면 rmmod가 거부되는 것이 정상이다. 임의로 강제 unload하지 않는다.

추가 safety 회귀 항목: heartbeat가 중단된 경우 kernel lease2초 만료 정지, 최대 ON30초 정지,
SSH 단절에서 process 생존/종료 및 정지시간. 이것들은 아직 physical 검증 결과가 없다.
Kernel hang/전원 문제를 software lease로 보장하지 않는다.

이 확인 뒤2단계 Rotary Encoder/PWM를 구현한다. PWM 추가 시GPIO/PWM 중복 점유 및 M7 diode 적합성은 별도 검토한다.
