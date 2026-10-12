# 현재 구현·검증 상태

2026-10-12, 1~7단계 기준입니다. 과거 세부 기록은 Git 추적에서 제외한 로컬 보관 자료이며, 주요 개발 과정은 [development](development.md)에 정리했습니다.

| 항목             | 상태                       | 증거 / 남은 내용                                                                               |
| ---------------- | -------------------------- | ---------------------------------------------------------------------------------------------- |
| Motor ON/OFF/PWM | 구현·기본 실물 확인        | 사용자 회전 보고, 단계별 RPM/전류/발열 미측정                                                  |
| EC11             | 구현·통합 실물 확인        | NPN(Rb10kΩ/Rc1kΩ), 기본 기간0, 사용자 입력 성공                                                |
| LED Bar          | 구현·실물 확인             | 8칸 점등 보고, 풍속 mapping 회귀                                                               |
| LCD              | 구현·문자 표시 확인        | 3.3V 대비 부족·흐려짐 미해결                                                                   |
| BMP180           | 구현·실측/IPC 확인         | ID0x55, Bosch 기준값, 실제 온도·기압 read, 정밀 교정 미수행                                    |
| AUTO             | 구현·정지/복구 보고        | 회귀 tests, 사용자 구체적 fault 주입 절차는 미기록                                             |
| 30초 최대ON      | 구현·사용자 확인           | 반복 ON·풍속 변경 deadline 회귀 포함                                                           |
| Lease            | 구현·사용자 확인           | controller 중단 후 정지 확인 보고, 실제 PM 시험 미수행                                         |
| Ctrl+C 종료      | 구현·사용자 확인           | fd/process/thread 정리, LCD stuck mock                                                         |
| Process/IPC      | 구현·검증                  | production monitor + I²C stub의 정상 fork/exec/IPC, AUTO 일관성, parent death, SIGKILL/waitpid |
| 전원 설계        | 기능 동작 확인·정량 미완료 | Motor Vs=Jetson5V 핀, 최대·기동·정지 전류와 전체 부하 전압 여유 미측정                         |

현재 운영 명령:

```sh
sudo ./build/fanctl --encoder --bmp180 --lcd --lcd-address 0x27
```

파이프 없는 실행의 명령 입력과 로터리 동작을 확인했습니다. 7단계에서는 선택형 로그 worker와 동시성 검증을 추가했습니다. 이번 개선과 최종 판정은 [quality-review.md](quality-review.md), 재현 검증은 [build-run.md](build-run.md)와 [final-test.md](final-test.md)를 따릅니다.

6단계의 구현·검증 보강과 아직 수행하지 않은 실물 프로세스 종료 시험은 [Process/IPC](stage6-process-ipc.md)를 기준으로 합니다.

7단계의 thread 소유권·로그 옵션·검증 한계와 실물 확인은 [stage7-threads](stage7-threads.md)를 따릅니다.
