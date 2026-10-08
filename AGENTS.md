# 프로젝트 지침

- 한국어로 보고하며, 사용자가 제공한 전체 프로젝트 요청과 상위 지침을 따른다.
- 각 단계의 실제 검증 결과를 보고하고 정상 동작 확인 후 확장한다. 최신 사용자 지시: 승인 질문을 반복하지 않고, 에이전트가 가능한 작업은 수행하며 사용자 작업은 명령과 순서로 바로 안내한다.
- 최신 사용자 지시: hardware 저항 부하 시험을 코딩의 필수 선행 조건으로 두지 않는다. 작성/Build/software test는 먼저 진행할 수 있고, module load/GPIO 출력/모터 통전은 실제 배선·전원 검증 후 진행한다.
- `git add`, `git commit`, `git push`, `git reset --hard`, `git clean -fd`를 실행하지 않는다.
- 실물 및 전기적 적합성이 검증되기 전에는 모터 구동이나 배선 실행을 안내하지 않는다.
- 이 프로젝트의 DT/Pinmux 수정과 필요한 재부팅은 사용자에게 승인됐다. 현 상태/백업/복구 경로를 확인하고 진행하며 중복 승인 질문은 하지 않는다. 실제 통전 안전 조건과 권한 제약은 계속 준수한다.
- 기존 `/boot/modified.dtb`, Jetson-IO overlay, 수업 소스 및 사용자 파일을 덮어쓰지 않는다.
- 실행 Kernel은 조사 시점 `5.15.199-tegra`; Kbuild는 `/lib/modules/$(uname -r)/build`를 사용한다. `../linux`는 `5.15.185`이므로 대체 빌드 경로로 사용하지 않는다.
- 물리 Header 번호, gpiochip line offset, DT GPIO specifier를 구분한다. 하드코딩된 전역 GPIO 번호나 MMIO 주소를 수업 소스에서 복사하지 않는다.
- Carrier의 TXB0108 GPIO 구동 제약을 준수한다. ±20µA 출력 전압 보장 조건과 절대최대 전류를 혼동하지 않는다. 저항을 포함한 LED 직결 시험은 부품/전압/전류 검증 후 판단하며 외부 LED Driver를 무조건 필수로 요구하지 않는다. 모터 전원을 GPIO에서 공급하거나 GPIO 측에 10kΩ pull-down을 임의 추가하지 않는다.
- 직접 구현한 Driver가 모터 제어와 정지 정책을 담당한다. GPIO/PWM provider를 재구현하거나 기존 Driver와 같은 자원을 중복 점유하지 않는다.
- GPIO 요청·출력, I²C scan/read/write, PWM export/enable, module load, system 설정 변경은 읽기 전용 환경 조사와 구분한다.
- Build 성공과 실물 테스트 성공을 구분한다. 진행 상태·검증·Pin Mapping 변경을 `docs/`에 갱신한다.
