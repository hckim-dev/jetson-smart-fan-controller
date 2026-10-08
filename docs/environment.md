# 환경 조사

조사일: 2026-10-08 (Asia/Seoul). 로컬 Remote-SSH 대상에서 읽기 전용으로 확인했다.

## 확인된 환경

| 항목 | 관찰 결과 / 근거 |
|---|---|
| Board model | `NVIDIA Jetson Orin Nano Engineering Reference Developer Kit Super` (`/proc/device-tree/model`) |
| DT compatible | `nvidia,p3768-0000+p3767-0005-super`, `nvidia,p3767-0005`, `nvidia,tegra234` |
| Module SKU | `699-13767-0005-300 W.1` (`chosen/nvidia,sku`) |
| Carrier | DT상 P3768-0000. 실물 revision/표시는 사진으로 추가 확인 필요 |
| OS / CPU | Ubuntu 22.04.5 LTS / aarch64 |
| L4T | 36.5.2 (`/etc/nv_tegra_release`, `nvidia-l4t-core`) |
| JetPack | L4T 36.5.2의 공식 대응 버전은 6.2.3. `nvidia-jetpack` 메타패키지는 설치 확인되지 않아 SDK 전체 설치 여부와 구분 |
| Running Kernel | `5.15.199-tegra`, SMP PREEMPT |
| Kernel package | `5.15.199-tegra-36.5.2-20260716114719` |
| Compiler | Native GCC / G++ 11.4.0 |
| Kernel 제작 compiler | Buildroot GCC 11.3.0 (`/proc/version`, generated `compile.h`) |
| Build tools | Binutils 2.38 / GNU Make 4.3 / DTC 1.6.1 |
| GPIO tools | libgpiod tools 1.6.3 |
| I²C tools | i2c-tools 4.3 |
| Privilege | 사용자 `aidl`, `gpio`/`i2c` 그룹. 비대화식 sudo 불가: password 필요 |
| Repository | 기존 `master` branch, 첫 commit `742a909`, 조사 시작 시 clean; README 제목만 존재 |

