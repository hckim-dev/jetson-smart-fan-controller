# Build 및 실행

**현재 2단계 명령은 [stage2-run.md](stage2-run.md)를 따른다. 아래는 1단계 실행 이력 및 일반 Build 설명이다.**

이번 작업은 Driver, CLI, DT overlay를 작성하고 compile하는 범위다.
Module load, GPIO 출력, DT 설치, Pinmux 변경, reboot는 수행하지 않는다.
Build 성공과 실제 전압·모터 동작 검증은 별개다.

## 환경과 Build

조사 기준: Jetson Orin Nano P3767-0005/P3768-0000,
L4T 36.5.2, Kernel `5.15.199-tegra`, native `aarch64`, GCC `11.4.0`.
Kernel 제작 compiler는 Buildroot GCC `11.3.0`이다.
Kernel headers의 release와 `Module.symvers`를 확인했다.
`../linux`는 `5.15.185` source이므로 현재 모듈의 Kbuild 경로로 사용하지 않는다.

Repository 최상위에서 실행한다.

```sh
make all
make test
```

`make all`은 다음 세 산출물을 compile하며 설치하거나 실행하지 않는다.

| Target        | 산출물 / 용도                                                      |
| ------------- | ------------------------------------------------------------------ |
| `make app`    | `build/fanctl`: native C11 CLI                                     |
| `make module` | `driver/smartfan.ko`: external Kbuild module                       |
| `make dt`     | `build/smartfan.dtbo`: DT overlay                                  |
| `make test`   | CLI를 build하고 `tests/test_fanctl.py` 실행; dry-run software 검사 |
| `make clean`  | Driver build 산출물 및 프로젝트 `build/` 제거                      |

기본 Kbuild 경로는 `/lib/modules/$(uname -r)/build`다.
다른 경로를 지정해야 할 때는 running kernel과 일치하는 prepared tree만 사용한다.

```sh
make KDIR=/path/to/matching/prepared/kernel all
modinfo driver/smartfan.ko
```

CLI는 `-std=c11 -Wall -Wextra -Wpedantic -Werror -O2`로 build한다.
DT source는 matching kernel의 `dt-bindings/gpio/{gpio,tegra234-gpio}.h`를
`cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp`로 전처리하고,
`dtc -@`로 compile한다. 전처리 결과도 `build/` 아래에 저장한다.
Compiler warning/error 및 Kbuild/modpost 오류는 원인을 확인한다.
GCC minor version 차이를 이유로 warning을 숨기거나 headers를 임의 수정하지 않는다.

현재 `make module`은 CC/MODPOST/LD까지 성공했고 module vermagic은
`5.15.199-tegra SMP preempt mod_unload modversions aarch64`다.
Kbuild는 headers에 기록된 Ubuntu GCC `11.4.0-1ubuntu1~22.04`와 local package
`11.4.0-1ubuntu1~22.04.3`의 문자열 차이에 대한 compiler warning을 출력한다.
이는 위에서 확인한 running kernel 제작 compiler `11.3.0`과 구분하여 기록한다.
실제 module load 및 ABI/runtime 동작은 아직 검증하지 않았다.

## Hardware 없이 CLI 검사

`--dry-run`은 `/dev/smartfan`을 열지 않고 userspace에서 상태와 timeout을 모사한다.
다음 명령은 실제 GPIO/모터를 제어하지 않는다.

```sh
./build/fanctl --dry-run
```

실행 후 `on`, `status`, `heartbeat`, `off`, `quit`를 입력한다.
`--lease-ms`, `--max-on-ms`는 dry-run timeout 검사용 옵션이다.
자동 software regression은 `make test`로 실행한다.
Dry-run 성공은 실제 Driver의 ioctl, workqueue, GPIO 출력 검증을 의미하지 않는다.

`make all`과 `make test`를 실제 실행했고 CLI integration9개가 통과했다.
부분 입력 중 heartbeat 유지, 종료 신호/EOF 처리, 반복 ON의 최대시간 유지 및 만료 후
heartbeat 자동 재시작 금지를 확인했다. Kernel module은 적재하지 않았다.
Dry-run을 SIGSTOP하면 그 process 자체도 멈추므로 OFF 모사는 resume 시 진행한다.
실제 Driver의 독립적인 delayed work에 의한 정지는 hardware/runtime 검증이 필요하다.

