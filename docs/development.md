# 문제 해결

**동작 여부를 확인한 뒤, 실패 원인을 하드웨어·커널·사용자 공간으로 나누어 해결했습니다.**

| 문제                             | 확인한 근거                                                         | 해결                                                                |
| -------------------------------- | ------------------------------------------------------------------- | ------------------------------------------------------------------- |
| ON 상태인데 모터 무회전          | GPIO claim과 달리 pinmux의 tristate/input 설정이 출력 조건과 불일치 | DT/pad 설정 보완 후 실물 회전 확인                                  |
| EC11 무응답·정지 중 가짜 입력    | Raw edge/level 비교, 고정 GND/3.3V는 안정적                         | 채널별 NPN 입력 보강 후 회전 확인; Gray cycle·누락 재동기화 유지    |
| 보호 정지 뒤 AUTO 재기동 경쟁    | GET snapshot과 기동 사이에 suspend/정지가 끼어드는 경로             | `SET_AUTO`에서 차단·freshness·rearm을 kernel mutex 안에서 원자 검증 |
| 센서 종료 결과가 간헐적으로 실패 | Full pipe에서 read end close와 child write의 SIGPIPE 경쟁 재현      | SIGTERM/회수 시도 후 pipe close, blocked write 종료 회귀 추가       |
| DT 재준비 시 잘못된 base 사용    | Fallback을 선택해도 retained overlay에는 활성 DTB를 입력            | 보존 base를 일관되게 사용, 선택·누락·불일치 회귀 추가               |

## 설계에서 남긴 선택

- Blocking 센서는 process, LCD·파일 기록은 worker로 분리하고 **제어 상태는 main 한 곳에서 변경**합니다.
- 정상 센서값은 반복 출력하지 않습니다. `status`·선택형 `monitor`·event log로 관찰합니다.
- 샘플 공급 복구와 모터 재기동을 구분합니다. **장애 복구만으로 다시 ON하지 않습니다.**
- 기술 수를 늘리는 대신 현재 통신량·고장 경로에 필요한 IPC와 동기화만 사용합니다.

입력 CR/LF/CRLF 처리는 PTY 회귀로 검증했습니다. 이 장비의 `sudo + tee` 입력 무응답은 파이프 제거 후 해소됐으나 내부 원인을 확정하지 않았습니다.

[구현 구조](architecture.md) · [회귀 근거](review.md) · [테스트 소스](../tests/)
