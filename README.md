# jetson-smart-fan-controller

Linux Device Driver 기반 Jetson Orin Nano 스마트 선풍기 프로젝트.

현재 상태: **Motor/LED Bar 실물 성공, LCD 문자 표시 확인(대비 부족). BMP180 실제 온도·기압/IPC 측정 성공, AUTO 및 통합 코드 구현 완료. Encoder 실물은 제외/보류.**

지금 실행할 순서는 [최종 테스트 순서](docs/final-test.md)를 따른다.
수정 내역과 검증 범위는 [최종 재검토](docs/final-review.md), AUTO 정책은
[BMP180 / AUTO 실행 안내](docs/bmp180-run.md)에 정리했다.
기능별 검증과 남은 실물 확인은 [완성도 검토](docs/completeness-review.md)에 정리했다.
레벨 시프터 없이 시험한 LCD 전체3.3V 공급을 유지한다.5V 공급으로 바꿀 때는 I²C 전압 변환이 필요하다.
Encoder 관련 이전 구성/진단 기록은 [핀 변경 시험](docs/encoder-alt-pin-trial.md)과
[조사 결과](docs/encoder-audit.md)에 보존했다.

```sh
make all
make test
./build/fanctl --dry-run
```

`--dry-run`에서 `on`, `speed 1`~`speed 5`, `speed 0`, `status`, `off`, `quit`를 입력할 수 있다.
모터 OFF 상태에서 `led 0`~`led 8`로 점등을 시험하고 `led auto`로 자동 표시에 복귀한다.
`./build/fanctl --dry-run --lcd`는 LCD 표시 preview도 하드웨어 없이 시험한다.
`./build/fanctl --dry-run --bmp180 --lcd`에서는 `temp 27`, `mode auto`, `on`,
`temp error`로 AUTO 동작과 센서 오류 정지를 시연할 수 있다. 실제 구동은
`sudo ./build/fanctl --bmp180 --lcd --lcd-address 0x27`이며 시작은 MANUAL/OFF다.
실제 GPIO를 사용하지 않는 CLI 시험이다. 실제 실행은 배선·전원·초기 OFF 검증 후 진행한다.

- [Build / CLI 사용 / 실제 실행 준비](docs/build-run.md)
- [1단계 종료 확인](docs/stage1-checklist.md)
- [환경 조사](docs/environment.md)
- [하드웨어 검토 및 연결 후보](docs/hardware.md)
- [사진 확인 결과와 선별 상세 연결표](docs/wiring.md)
- [Jetson 핀 → 부품 단자 간단 연결표](docs/pin-connections.md)
- [강의 자료 반영 및 LED 구동 판단 정정](docs/lecture-review.md)
- [L298N 전원 및 하드웨어 검사](docs/l298-hardware-test.md)
- [Software Architecture 및 Build 설계](docs/architecture.md)
- [진행 상태와 단계별 검증](docs/status.md)

개발은 최소 ON/OFF 완성 → 실제 테스트 → 사용자 확인 → 확장 순서로 진행한다.
Git staging/commit/push는 사용자가 직접 수행한다. 발표 자료는 별도 요청 전까지 만들지 않는다.