실제 CLI는 매250ms 이하 간격으로 heartbeat를 보낸다.
최대 ON30초가 지나면 정지 상태를 표시하며, 사용자가 새 `on`을 입력해야 다시 시작한다.

## DT와 실제 실행의 준비 조건

실제 GPIO는 Driver probe에서 descriptor로 자동 요청한다. 사용자가 sysfs export나 `mknod`를 수행하지 않는다.
현재 live DT에는 `edu,jetson-smartfan` node가 없고 module도 적재되지 않아 `/dev/smartfan`이 없다.
따라서 `insmod`만 실행해도 제어 장치가 생성되는 상태는 아니다. DT 적용과 Pinmux 확인이 먼저 필요하다.
현재 출력 설정을 바꾸지 않는 확인 명령은 아래와 같다.

```sh
sudo sh -c 'cat /sys/kernel/debug/pinctrl/*/pinmux-pins /sys/kernel/debug/pinctrl/*/pinconf-pins' | rg -i 'pg6|pn1|pq5'
```

이 명령은 GPIO나 pad 설정을 변경하지 않는다. root-only 항목은 사용자 Terminal에서 실행하고 결과를 전달한다.
배선/초기 OFF와 부팅 복구 준비가 확인되기 전에는 DT 설치나 module load를 진행하지 않는다.

현재 overlay는 root 아래 `edu,jetson-smartfan` device를 추가하고 GPIO와
timeout 속성을 전달하며, 수정안에서는 세 motor pad의 default pinctrl state도 추가한다.
`tristate=0`, `enable-input=0`, `gpio-mode=0`으로 출력 설정을 보완한다.
기존 header/UART state는 보존하고 Driver probe 시 motor state가 선택되도록 연결한다.
**재부팅 후 module 적재 전에는 motor pad state가 아직 선택되지 않을 수 있다.**
현재 base DT의 `gpio` symbol은 `/bus@0/gpio@2200000`를 가리킨다.
다른 base DT에서는 symbol과 GPIO provider를 다시 확인해야 한다.

| 용도     | J12 물리 Pin | SoC GPIO | 조사한 gpiochip line offset | DT specifier               |
| -------- | -----------: | -------- | --------------------------: | -------------------------- |
| L298 ENA |           32 | PG.06    |                          41 | `TEGRA234_MAIN_GPIO(G, 6)` |
| L298 IN1 |           15 | PN.01    |                          85 | `TEGRA234_MAIN_GPIO(N, 1)` |
| L298 IN2 |           29 | PQ.05    |                         105 | `TEGRA234_MAIN_GPIO(Q, 5)` |

물리 Pin, gpiochip line offset, DT specifier의 값은 서로 다른 식별자다.
Linux 전역 GPIO 번호를 위 표의 값으로 대신하지 않는다.
Overlay의 `compatible`은 P3768-0000/P3767-0005 조합의 super 및 일반 profile로 제한한다.
DT compile 성공은 active overlay, pinmux, pad 전압을 확인한 결과가 아니다.

이번 software 검사에서는 `make dt`가 warning 없이 성공했고,
`/boot/modified.dtb`를 읽어 `fdtoverlay`로 `build/smartfan.offline-merged.dtb`에
합성했다. 외부 `gpio` reference가 resolve되고 EN/IN1/IN2 specifier가 각각
`54`/`105`/`125`로 유지되는 것을 확인했다. `/boot` 원본은 변경하지 않았다.
Running DT blob `/sys/firmware/fdt`는 root-only여서 읽지 않았으므로,
이 결과는 boot 파일에 대한 offline 검사다.

실제 사용 전에 완성 배선·전원·L298 jumper·공통 GND·입력 전압 및
전원 순서에서 EN OFF가 보장되는지 확인한다. GPIO 점유 충돌과 현재 Pinmux도 확인한다.
검증 후 DT 적용 방법과 기존 `/boot/modified.dtb`, Jetson-IO 설정의 복구 경로를 정리하고,
사용자의 시스템 변경 승인을 받은 뒤 실제 load/run 명령을 제공한다.
실제 module load 명령은 아직 제공하지 않는다. 승인된 boot 설치 단계는 아래에 구분한다.

## 사용자 승인 후 DT 적용

