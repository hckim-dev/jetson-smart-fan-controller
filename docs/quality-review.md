# 6단계 전체 품질 개선 결과

이 문서는 6단계까지의 품질 개선 당시 기록입니다. 최신 완료 범위와 추가 검증은 [status](status.md), [7단계](stage7-threads.md), [8단계](stage8-operations.md)를 기준으로 합니다.

2026-10-12, 기준 commit adbd04b 이후의 작업입니다. 현재 소프트웨어는 다음 단계로 진행할 수 있는 상태이며, 이번 요청에서 이후 단계 신규 기능은 추가하지 않았습니다. Git staging/commit/push와 /boot 설치·재부팅·모터 원격 기동은 실행하지 않았습니다.

## 검토 범위와 판단

Kernel의 probe/open/ioctl/workqueue/PM/remove 수명과 locking, GPIO/PWM 소유권과 정지 순서, userspace의 입력/모드/heartbeat, encoder/IPC/sensor math, LCD worker 종료, build/DT installer, 테스트와 전체 문서를 검토했습니다. 후속 검토에 따라 사용자 프로그램을 main/controller/fan-device로 책임별 분리했습니다. 신규 framework는 도입하지 않았습니다. 이번 구조 분리에서는 Kernel/UAPI/DT를 변경하지 않았습니다.

## 발견과 실제 수정

| 항목                                  | 근거                                                                                                | 개선                                                                                        |
| ------------------------------------- | --------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------- |
| fork~exec의 SIGTERM 상속              | 자식을 exec 전에 멈추는 mock에서 부모 handler가 신호를 소비하고 SIGKILL fallback까지 약0.514초 대기 | fork caller의 control signal 차단, child default disposition 설정 후 해제, parent mask 복원 |
| clean의 사용자 데이터 삭제            | 이전 Makefile을 격리한 복사본에서 measurement.log가 삭제됨                                          | 알려진 산출물만 제거; 로그/메모/boot proposal 보존 테스트                                   |
| 닫힌 stdin의 fd0 재사용               | stdin을 닫은 dry-run이 self-pipe를 입력으로 오인하고 대기하는 경로 재현                             | 자원 생성 전에 fd0 읽기 가능 여부 확인, closed/write-only stdin 회귀 추가                   |
| NDEBUG에 의한 테스트 검증 제거 가능성 | 테스트가 assert를 사용하며 기존 recipe가 CFLAGS를 그대로 상속                                       | TEST_CFLAGS의 -UNDEBUG, release override에서도 검증 유지                                    |
| 검증 절차가 임시 shell 명령에 의존    | analyzer/sanitizer 실행 명령이 build scratch에 흩어짐                                               | make analyze / make sanitize, profile-dry-run.py 추가                                       |
| 상태 통지의 magic numbers             | pending/valid/error/stale를0/1/2/3으로 비교                                                         | 내부 sensor_health enum으로 명명, 동작/IPC 변경 없음                                        |
| 현재 안내와 과거 후보 핀이 혼재       | root docs에서7/31 입력,2ms와 현재12/38/0ms 정보가 함께 존재                                         | 현재 문서를 다시 쓰고 역사 문서/링크를 history로 분리                                       |

기존 SIGKILL fallback과 kernel 정지 정책은 이미 동작했으며 이 변경은 자식 종료 신뢰성을 개선합니다. 수정 후 같은 pre-exec mock에서 약0.010초에 SIGTERM 정리를 관찰했습니다. 실제 blocked I²C 종료의 성능 보장으로 사용하지 않습니다. 새 테스트는 child disposition/mask, fork 실패의 errno/mask/fd 정리도 직접 확인합니다.

## 구조·파일 정리

- 구조를 유지한 모듈: Kernel consumer, main poll/controller, sensor process, LCD worker, encoder decoder. 시간 필터는 진단용으로 유지하며 운영에 강제하지 않습니다.
- Makefile은 app/test 산출물 목록과 test/sanitizer flags를 공통화하고 compiler 환경 override를 존중합니다. C11/경고 검사는 유지합니다.
- 옛 문서17개를 docs/history로 이동하고, 다시 쓴 주요 문서4개의 원본도 보존했습니다. 원본은 삭제하지 않고 `.gitignore`로 로컬 보관하며, 공개 문서는 과거 기록에 의존하지 않도록 링크를 정리했습니다.
- 과거 agent가 만든 check_encoder_debounce, check_encoder_filter_runtime, review_suspend_race의 임시 C파일/실행파일6개를 build에서 제거했습니다. 해당 동작은 tracked 회귀 테스트로 검사합니다.
- 사진/PDF, 실제 실험로그, 사용자 메모와 boot proposal은 삭제하지 않았습니다. 옛 compiler 산출물이나 측정 로그를 일괄삭제하지 않았습니다. History 사진/PDF 링크의 파일은 로컬 Gitignore 자료일 수 있습니다.

## 문서 결과

README를 GitHub 소개/빠른 시작/검증 한계 중심으로 정리했습니다. architecture, hardware, build-run, final-test, status는 현재 구현 기준으로 다시 썼습니다. development는 실제 실패와 설계 결정, portfolio는 발표 흐름과 사용 가능한 소개문을 정리합니다. PPT 파일은 제작하지 않았습니다.

## 최종 검증

