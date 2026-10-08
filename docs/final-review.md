# 최종 재검토 — 2026-10-08

로터리 실물 구현은 다음 주로 제외한다. BMP180/AUTO 통합과 후속 수정에 대해
코드, 빌드, 회귀 테스트, 현재 실행 환경을 다시 확인했다.
**발견한 결함은 수정했으며 소프트웨어 검증은 통과했다. 실제 AUTO 통합 시험은
[최종 테스트 순서](final-test.md)에 따라 확인해야 한다.**

## 수정 결과

| 항목 | 원인 / 처리 | 검증 |
|---|---|---|
| AUTO 정지 뒤 의도치 않은 재시작 | 상태 GET과 일반 ON 사이에 suspend/만료가 끼어들 수 있었다. 새 SET_AUTO가 같은 kernel mutex 안에서 재시작 차단 상태와 sample freshness를 검사한다. | 조회 사이 보호 정지를 주입하는 mock, 만료 sample, 명시적 rearm, cold update/idle 검사 |
| 보호 정지 상태 유실 | 일반 OFF/풍속0이 표시 사유를 USER로 바꿔도 auto_inhibited는 유지한다. 명시적인 사용자 ON만 차단을 해제한다. | 소스 검토, AUTO mock 및 최대ON/센서 오류 CLI 회귀 |
| 구형 module과 새 CLI 혼용 | SMARTFAN_CAP_AUTO가 없으면 AUTO 모드를 거부한다. 일반 ON으로 대체하지 않는다. 기존 MANUAL ABI는 유지한다. | capability 분기/ABI 크기 확인, module build |
| LCD 종료 대기 | 무제한 join을 CLOCK_MONOTONIC 기준 2초 join으로 변경했다. 실패 시 worker 객체를 보존하고 호출자가 오류로 종료한다. motor fd는 먼저 닫는다. | I²C write를 막는 mock으로 timeout과 객체 수명 검사, ASan/UBSan |
| LCD control signal 소유 | worker는 SIGINT/SIGTERM/SIGHUP을 차단하고 main의 기존 mask를 복구한다. | worker mask/main mask 회귀 |
| sensor exec 실패 진단 | child가 exec errno를 IPC error frame으로 전달해 EPIPE만 보이던 문제를 보완했다. | 실제 fork/exec 실패의 ENOTDIR 전달 검사 |
| sensor partial frame EOF | 완성되지 않은 frame에서 종료하면 EPROTO로 처리한다. | 잘린 frame 회귀 추가 |

sample_boottime_ms는 절전 시간을 포함한다. Kernel은 PWM 설정 등 잠들 수 있는
작업 뒤, EN 활성화 직전에도 freshness를 다시 검사한다. 자동 갱신은 rearm=0,
사용자가 입력한 on만 rearm=1이다. 이 요청은 명시적인 재시작 허용이며,
실제 system suspend/resume을 통과한 실물 검증은 아직 수행하지 않았다.

이전 검토의 Kbuild 문법 오류는 재검토 시작 시 이미 정상 두 줄로 복구돼 있었다.
현재 Kbuild로 module/W=1 build를 다시 통과했다. 이전 build/review_suspend_race
바이너리는 수정 전 재현 artifact이며 최신 검증 명령은 make test다.

## 다시 검토한 범위

- Kernel: GPIO/PWM 단일 소유, EN 우선 차단, mutex와 delayed work, boost/lease/
  최대ON 만료, sleeping PWM 호출 뒤 재검사, open/release/remove와 kref/devm 수명.
- Userspace: AUTO arm/disarm 및 hysteresis, 명령 파싱/부분 입력, heartbeat,
  signal self-pipe, 종료 순서와 구형 driver 호환성.
- BMP180: calibration endianness/signedness, 64-bit 보정식과 범위 검사,
  repeated-start read, ID 검사, conversion 완료 확인, cooperative reader lock.
- IPC: 고정 32-byte frame, partial read/EAGAIN/EOF, sequence/범위 검사,
  fork/exec/CLOEXEC, parent death signal, bounded child cleanup.
- LCD: worker의 fd 소유, snapshot mutex/condition, 변경 행만 갱신,
  I²C 부분 실패 재전송 금지, timeout 후 use-after-free 방지.
- DT: 기존 pinmux/PWM/LED/I²C 구성 및 boot fallback 보존 검사. 추가 설치는 하지 않았다.

I²C read는 combined transaction을 제공하는 I2C_RDWR를 사용한다.
[Linux 5.15 i2c-dev 문서](https://www.kernel.org/doc/html/v5.15/i2c/dev-interface.html)
PWM disable의 출력 레벨에 의존하지 않도록 모터 EN을 먼저 내린다.
[Linux 5.15 PWM 문서](https://www.kernel.org/doc/html/v5.15/driver-api/pwm.html)

기존 구조를 대체할 성능 병목은 확인되지 않았다. 센서 child와 LCD worker가
I²C blocking을 main heartbeat에서 분리하며, LCD는 변경된 행만 전송한다.
기존 dry-run 소규모 측정은 5.011초 동안 CPU 0.0058초, RSS 약9.4MiB였다.
이 수치는 실제 I²C/GPIO worst-case latency나 실물 부하 측정이 아니다.

## 이번 검증

- Jetson aarch64, L4T36.5.2, kernel5.15.199-tegra, GCC11.4.0, glibc2.35 확인.
- make all, make module W=1 성공. vermagic이 실행 kernel과 일치한다.
  GCC의 Ubuntu package revision만 kernel build compiler와 다르다는 경고가 있다.
- make test: C 테스트 프로그램6개 + CLI25 tests 통과. 실제 GPIO/I²C 출력 없음.
- BMP180 malformed calibration10,000건과 LCD timeout 회귀의 ASan/UBSan 통과.
- 변경한 userspace5개 translation unit의 GCC analyzer 통과.
- shell syntax, Python compile, git diff --check 통과.
- offline DT 합성/installer --check 통과. live I²C clock도100000 확인.
  installer의 일반적인 reboot 안내와 별개로 현재 LCD DT는 이미 적용돼 있다.
- 실행 중인 기존 fanctl --bmp180 --lcd 및 sensor child를 확인했다.
  해당 세션을 중단하거나 모터를 원격 기동하지 않았다. 새 module 재적재는 필요하다.

## 남은 실물 확인

모터/LED Bar 성공, LCD 문자 표시, BMP180 실측/IPC는 이전 단계에서 확인했다.
이번 수정본의 AUTO 온도 연동, sensor stale 정지/복구 후 재시작 방지,
30초 제한과 종료 시 모터 정지는 사용자가 확인해야 한다. 실제 PM와 stuck I²C는
모의 시험으로 대체할 수 없으며 아직 미검증이다. LCD3.3V 대비 부족과 단계별
RPM/전류/발열 정량 측정도 남아 있다. 로터리는 완료 범위에 포함하지 않는다.

현재 미커밋 BMP180 기능과 후속 수정을 함께 담는 commit 제목:

```text
feat: add BMP180 auto control with atomic restart safeguards
```
