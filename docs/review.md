# 검증

**소프트웨어 검증과 실물 확인을 분리해 기록합니다.** 아래는 2026-10-12 DT provider 비교 수정과 커널 형식 정리 후 재검증한 결과입니다.

## 소프트웨어

| 검사                                       | 결과 / 범위                                 |
| ------------------------------------------ | ------------------------------------------- |
| Native app/module/DT, kernel W=1           | 빌드 통과                                   |
| C 11개 · CLI 37개 · 정리 1개 · DT 제안 5개 | 정상/오류·입력·IPC·AUTO·자원 정리 회귀 통과 |
| GCC analyzer                               | 사용자 소스 12개 통과                       |
| ASan/UBSan                                 | 테스트 9개 통과; full pipe 종료 포함        |
| ThreadSanitizer                            | Log/LCD snapshot 스트레스 2개 통과          |
| Kernel/UAPI checkpatch                     | 오류 0 · 경고 0                             |
| DT offline 준비·installer check            | 통과; 적용 DT와 일치, /boot 미변경          |

Kernel build에는 GCC 11.4.0의 Ubuntu package revision 차이 경고가 남아 있습니다. 소스 경고를 숨기거나 compiler를 변경하지 않았습니다.

## 실물과 한계

| 확인된 기존 동작                                | 아직 확인·측정할 부분                              |
| ----------------------------------------------- | -------------------------------------------------- |
| 모터·엔코더·LED·BMP180·LCD 문자 표시            | LCD 대비, 전원 전류/전압 여유                      |
| AUTO 정지/복구·lease·30초 제한·Ctrl+C 정지 보고 | 최종 종료 수정 후 재확인, 모든 fault/recovery 조합 |
| 기본 통합 동작                                  | 실제 system suspend, RPM·발열·장기 반복            |

Mock/stub과 sanitizer는 실제 전기적 조건이나 모든 interleaving의 검증을 대신하지 않습니다. PWM duty는 RPM 측정값이 아니며 hard real-time/전원 장애 시 로그 영속성을 보장하지 않습니다.

## 재현 근거

[테스트 소스](../tests/) · [검사 명령](build-run.md) · [실물 절차](final-test.md)

원본 결과는 로컬 `build/final-fix-validation.log`, `final-fix-module.log`, `final-fix-checkpatch.log`, `final-fix-dt.log`에 보존했습니다. Build/history/원본 사진은 Git에서 제외합니다.
