# EC11 NPN 입력 보강 회로 검토 — 2026-10-12

사용자가 S1/S2 각각 NPN 회로를 구성하고 no-debounce 회전 동작 성공을 보고했다.
확인한 구성은 EC11→B 직렬10kΩ, C→3.3V pull-up1kΩ, E→공통GND,
C→Jetson 물리12/38이다. 에이전트가 해당 회로의 실제파형/전압을 측정한 것은 아니다.

```text
             3.3V
               |
              1kΩ
               |
               +---------- Jetson 물리12(S1) 또는38(S2)
               |
               C
EC11 S1/S2--10kΩ--B  MPS2222A 후보 NPN
               E
               |
             공통GND
```

## 판단

기존 베이스10kΩ과 컬렉터pull-up1kΩ을 유지하면서 전압/정지 안정성을 검증하는
것이 적절하다. 앞선 NPN+저항 방식 배제는 과도했다. TXB0108의50kΩ은 보수적
권장조건이며Rev.L p.3 note D는 VOH/VOL 변화가 허용되면 작은pull저항도조건부로
허용한다. 이번회로는push-pull은아니지만NPN이LOW를능동구동하고1kΩ이HIGH를
올린다. 10kΩ pull-up보다구동/부하조건이변경된것이개선원인후보이다.
[TI TXB0108 Rev.L](https://www.ti.com/lit/ds/symlink/txb0108.pdf).

LOW0.2V를가정하면1kΩ분기전류는(3.3-0.2)/1000=3.1mA,
저항전력은약9.6mW이다. 전압전체3.3V를가정한상한도3.3mA/10.9mW다.
이는pull-up분기의전류이며TXB출력과베이스전류를포함한총전류가아니다.
3.3V rail→저항→NPN→GND 경로이며GPIO가3.3mA를출력한다는뜻도아니다.
베이스10kΩ의전류는VBE0.7V/입력3.3V가정시약0.26mA이나모듈내저항이있으면
더작다. 증폭률만으로포화를보장하지않고실제LOW를확인한다.
[onsemi MPS2222/A](https://www.onsemi.com/download/data-sheet/pdf/mps2222-d.pdf).

## 최소 변경 / 확인

- S1/S2에같은회로를각각사용하고공통GND와3.3V공급을유지한다.
- 동작중인Rc1kΩ/Rb10kΩ은현재변경하지않는다.
- 선택보완: NPN회로근처3.3V–GND 사이100nF 전원decoupling.
  GPIO신호선에큰capacitor를추가하는것과구분한다.
- 선택보완: 각NPN의B–E 사이100kΩ으로입력분리시베이스부동을완화할수있다.
  필수는아니며추가시베이스전류가조금줄어드므로LOW와동작을다시확인한다.
- GPIO12/38은입력으로유지한다. 두회로모두신호를반전하지만두채널동시반전은
  quadrature의상대위상/회전방향을뒤집지않는다. 실제방향은배선/장착기준과대조한다.
- C노드를공통GND기준으로회전중정지위치를바꿔측정한다.
  LOW0~0.4V/HIGH3.0~3.3V는여유를보기위한목표이며datasheet최소threshold가아니다.
- 정지중가짜STEP없음,양방향회전인식,sequence_gaps증가여부를기록한다.
  user보고의동작성공을장기/온도범위/모터부하시신뢰성보장으로확장하지않는다.
- 후속통합에서fanctl/encoder-monitor의기본기간을0으로변경했다. no-debounce
  monitor 성공과통합fanctl 실물완료는구분하며 [최종시험](final-test.md)을수행한다.
