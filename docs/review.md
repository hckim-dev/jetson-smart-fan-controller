# 최종 검토와 검증 결과

2026-10-12, 1~8단계 구현을 기준으로 검토했습니다. Commit/push, module 적재, 모터 원격 기동, /boot 설치·재부팅은 수행하지 않았습니다.

## 실제 개선

| 항목                     | 근거와 수정                                                                                                                                                                                          |
| ------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 센서 정리의 SIGPIPE 경쟁 | 최종 ASan 검사에서 정상 종료 기대가 실패했다. Read end를 먼저 닫지 않고 SIGTERM/회수 시도 후 닫으며, 종료 신호로 blocked write가 중단된 경우 정상 정리로 처리했다. Full pipe 재현 테스트를 추가했다. |
| DT fallback 입력 혼동    | 보존한 base를 선택한 뒤 retained overlay 적용에는 활성 DTB를 사용했다. 모든 적용에 preparation_base를 사용하도록 수정하고 fallback 선택·누락·불일치 테스트3개를 추가했다.                            |
| 주석과 계약              | 공개 C/header/DTS/Python/shell/Makefile 주석을 한국어로 정리했다. 소유권·신호·lock·실패 이유·단위 중심으로 작성하고 SPDX/ABI/API 식별자는 유지했다.                                                  |
| Kernel 스타일            | Control/struct brace를 커널 규칙으로 정리하고 중복 misc.parent 초기화를 제거했다. Driver용 clang-format 규칙으로 userspace와 구분한다.                                                               |
| 예상 경고의 테스트 출력  | 회수 지연 mock의 경고는 stderr를 capture해 내용까지 assert한다. 실제 sensor/LCD/log 오류 경고와 compiler 검사는 제거하지 않았다.                                                                     |
| 문서·Git                 | 공개 문서를7개로 통합하고 이전6개 파일은 로컬 history에 원본 보존했다. Tests와 프로젝트 전용 AGENTS/formatter 규칙은 공개하고 대화 이력·임시 진행 지시는 제거했다.                                   |

## 구조·성능 판단

Main은 제어 상태와 자원 수명을 소유하고 controller/fan-device, sensor process, LCD/log worker의 기존 책임을 유지했습니다. Kernel의 mutex/workqueue·kref/remove gate·GPIO/PWM 정지 순서, UAPI layout과 번호, DT pad/공급원·timing과 install hash/fallback을 검토했습니다. 불필요한 framework나 추가 IPC, 측정 근거 없는 최적화는 도입하지 않았습니다. Public API와 CLI 이름, 핀 배열·모터 보호 정책을 유지했습니다.

Tests는 GitHub에 유지합니다. 안전 정책과 오류·수명·동기화·정리 조건을 재현하는 소스이며 실행 산출물은 build에서 제외합니다. 일부 LCD tests는 private snapshot/수명을 검사하기 위해 구현 파일을 포함하며 운영 코드에 test hook을 추가하지 않았습니다. Python tests는 표준 라이브러리만 사용합니다.

## 공개 파일 정책

- 유지: README, architecture/hardware/build-run/final-test/development/portfolio/review, 소스·DTS·scripts·tests·Makefile.
- AGENTS.md: 저장소의 자원·검증·변경 규칙을 공유하기 위해 유지한다. 사용자 대화·단계별 임시 승인 기록은 넣지 않는다.
- 로컬 제외: build 산출물/로그, docs/history, 원본 사진/PDF, 편집기/가상환경/cache/임시·core 파일.
- 통합 원본: status/quality-review/stage6-process-ipc/stage7-threads/stage8-operations/encoder-npn-review의 정확한 사본을 docs/history/final-20261012-\*로 보존했다. 사용자 자료는 삭제하지 않았다.

## 수행한 검증

| 검증                 | 결과                                                                               |
| -------------------- | ---------------------------------------------------------------------------------- |
| Native app/module/DT | make all 통과                                                                      |
| Kernel W=1           | 통과; GCC 패키지 revision 차이 안내만 남음                                         |
| Kernel/UAPI 스타일   | matching kernel checkpatch 오류0/경고0                                             |
| 회귀                 | C11개·CLI37개·clean 보존1개·DT 제안3개 통과                                        |
| GCC analyzer         | 사용자 소스12개 통과                                                               |
| ASan/UBSan           | 9개 테스트 통과; full pipe 종료 포함                                               |
| ThreadSanitizer      | log/LCD snapshot 2개 통과                                                          |
| Offline DT           | prepare-dt와 installer --check 통과; 이미 설치된 DT와 일치, /boot 미변경           |
| 기타                 | shell syntax·Python compile·주석 언어·공개 문서 상대 링크·git diff whitespace 검사 |

검증 로그는 로컬 build/final-review-confirmation.log, final-module-review.log, final-checkpatch-confirmation.log, final-dt-review.log에 보존합니다. 최초 sanitizer 실패 로그도 남겼으며 실패를 최종 성공으로 바꾸어 기록하지 않았습니다.

커널을 빌드한 GCC는 Ubuntu11.4.0-1ubuntu1~22.04, 현재 compiler는11.4.0-1ubuntu1~22.04.3입니다. 기본 GCC 버전은 같지만 Kbuild가 package 문자열 차이를 경고합니다. 경고를 숨기거나 시스템 compiler를 변경하지 않았습니다.

## 아직 확인할 사항과 최종 판단

현재 범위의 소프트웨어는 최종 실물 재확인 단계로 진행할 수 있습니다. 수정한 monitor의 정상/오류/종료 경로는 production main+I²C stub으로 검증했고 실제 통전은 수행하지 않았습니다. 사용자는 [최종 테스트](final-test.md)에서 센서 종료·재시작 방지·Ctrl+C·30초 제한·lease·로그/Monitor를 다시 확인해야 합니다. Driver 바이너리를 새로 반영할 때는 controller를 종료하고 module을 재적재합니다. DT 재설치·재부팅은 필요하지 않습니다.

전원 전류/전압 여유, LCD 대비, 실제 system suspend, 단계별 RPM/발열/장기 신뢰성은 기존 미측정 항목입니다. 소프트웨어 검사 성공을 실물·전기적 안전 인증이나 모든 race의 부재로 확대하지 않습니다.
