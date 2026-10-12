# 프로젝트 개발 지침

이 파일은 저장소에 적용되는 개발·검증 규칙이다. 실행 환경과 배선의 기준 문서는 `docs/build-run.md`, `docs/hardware.md`이다.

## 변경과 Git

- 한국어로 결과를 보고하고 소프트웨어 검증과 실물 확인을 구분한다.
- 기존 변경과 사용자 자료를 보존한다. `git add`, `git commit`, `git push`, `git reset --hard`, `git clean -fd`는 실행하지 않는다.
- Boot 설정·전원·배선 변경은 현재 상태와 백업·복구 방법을 먼저 확인한다. `/boot/modified.dtb`, Jetson-IO 구성과 원본 자료를 임의로 덮어쓰지 않는다.

## 자원과 동시성

- Kernel driver가 모터 출력과 lease/max-on 보호를 소유한다. 기존 GPIO/PWM provider를 재사용하고 동일 자원을 이중 요청하거나 임의 MMIO 쓰기를 추가하지 않는다.
- main이 controller 상태를 변경한다. Sensor는 process/pipe, LCD와 선택형 log는 snapshot/queue worker이다. Fork는 worker 생성 전에 수행한다.
- 오류·종료·suspend 후 자동 재기동을 허용하지 않는다. AUTO는 유효 sample과 명시적 ON을 요구한다.
- 주석은 한국어로 이유·계약·실행 문맥을 설명한다. SPDX, API 이름, ABI 식별자는 원문을 유지한다.
- 사용자 C 코드는 C11/공백 4칸, kernel/UAPI/DTS는 해당 커널 스타일을 따른다. 기존 ABI layout과 ioctl 번호를 변경하지 않는다.

## 환경과 검증

- Kbuild는 실행 kernel과 일치하는 `/lib/modules/$(uname -r)/build`를 사용한다. 다른 버전의 수업 kernel tree로 대체하지 않는다.
- 물리 header 번호, gpiochip offset, DT specifier를 구분한다. GPIO 전류 한계와 전압 보장 조건을 혼동하지 않는다.
- Module 적재, GPIO/I²C/PWM 요청, 실제 모터 통전은 읽기 전용 조사나 dry-run과 구분한다. 전기 조건이 미확인인 배선을 실행하지 않는다.
- 변경에 맞는 `make test`, analyzer/sanitizer/thread 검사와 DT offline 검증을 수행한다. 경고를 감추려고 compiler 옵션·시간 제한을 바꾸지 않는다.
- `docs/history`, build 로그, 원본 사진/PDF는 로컬 보관 자료다. 공개 문서는 현재 구현과 확인한 결과만 설명한다.
