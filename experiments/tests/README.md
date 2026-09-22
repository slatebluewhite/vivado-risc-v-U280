# 자체 작성 테스트 (U280 Gemmini 실험용)

이 파일들은 `generators/gemmini/software/gemmini-rocc-tests/bareMetalC/`에서 작성했다.
그 디렉터리는 **중첩 서브모듈**(`SKIP_SUBMODULES`)이라 추적되지 않으므로,
`make update-submodules` 시 유실될 수 있어 여기에 사본을 둔다.

사용하려면 위 경로로 복사한 뒤 다음처럼 빌드한다:

```bash
cd generators/gemmini/software/gemmini-rocc-tests
riscv64-linux-gnu-gcc -O2 -static -march=rv64gc -mabi=lp64d -I. -Iinclude \
  -o /tmp/<name> bareMetalC/<name>.c -lm
```

**중요**: `include/gemmini_params.h`는 config별로 덮어써지는 공유 파일이다.
빌드 전에 대상 하드웨어의 `DIM`/`elem_t`가 맞는지 반드시 확인할 것(JOURNAL E42).

## 회귀 테스트 (새 하드웨어를 처음 부팅할 때)

| 파일 | 용도 |
|---|---|
| `rocc_probe.c` | RoCC 명령을 한 단계씩 실행하며 표식 출력. **교착 시 어디서 멈췄는지 알려준다** |
| `stress_matmul.c` | `tiled_matmul_auto`(WS) 반복 검증. CPU 기준값 대조 포함 |
| `verified_encoder.c` | 트랜스포머 전 단계를 double 오라클로 검증 |

## 성능 측정

| 파일 | 용도 |
|---|---|
| `roofline.c` | N을 늘리며 matmul 사이클 측정 |
| `bwrate.c` | 고정 시간창 메모리 처리량 (**sudo 필요** — `mlockall`) |
| `convprof.c` | Gemmini 하드웨어 성능 카운터 16종 |
| `fuse.c`, `tilesweep.c`, `rwmix.c`, `bwsweep.c`, `bwone.c` | 보조 측정 |

## BF16 전용

| 파일 | 용도 |
|---|---|
| `bf16_verify.c` | BF16↔float 변환 + double 오라클. **번들 테스트는 BF16을 검증 못 한다**(E52) |
| `bf16_verify_full.c` | 위의 전폭(acc_t) 출력 변형 |
| `acc_probe.c` | 누산기 왕복만 수행해 값 손실을 확인 |

## 진단용 (정확성 조사, JOURNAL E82~E94)

`stress_*.c`, `count_*.c`, `matmul_pad.c`는 간헐 오답 조사 과정에서 만든 변형들이다.
**주의**: `count_os.c`/`count_ws.c`는 `exit(1)`을 카운터로 바꾼 것이라
**첫 실패 이후 연쇄 오류를 세므로 실패율로 읽으면 안 된다**(E94).

## 런타임 파라미터 라이브러리 (`include/gemmini_rt.h`)

Gemmini 기본 헤더 `gemmini_params.h`는 `DIM`·`elem_t`·opcode를 **컴파일 타임 상수**로 박고
CONFIG마다 덮어써진다. 그래서 구성이 다르면 바이너리를 다시 빌드해야 했고,
"같은 바이너리로 다른 비트스트림을 비교"하는 통제가 원천적으로 불가능했다.

`gemmini_rt.h`는 그 값들을 **구조체**로 받는다. `gemmini.h`/`gemmini_params.h`를 전혀
include하지 않고 `rocc-software/src/xcustom.h`만 쓴다.

```c
typedef struct { int dim; int elem_bytes; int acc_bytes; int opcode; } grt_ctx;
static const grt_ctx INT8 = { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 };
static const grt_ctx FP32 = { .dim= 8, .elem_bytes=4, .acc_bytes=4, .opcode=GRT_OP_FP32 };
```

지원 범위는 기본 WS matmul뿐이다(활성화·정규화·전치·풀링 없음). 검증과 측정이 목적이다.

**주의 — 인코딩은 반드시 원본 매크로와 대조할 것.** 기억이나 추측으로 옮기다 네 번 틀렸다
(E124 3건, E125 1건). 특히:
- `config_ld`의 스케일은 float 1.0의 비트패턴 `0x3f800000`이다. 정수 `1`을 넣으면
  denormal이 되어 mvin 결과가 전부 0이 된다.
- `config_ld`의 `block_mvin_stride`가 **DIM**이다.
- `GARBAGE_ADDR`는 `0xFFFFFFFF`다. `>> 1`을 붙이면 bit31이 지워져 하드웨어가
  "실재하는 거대한 스크래치패드 주소"로 해석하고 **시스템이 멎는다**.
- opcode는 `CAT(CUSTOM_, x)` 토큰 붙이기 때문에 **컴파일 타임 리터럴**이어야 한다.
  런타임 선택은 두 변형을 만들어 분기해야 하고, 각 분기를 중괄호로 감싸야 한다
  (`ROCC_INSTRUCTION_0_R_R`이 `{ asm ...; }`로 확장돼 `;`가 `else`를 끊는다).

## 이종/다중 가속기 테스트

| 파일 | 용도 |
|---|---|
| `rocc_core.c` | **구성 무관.** flush 하나만 모든 코어에서 시도해 코어별 RoCC 생존 확인. `rocc_core <로그> [2\|3]`로 opcode 선택. 어느 비트스트림에서도 재빌드 없이 돌아간다 |
| `bl_probe.c` | big.LITTLE 구성 검증 — 한 바이너리가 코어0에서 INT8→FP32→INT8을 번갈아 사용 |
| `bl_bench.c` | **같은 코어·같은 클럭·같은 코드**로 두 가속기 성능 비교(best-of-3×10) |
| `het_step.c` / `het_c1.c` / `het_split.c` | 이종 정지 지점을 명령 단위·코어별·명령종류별로 좁히는 진단 3종 |
| `het_watch.c` | 감시자 스레드를 다른 코어에 고정해 "하트 정지"와 "버스 막힘"을 가른다 |
| `het_unified.c` / `het_mm.c` | 한 바이너리로 두 코어의 서로 다른 가속기 구동(정확성/성능) |

### 타깃이 멎어도 진행 지점을 보는 법

rootfs가 NFS이므로 타깃에서 `/mnt2/tmp/*.log`에 쓰고 `fsync`하면 호스트의
`/srv/nfs/debian-riscv64/tmp/`에서 바로 읽힌다. **stdout은 쓸모없다** — ssh가 출력을
끝까지 모았다가 내보내므로 타깃이 멎으면 아무것도 안 나온다. 위 진단 도구들이 전부
이 방식을 쓴다. 이걸 도입하기 전까지 마크를 찍어 놓고도 하나도 못 보고 있었다.
