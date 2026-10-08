# jetson-smart-fan-controller

Linux Device Driver 기반 Jetson Orin Nano 스마트 선풍기 프로젝트.

현재 상태: **1단계 GPIO 제어 motor 회전 확인, 종료·반복·재적재 검증 대기**.
사용자 지시에 따라 hardware 부하 시험을 코딩의 선행 조건에서 분리했다.
사용자가 module load, 수정 DT 적용 및 실제 motor 회전을 확인했다. Hardware safety regression은 계속 구분해 기록한다.

```sh
make all
make test
./build/fanctl --dry-run
```

`--dry-run`에서 `on`, `status`, `off`, `quit`를 입력할 수 있다.
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