JetPack 대응 버전 근거: [NVIDIA JetPack Archive](https://developer.nvidia.com/embedded/jetpack-archive).

## Kernel build 조건

`/lib/modules/5.15.199-tegra/build`의 실제 경로:

```text
/usr/src/linux-headers-5.15.199-tegra-ubuntu22.04_aarch64/
  3rdparty/canonical/linux-jammy/kernel-source
```

`UTS_RELEASE` 및 `include/config/kernel.release`가 실행 Kernel과 일치한다.
`.config`, `Module.symvers`, generated headers, native `modpost`/`fixdep`가 존재한다.
`CONFIG_MODULES=y`, `CONFIG_MODULE_UNLOAD=y`, `CONFIG_MODVERSIONS=y`이며
`CONFIG_MODULE_SIG_FORCE`는 미설정이다. 이는 module load 성공을 보장하지 않는다.

Headers `.config`의 GCC 표기(11.4)와 running Kernel compiler(11.3)는 다르다.
1단계 실제 Kbuild 경고와 새 module의 vermagic/load 결과를 확인해야 한다.
Cross compiler 없이 native aarch64 build를 우선한다.
**이번 단계에서는 compile/load를 수행하지 않았다.**

## GPIO / PWM / I²C

| 자원 | 관찰 결과 | 남은 확인 |
|---|---|---|
| GPIO | `/dev/gpiochip0`: `tegra234-gpio`, 164 lines; chip1: `tegra234-gpio-aon`, 32 lines | 선택 pin의 실제 padmux/output 및 초기 전압 |
| GPIO API | `CONFIG_GPIO_CDEV=y`; `/sys/class/gpio` 없음 | descriptor 기반 Driver 사용 |
| PWM | `32a0000`, `32e0000`, `3280000`, `32c0000` controller가 등록됨 | pin routing 및 파형; sysfs 등록만으로 Header 출력 확인 불가 |
| PWM 추가 항목 | `39c0000.tachometer`도 pwmchip으로 보임 | 모터 출력 채널로 사용하지 않음 |
| I²C | `/dev/i2c-{0,1,2,4,5,7,9}` 관찰 | 외부 부품 연결 여부, 주소, 전압 확인 후 통신 검사 |
| BMP180 Driver | `CONFIG_BMP280` 미설정, 관련 module 없음 | 향후 upstream module 또는 i2c-dev 중 하나 선택 |
| Encoder Driver | `CONFIG_INPUT_GPIO_ROTARY_ENCODER` 미설정, 관련 module 없음 | 향후 upstream module build 또는 GPIO event 처리 선택 |

PWM 매핑은 조사 시점 다음과 같다. `pwmchipN` 번호는 고정 식별자로 저장하지 않는다.

| Controller | 현재 sysfs | 비고 |
|---|---|---|
| `32a0000.pwm` | pwmchip0 | runtime DT `pwm-fan`이 channel 0 소비; 프로젝트에서 사용 금지 |
| `32e0000.pwm` | pwmchip1 | Header pin32 PWM7 후보 |
| `3280000.pwm` | pwmchip2 | Header pin15 PWM1 후보 |
| `32c0000.pwm` | pwmchip3 | Header pin33 PWM5 후보 |

I²C adapter의 현재 주소는 아래와 같다. 주소는 controller MMIO 식별이며 센서 slave 주소가 아니다.

```text
i2c-0 3160000.i2c     i2c-1 c240000.i2c
i2c-2 3180000.i2c     i2c-4 Tegra BPMP I2C adapter
i2c-5 31b0000.i2c     i2c-7 c250000.i2c
i2c-9 NVIDIA SOC i2c adapter (display)
```

Runtime DT symbol `hdr40_i2c1`은 `/bus@0/i2c@c250000`이다.
따라서 해당 Header bus는 현재 `/dev/i2c-7`이다. 과거 예제의 `/dev/i2c-1`을 복사하지 않는다.
기존 `0-0050`, `0-0057` EEPROM 등 장치가 있으므로 무차별 scan을 수행하지 않았다.

## 현재 부팅 설정과 Pinmux

`/boot/extlinux/extlinux.conf`:

- `DEFAULT JetsonIO`
- 해당 entry의 `FDT /boot/modified.dtb`
- `OVERLAYS /boot/jetson-io-hdr40-user-custom.dtbo`
- `primary` entry도 있지만 실제 fallback 부팅/복구는 검증하지 않았다.

Runtime DT에 `hdr40-pin11` 및 `hdr40-pin36`의 `uarta` 설정이 나타난다.
Pin11/36은 후보 GPIO에서 제외한다. 나머지 pad 설정은 boot firmware 설정도 관여하므로,
`gpioinfo`의 `unused`나 controller의 `status=okay`만으로 실제 출력 가능하다고 판단하지 않는다.
root-only debugfs 접근은 불가하여 실제 pinctrl register 상태와 PWM consumer 상세는 미확인이다.

`CONFIG_OF_OVERLAY=y`지만 `/sys/kernel/config/device-tree/overlays` 인터페이스가 없다.
runtime configfs overlay를 당연히 사용할 수 있다고 가정하지 않는다.
DT 설치가 필요하면 기존 파일 백업, 별도 boot entry 및 실제 UART/Recovery 접근 방법을 준비하고 승인 후 수행한다.
현재 `/boot`/DT/Pinmux는 변경하지 않았다.

설정 방법 근거: [현재 L4T의 NVIDIA Orin NX/Nano bring-up 문서](https://docs.nvidia.com/jetson/archives/r36.5.2/DeveloperGuide/HR/JetsonModuleAdaptationAndBringUp/JetsonOrinNxNanoSeries.html).

## 수업 자료

- `../practice/EX08-03_std_platform_dt/{Makefile,devtest.c,dt.txt}`
- `../practice/EX08-04_std_platform_dt_hcsr04/howto.txt`
- `../extracted.dts`, `../modified.dtb` 및 기존 `/boot` 파일: 현 구성의 참고자료, 수정 금지
- `../examples`: 주로 AI 수업 자료

수업 Kbuild 경로는 참고할 수 있지만, fixed major 120, 전역 GPIO 461, IRQ 133,
phandle 0xf3, MMIO 주소 등을 새 Driver에 복사하지 않는다.
`../linux/Makefile`의 버전은 **5.15.185**이므로 실행 Kernel용 source로 취급하지 않는다.
