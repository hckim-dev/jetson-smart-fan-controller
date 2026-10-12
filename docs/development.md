# 개발 과정과 설계 결정

## 요구 단계와 실제 구현

| 요구 단계        | 현재 구현                                               | 증거 / 범위                                                |
| ---------------- | ------------------------------------------------------- | ---------------------------------------------------------- |
| 1. Kernel ON/OFF | platform driver, miscdevice, GPIO consumer, bounded run | native module build, 사용자 motor 회전 보고                |
| 2. Rotary/PWM    | GPIO v2 양edge/Gray, 0~5단계, NPN 입력 보강             | mock request/decoder tests, 사용자 통합 입력 성공          |
| 3. LCD           | PCF8574 4bit 초기화, worker, 모드/온도/풍속             | protocol/mock tests, 사용자 문자 표시(대비 미해결)         |
| 4. LED Bar       | optional GPIO8개, 독립시험/풍속연동                     | level mapping tests, 사용자8칸 점등                        |
| 5. BMP180/AUTO   | calibration/보정식, hysteresis, fault/stale stop        | Bosch 예제와 실제I²C/IPC, AUTO 회귀 및 사용자정지/복구보고 |
| 6. Process/IPC   | sensor fork/exec/pipe/waitpid/signals                   | partial IPC/exec 오류/수명 tests, 실제child sample         |

구현 순서는 전기 조건과 실물 확인에 맞춰 조정했습니다. Motor→PWM→LED→LCD→BMP/AUTO/IPC를 진행하고, 미해결 EC11 입력을 마지막에 NPN 회로로 보강했습니다. LCD I/O를 main에서 분리하기 위한 thread는 이미 구현된 구조이며 별도의 다음 단계 확장을 추가한 것이 아닙니다.

## 1. GPIO 요청 성공과 실제 출력은 달랐다

초기ON 상태는 표시됐지만 모터가 회전하지 않았습니다. 실제 pinconf의 tristate/input 설정을 확인하고 출력 pad 적용을 보완한 뒤 사용자 회전을 확인했습니다. GPIO claim이나 logical 상태만으로 header 출력/회전을 증명할 수 없다는 점을 기록했습니다. 물리핀·gpiochip line·DT specifier를 구분해 provider를 사용했습니다.

## 2. EC11 무응답/가짜 회전의 계층 분리

기존7/31 입력을12/38로 옮긴 뒤에도 문제가 남았습니다. Raw edge, IRQ, 직접 level sampling을 비교했고, debounce0은 정지 중에도 변화하고1/2ms는 정상 회전도 거의 제거했습니다. 단순 STEP rate limit 대신 독립 진단과 안정 시간 필터를 시험했지만 실물 입력을 복구하지 못했습니다.

GPIO를 GND/3.3V에 고정한 시험은 모두 안정적이었습니다. 이후 사용자가 채널별 NPN(베이스10kΩ / 컬렉터 pull-up1kΩ) 회로에서 회전 성공을 확인했습니다. Push-pull buffer가 유력 후보였지만 필수라고 단정했던 판단은 수정했습니다. 작은 pull 저항의 조건부 허용과 실제 결과를 반영해 현 회로를 유지했습니다.

운영은 검증된 기간 0을 사용하고, Gray 4전이와 누락시 재동기화를 유지합니다. 진단용 software filter는 운영 성공을 대신하는 증거로 사용하지 않습니다.

## 3. AUTO의 GET→ON 경쟁 조건

여러 ioctl의 상태 snapshot 사이에 suspend/보호 정지가 끼어들면 오래된OFF/USER 상태로 자동 재시작할 수 있었습니다. 반복 GET으로 해결하지 않고 SET_AUTO에 보호 정지 latch/freshness/rearm을 넣어 kernel mutex 안에서 검사하도록 보완했습니다. Mock으로 경쟁 경로를 검사하며 실제 system suspend의 실물 시험은 별도입니다.

## 4. Blocking I²C와 heartbeat 분리

센서 conversion과 LCD 송신을 main에서 수행하면 입력/heartbeat 처리가 지연될 수 있습니다. Sensor는 별도 process, LCD는 snapshot worker로 분리했습니다. IPC는 고정frame/partial I/O/sequence/range를 검사하며, LCD 변경행만 전송합니다. 메모리 공유가 필요 없는 센서에는 pipe가 충분했습니다.

후속 품질 검토에서는 fork~exec 구간의 상속된 신호 핸들러 때문에 SIGTERM 정리가 지연되는 경로를 재현했습니다. Child default disposition과 mask를 exec 전에 설정해 보완했습니다. LCD join timeout 뒤 살아 있는 객체를 free하지 않는 수명 정책도 테스트합니다.

## 5. 명령 입력 장애와 과도한 출력

BMP 정상sample마다 MODE/SENSOR를 출력해 명령 결과가 가려졌습니다. Normal 측정은 조용히 유지하고 상태전이와status 요청만 표시하도록 바꿨습니다. 수신명령은CMD로 추적합니다.

LF-only parser는 raw PTY에서 CR Enter를 무시하는 결함이 있어CR/LF/CRLF를 지원했습니다. 실제 사용자의 sudo+tee 실행은 canonical=1/icrnl=1에서도 입력이 안 됐으므로 같은 원인으로 확정하지 않았습니다. 외부 파이프 제거 후 명령 입력, --encoder 추가 후 로터리 통합 성공을 확인했습니다. sudo 내부 원인을 분석 완료했다고 주장하지 않습니다.

## 근거와 원본

주요 commit은 e2da01d(ON/OFF), 11ad335(PWM/LED/LCD), 0cbbf82(BMP/AUTO), adbd04b(EC11/CLI)입니다. 이번 품질개선은 아직 commit하지 않았습니다. 주요 조사 결과와 해결 과정은 이 문서에, 최신 검증 범위는 [quality-review](quality-review.md)에 정리합니다. 과거 원본은 로컬 `docs/history/`에 보존하고 `.gitignore`로 공개 대상에서 제외합니다.
