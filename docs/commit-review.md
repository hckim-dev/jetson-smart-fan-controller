# 커밋 전 최종 검토 — 2026-10-12

**판정: 현재 변경은 커밋할 수 있다.** 변경된 운영 경로에서 추가 확정 결함을
찾지 못했고 현재 소스의 build 및 회귀 테스트를 통과했다. Git staging/commit/push는
실행하지 않았다. 기본 동작 성공과 모든 실물 고장 시험의 완료는 구분한다.

## 최종 동작과 변경 범위

- EC11은 채널별 NPN, 베이스 직렬10kΩ, 컬렉터3.3V pull-up1kΩ으로 구성한다.
  각 컬렉터를 Jetson 물리12/38에 연결한다. 사용자 회전 성공 보고를 반영했다.
- fanctl/encoder-monitor 기본 debounce 기간은0이다. 이전1/2ms 설정이 남지
  않도록 GPIO request의0 attribute를 유지한다. Tegra hardware enable bit를
  직접 끄는 변경은 아니다. Gray4전이 해석과 sequence 재동기화는 유지한다.
- 로터리는 MANUAL에서 풍속0~5를 변경하고0에서 정지한다. OFF 뒤 회전은
  선택만 변경하며 명시적ON에서만 시작한다. AUTO에서는 로터리를 무시한다.
- 정상 BMP sample의 반복 터미널 출력을 제거했다. 처음성공/오류/복구/stale를
  알리며status는현재값을출력한다. 센서측정과LCD/AUTO갱신은계속된다.
- 수신 명령은CMD로 표시하고CR/LF/CRLF를 지원한다. CR-only 미처리는rawPTY에서
  별도로 재현/수정했다. 실제 사용자 입력 장애는canonical=1/icrnl=1에서도
  발생했으므로이결함과같은원인으로확정하지않았다.
- 외부tee파이프를제거한직접실행에서명령입력이복구되고--encoder를포함한
  통합실행의로터리동작도사용자가확인했다. sudo/PTY중계내부원인은미확정이다.
- 소프트웨어 안정 시간 필터와 debounce 선택은 encoder-monitor의 진단 기능이다.
  운영 fanctl에는 실물 성공 조건인 기간0을 적용하며 진단 필터를 적용하지 않는다.

## 코드 검토

GPIO입력요청/flags와fd수명, nonblocking/CLOEXEC, Gray전이/반전/상하한,
순서대로풍속변경하는batch처리, AUTO에서입력을무시하는분기, 명령파서의
부분입력/길이/NUL/CRLF, sensor통지상태, LCD snapshot/종료, motor close/
lease/max-on 정책의연결을검토했다. 코드구조를바꿀확정성능문제는없었다.
이번변경에kernel UAPI/driver/DT 변경은없다.

## 검증

- 현재소스로make all 및make module W=1 성공.
- C테스트8개, CLI테스트28개 통과. IRQ입력요청의기본기간0, input/both-edge,
  CLOEXEC/nonblocking은mock syscall로확인했다.
- rawPTY의CR/CRLF 명령수신·ON/OFF·중복실행방지, 센서정상100sample 출력억제와
  오류/복구/stale통지, 엔코더0정지/재시작방지/AUTO 충돌회귀를포함한다.
- 변경된fanctl/encoder-monitor/encoder-filter의GCC analyzer 통과.
  앞선encoder/필터ASan/UBSan도통과했으며그뒤해당핵심해석코드는변경하지않았다.
- whitespace 검사 및문서local link확인. build 로그와driver .ko는gitignore 대상이다.
- 컴파일러경고는kernel GCC11.4.0과현재GCC11.4.0의Ubuntu package revision차이다.
  module build성공과실제파형검증을같은것으로보지않는다.

## 실행 / 남은 검증

```sh
sudo ./build/fanctl --encoder --bmp180 --lcd --lcd-address 0x27
```

interactive 실행은외부tee를붙이지않는다. `mode manual`, `speed 5`, `on`,
`status`, `off`, `quit` 및로터리시험은 [최종시험순서](final-test.md)를따른다.
사용자가기본통합동작을성공보고했지만sensor fault/stale, controller STOP lease,
30초최대ON과PM조합을모두실물검증했다고보고한것은아니다. 이항목은체크리스트로
남긴다. LCD3.3V의대비부족/최근흐려짐은미해결이며이번변경은LCD송신/백라이트
설정을바꾸지않았다. 모터단계별RPM/전류/발열및장기내구성도별도측정이다.

현재변경전체를담는커밋제목:

```text
feat: finalize EC11 input and improve interactive CLI diagnostics
```

새파일app/encoder-filter.c/.h와tests/test_encoder_filter.c,
tests/test_encoder_config.c도함께포함해야Makefile대상이완성된다.