사용자가 DT 적용·재부팅 진행을 승인했다. Module load 및 실제 모터 구동은 아직 수행하지 않는다.
현재 준비된 `build/smartfan-board.dtb`와 `build/extlinux.smartfan.conf`를 적용한다.
Pad 수정안은 `make prepare-dt`로 현재 boot DTB/config에 맞춰 다시 준비한다.
이 target은 compile/offline 합성만 수행하며 `/boot`를 변경하지 않는다.
현재 boot config의 hash를 확인해 검토 이후 다른 변경이 있으면 중단한다.
원래 boot 항목을 보존하며 `/boot/modified.dtb`와 기존 Jetson-IO overlay는 덮어쓰지 않는다.

**모터 전원을 분리한 상태를 유지한 뒤** repository 최상위에서 실행한다.

```sh
sudo bash scripts/install-dt.sh
sudo reboot
```

첫 명령이 `DT_INSTALLED` 또는 `DT_ALREADY_INSTALLED`로 성공했을 때만 reboot한다.
Installer는 기존 extlinux config를 timestamp가 붙은 `extlinux.conf.before-smartfan-*`로 백업하고,
new DTB를 먼저 설치한 뒤 config를 atomic rename으로 적용한다. 실제 reboot는 수행하지 않는다.
Pad 수정안의 새 경로는 `/boot/dtb/smartfan-stage1-output.dtb`, 새 entry는 `smartfan-output`이다.
기존 `smartfan`/`JetsonIO` entry와 DTB는 보존한다.
SSH 연결은 재부팅 중 끊어지며 부팅 후 다시 연결한다.

복구: Boot menu에서 보존한 `JetsonIO` 항목을 선택해 이전 DT로 부팅한다.
원래 default로 돌아가려면 installer가 출력한 **정확한 BACKUP 경로**를 extlinux.conf로 복원하고 reboot한다.
백업이 여러 개일 수 있으므로 wildcard로 임의 선택하지 않는다.

재연결 후 DT node를 확인한다.

```sh
tr '\0' '\n' < /proc/device-tree/smartfan/compatible
```

예상값은 `edu,jetson-smartfan`이다. 이 단계는 pinmux 변경이나 실제 모터 동작 검증이 아니다.

수정안 설치/reboot 후 module을 적재한 다음 pad 값을 확인한다.

```sh
sudo cat /sys/kernel/debug/pinctrl/2430000.pinmux/pinconf-groups | grep -Ei -A 14 'soc_gpio19_pg6|soc_gpio39_pn1|soc_gpio32_pq5'
```

세 pad 모두 tristate=0, enable-input=0, gpio-mode=0이어야 한다.
다른 별칭 속성 출력만 보고 추가 bit를 임의로 바꾸지 않는다.
그 다음 motor를 분리한 상태에서 EN 초기 LOW와 ON/OFF 출력 전압을 확인한다.

## DT 확인 후 Driver / CLI 검사

사용자와 에이전트가 재부팅 후 live DT의 `edu,jetson-smartfan`을 확인했다.
다음은 **모터 전원을 분리한 상태**에서, ENA jumper가 제거됐고 GPIO32 wire가
실제 ENA signal pin(5V jumper pin이 아님)에 연결됐음을 확인한 뒤 수행한다.
Jumper/배선을 바꿔야 하면 먼저 정상 종료하고 전원을 분리한다.

```sh
cd /home/aidl/work/jetson-smart-fan-controller
sudo insmod driver/smartfan.ko
ls -l /dev/smartfan
sudo ./build/fanctl
```

CLI 입력 순서: `status`, `on`, `status`, `off`, `quit`.
이 검사는 Driver 등록과 ioctl 상태 전이를 확인한다. 모터 전원을 분리했으므로 실제 회전 시험은 아니다.
Driver가 GPIO를 자동 요청하고 miscdevice node를 생성하므로 export/mknod를 하지 않는다.
insmod 실패 또는 device node 없음이면 CLI를 진행하지 않고 출력과 kernel log를 전달한다.

```sh
sudo dmesg | tail -n 40
```

실제 terminal 결과/전압/모터 결과는 아직 전달받지 않았으며 성공으로 기록하지 않는다.

Driver 기본 lease는 `2000ms`, 최대 연속 ON은 `30000ms`다.
실제 값은 DT 속성 및 Driver 상태 조회로 확인한다.
CLI는 foreground로 device fd를 유지하며 heartbeat를 보낸다.
정상 fd close와 lease 만료, 최대 ON 시간은 각각 다른 정지 경로이므로 실제 환경에서 별도로 검사한다.
Software timeout으로 kernel hang이나 전원 장애를 보장할 수는 없다.