| 검증                  | 결과 / 범위                                                                         |
| --------------------- | ----------------------------------------------------------------------------------- |
| make all / module W=1 | native app/module/overlay 통과                                                      |
| make test             | C8개 + CLI29개 + clean 보존1개 통과, 실제 GPIO/I²C 출력 없음                        |
| make analyze          | 사용자 C 소스11개 GCC analyzer 통과                                                 |
| make sanitize         | encoder/filter/BMP math/sensor lifecycle/AUTO guard/LCD shutdown6개 ASan/UBSan 통과 |
| release flags         | CFLAGS=-O2 -DNDEBUG에서도 test assert 활성화 확인                                   |
| offline DT/installer  | prepare-dt와 --check 통과, /boot 미변경                                             |
| shell/Python/문서     | shell syntax, Python compile, whitespace와 상대 링크 검사                           |

현재 kernel은5.15.199-tegra, GCC는11.4.0입니다. Kernel compiler와 Ubuntu package revision 차이 경고는 남지만 실제 C 경고/오류는 숨기지 않았습니다. 기존 driver의 상태 mutex, removed gate/kref/devm 정리와 protected AUTO를 유지했습니다.

## 제한된 성능 관찰

5초 dry-run AUTO+LCD preview에 온도 명령5개를 주입했습니다. `python3 scripts/profile-dry-run.py`로 재현할 수 있습니다.

| 항목                            | 이번 한 번의 결과 |
| ------------------------------- | ----------------- |
| wall time                       | 5.0082s           |
| process user+system CPU         | 0.005790s         |
| 종료 직전 관찰한 RSS high-water | 804KiB            |

CPU는 launch/cleanup을 포함하며 RSS는 /proc의 종료 전 관측입니다. 이 dry-run에서는 실제 센서 자식을 생성하지 않아 live 다중 프로세스 전체의 메모리 수치가 아닙니다. 실제 I²C/GPIO 지연, worst-case scheduling 또는 장기 부하 수치가 아닙니다. 이 작은 표본을 성능 개선율이나 전체 장비 자원 사용량으로 표현하지 않습니다. 현재 근거로는 추가 event framework나 과도한 최적화가 필요하지 않습니다.

## 실물 결과와 남은 위험

사용자는 기본 입출력/로터리 통합, AUTO 정지·복구, controller 중단 후 lease 정지, 30초 제한과 Ctrl+C 종료를 확인했습니다. AUTO의 구체적 fault 주입 절차 및 모든 복구 조합은 기록되지 않았습니다.

남은 중요한 항목은 **Jetson 5V에서 공급하는 모터의 전류/전압 여유와 LCD 시인성**입니다. 모터 정격/기동/정지 전류, 전체 rail 부하와 발열, 단계별 RPM, 실제 system suspend, 반복 내구성은 미측정입니다. 기본 기능 성공만으로 이 전기적/장기 신뢰성을 보장하지 않습니다.

## 다음 단계 판단

현재 1~6단계 소프트웨어와 기본 통합 기능은 정리됐으며 다음 단계 작업을 시작할 수 있습니다. 발표/외부 시연 전에는 [통합 테스트](final-test.md)를 다시 실행하고 전원·LCD 항목을 우선 확인하는 것이 좋습니다. 새로운 기능을 더하기 전에 현재 증거와 한계를 명확히 기록한 상태입니다.

제안 commit 제목:

```text
fix: harden sensor lifecycle and preserve development records
```

## 후속 구조 개선

사용자 요청에 따라 기존 `app/fanctl.c`를 `main.c`, `controller.c/.h`, `fan-device.c/.h`로 분리했습니다. main은 옵션·자원 수명·signal/self-pipe·poll·heartbeat 스케줄을, controller는 MANUAL/AUTO·명령·상태 출력·LCD snapshot을, fan-device는 ioctl·ABI 응답 검사·dry-run 모델을 담당합니다. 센서 sample과 AUTO arm은 controller, dry-run 보호 상태와 deadline은 fan-device에 둡니다. 단일 이벤트 루프, 커널의 원자적 AUTO 검증, 기존 CLI와 실행 파일 이름은 유지합니다.

AUTO 테스트는 더 이상 애플리케이션 `.c`를 직접 포함하거나 main을 매크로로 치환하지 않습니다. 일반 모듈을 링크하고 linker의 `--wrap=ioctl`로 syscall 경계만 모킹합니다. 같은 테스트를 sanitizer 대상에도 추가했습니다.

`docs/history/` 22개 로컬 파일은 삭제하지 않았으며 `.gitignore`로 공개 대상에서 제외했습니다. 현재 문서의 archive 링크를 제거하고 주요 개발 과정은 development에 남겼습니다. 이 경로는 현재 Git 추적 파일이 없으므로 별도 index 제거가 필요하지 않습니다.

분리 후 `make all`과 C8개·CLI29개·clean 보존1개, 사용자 소스11개 analyzer, sanitizer6개 검사가 모두 통과했습니다. 공개 문서의 로컬 링크22개와 `git diff --check`도 통과했습니다. 검증 로그는 로컬 `build/structure-tests.log`, `build/structure-verification.log`, `build/structure-final.log`에 보존했습니다. 실제 통전 시험은 이 변경에서 실행하지 않으므로 [통합 테스트](final-test.md)로 다시 확인해야 합니다.
