# 발표·포트폴리오에 사용할 설명

다음은 실제 구현에 근거한 소개 문안과 발표 흐름입니다. PPT 파일을 제작한 것은 아닙니다.

## 프로젝트 소개 문안

> Jetson Orin Nano에서 Linux Kernel Driver와 C 사용자 프로그램을 이용한 스마트 선풍기 제어 시스템을 구현했습니다. Descriptor 기반 GPIO/PWM consumer와 ioctl 인터페이스를 설계하고, EC11 입력으로 수동 풍속을 조절하며 BMP180 온도에 따른 자동 정책을 적용했습니다. Sensor process의32-byte pipe IPC와 LCD worker의 snapshot 동기화로 blocking I²C를 제어 루프에서 분리했습니다. Kernel heartbeat lease와 연속 구동 제한, close/오류 정지 및 명시적 재시작 정책을 구현하고 mock·PTY·sanitizer 검증과 사용자 실물 확인을 구분해 기록했습니다.

## 기술적으로 설명할 기여

| 주제            | 설명할 구현 / 판단                                                                        |
| --------------- | ----------------------------------------------------------------------------------------- |
| Linux driver    | platform binding, devm GPIO/PWM, miscdevice, fixed-width UAPI, kref/removed gate          |
| 동시성          | kernel mutex/workqueue, LCD mutex/condition, signal self-pipe, fork 전후 signal mask      |
| 제어 정책       | MANUAL/AUTO 책임 분리, hysteresis, protected-stop latch, BOOTTIME freshness               |
| IPC             | partial stream read, framing/sequence/error validation, exec error frame, bounded cleanup |
| 하드웨어 디버깅 | pinmux readback, raw IRQ/level 비교, 고정LOW/HIGH 시험, NPN 입력 보강                     |
| 검증            | C/mock tests, Python CLI/PTY tests, ASan/UBSan, GCC analyzer, clean 데이터 보존 테스트    |

## 발표 흐름 예시

1. **문제와 목표:** 입력·센서·출력을 통합하고, 애플리케이션이 멈췄을 때도 정지 정책을 지키기.
2. **전체 구조:** architecture diagram과 프로세스·스레드별 자원 소유권.
3. **Kernel:** EN 우선 차단, lease/max-on/boost, 같은 mutex 안에서 처리하는 AUTO 갱신.
4. **Userspace:** poll과 Gray 해석, 센서 IPC, LCD snapshot coalescing.
5. **문제 해결:** pinmux와 EC11 입력 보강, GET→ON race, stdin 장애의 원인 분리.
6. **시연:** MANUAL ON → 로터리/PWM/LED/LCD → OFF → AUTO → 센서 stale 정지 → 복구 후 정지 유지.
7. **검증과 한계:** 테스트 표, 실물 확인 범위, 전원·LCD·PM·정량 성능의 남은 항목.

## 과장하지 않을 표현

- PWM duty 단계는 RPM을 측정한 결과가 아닙니다. Closed-loop RPM control도 없습니다.
- BMP180은 습도 센서가 아닙니다. 정밀 온도계·기압계와 교정 대조하지 않았습니다.
- Worker와 lease는 hard real-time 또는 모든 하드웨어 고장에서의 절대 정지를 보장하지 않습니다.
- Mock과 kernel build는 실제 suspend·전압 파형·전원 정격 검증을 대신하지 않습니다.
- Sensor fault 후 자동 process 재생성, Qt GUI, network remote control, watchdog framework는 구현하지 않았습니다.
- LCD는 문자 표시를 확인했지만 3.3V에서 대비 문제가 남아 있습니다. 모터 전원은 현재 Jetson 5V이며 전류 여유를 측정하지 않았습니다.

사진·동영상은 사용자가 확인한 정상 배선과 동작으로 추가하는 것이 좋습니다. 현재 로컬 사진·PDF는 Gitignore로 제외돼 있으며, 이 소개 문서는 해당 파일들이 공개 저장소에 존재한다고 가정하지 않습니다. 시연 증거와 fail-stop 시험 로그를 추가하면 소개 문안을 더 강하게 뒷받침할 수 있습니다.
