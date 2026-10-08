# Encoder 입력 핀 변경 시험: J12 12·38번

**최신 상태:** 새 DT의 실제 부팅 적용 및 smartfan driver bind를 확인했다.
재설치·재부팅을 반복할 필요 없이 [Encoder 조사 결과](encoder-audit.md)의
20초 진단 명령을 실행한다. 아래 설치 순서는 변경 이력/복구 참고용이다.

기존 구성의 S1→7번(PAC.06/offset144), S2→31번(PQ.06/offset106)에서는
탈착 시 GPIO edge가 관찰됐지만, 축 회전 시 event가 없었다. 특정 핀이나
carrier channel 문제인지 확인하기 위해 사용하지 않는 두 GPIO로 옮긴다.
이 시험은 실제 회전 동작이 확인되기 전까지 원인 확정으로 기록하지 않는다.

| Jetson **J12 물리 핀** | Encoder 단자 | Linux GPIO |
|---|---|---|
| **12번** | **S1** | `/dev/gpiochip0` offset **50**, PH.07 |
| **38번** | **S2** | `/dev/gpiochip0` offset **52**, PI.01 |
| 기존 **3.3V 빵판 +** | 전원 단자 **`5V` 인쇄** | 실제 공급은 **3.3V** |
| 기존 공통 GND | **GND** | 연결 유지 |
| 연결하지 않음 | **KEY** | 연결하지 않음 |

12·38번은 I2S 겸용 핀이지만 현재 `gpioinfo`에서 두 line 모두 `unused input`이며,
이번 별도 DT의 `smartfan-output/encoder-inputs`가 두 pad를 GPIO 입력으로 설정한다.
19·21번은 활성 `/dev/spidev*`가 있어 선택하지 않았다. 기존 모터 세 신호선
ENA→32, IN1→15, IN2→29와 I²C/UART/LED 예약 핀은 변경하지 않는다.
GPIO offset은 **물리 핀 번호가 아니다**.

## 실행 순서

1. `encoder-monitor` 및 `fanctl --encoder`를 Ctrl+C로 종료한다. Jetson을
   종료하고 L298N의 모터 전원을 끈다.

   ```sh
   sudo shutdown -h now
   ```

2. Jetson 전원이 꺼진 뒤 **S1 선만 7번에서 12번으로**, **S2 선만 31번에서
   38번으로** 옮긴다. 전원 단자의 3.3V 공급과 GND는 유지한다.
   엔코더에 5V 빵판을 연결하지 않는다. 연결 후 Jetson을 다시 켠다.

3. 재접속 후 아래 명령을 차례로 실행한다. 준비 script는 새 DTB
   `/boot/dtb/smartfan-stage2-encoder-alt.dtb`와 새 boot entry
   `smartfan-encoder-alt`를 설치한다. 기존 `smartfan-speed` 및 DTB는 남는다.
   오류가 나오면 그 자리에서 멈추고 출력 내용을 확인한다.

   ```sh
   cd /home/aidl/work/jetson-smart-fan-controller
   make all
   make test
   make prepare-dt
   bash scripts/install-dt.sh --check
   sudo bash scripts/install-dt.sh
   sudo reboot
   ```

4. 재접속 후 실제 DT 입력 핀과 raw edge를 확인한다. `fdtget` 결과가
   `soc_gpio41_ph7 soc_gpio43_pi1`이어야 한다. 모터를 켤 필요는 없다.

   ```sh
   cd /home/aidl/work/jetson-smart-fan-controller
   sudo fdtget -t s /sys/firmware/fdt /bus@0/pinmux@2430000/smartfan-output/encoder-inputs nvidia,pins
   ./build/encoder-monitor --no-debounce --raw
   ```

   손잡이를 한 클릭씩 천천히 돌린다. `EDGE`가 회전과 함께 늘어나는지 보고,
   완전한 Gray cycle이면 `STEP delta=+1` 또는 `STEP delta=-1`도 나온다.
   Ctrl+C 후 `CLOSED events=...` 값을 확인한다. `STEP` 방향만 반대라면
   `./build/encoder-monitor --reverse --no-debounce --raw`를 실행한다.
   두 프로그램이 동시에 같은 GPIO line을 열 수 없으므로 monitor를 종료한
   다음에 `fanctl --encoder`를 실행한다.

5. 여전히 회전 event가 없다면 monitor를 Ctrl+C로 종료하고 다음 입력
   level 진단을 실행한다. `--poll-levels`는 IRQ를 사용하지 않는다.

   ```sh
   ./build/encoder-monitor --poll-levels
   ```

   회전 중 `LEVEL change`가 있는지 확인한다. 새 핀에서도 `EDGE=0`과
   level 변화 없음이면 원래 두 GPIO만의 문제라는 가설은 약해진다.
   이때 모듈 출력과 carrier 입력 경로의 전기적 적합성은 별도로 남는다.

## 복구

새 항목에서 부팅 문제가 생기면 부팅 메뉴의 기존 `smartfan-speed`를 선택한다.
기존 배선으로 되돌릴 때는 Jetson과 L298N 전원을 끈 뒤 S1→7번, S2→31번으로
복귀한다. 기존 `/boot/dtb/smartfan-stage2.dtb`는 이 시험에서 덮어쓰지 않는다.
