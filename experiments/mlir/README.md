# MLIR pass: 연산 그래프에서 Gemmini shape 고르기

`experiments/mlir/` 는 "연산 그래프를 보고 텐서 크기에 따라 shapeshift Gemmini 의 shape 를
고르는 MLIR pass" 의 **실현 가능성 조사**다. 효율보다 가능성이 먼저라는 전제로 정리했다.

## 결론부터

**가능하다. 그리고 어려운 쪽은 MLIR 이 아니다.**

pass 자체는 작다 — matmul op 에서 M, K, N 을 읽고 정수 몇 번 비교해 shape 인덱스를 attribute
로 붙이는 일이다. 실제 일은 그 인덱스를 **명령 스트림까지 내려보내는 것**이고, 이 프로젝트는
그 경로를 이미 만들어 뒀다:

- **ISA 자리**: `config_ex` 의 rs1[12:10]. `CONFIG_EX_RS1_SPACER1`([15:10]) 안이라 기존 필드가
  하나도 안 움직이고, shape 를 안 쓰는 프로그램은 0 을 읽어 안 접힌 모양이 된다 (E446).
- **하드웨어**: `ExecuteController` 가 그 필드를 latch 하고 `ShapeshiftMeshWithDelays` 가
  segment 별로 scratchpad bank 와 accumulator bank 를 고른다 (E446/E447/E452/E456).
  stock 명령 스트림으로 세 shape 가 다 돈다 (E455/E456).
- **런타임 라이브러리**: `experiments/tests/include/gemmini_rt.h` 가 `dim` 을 컴파일타임 매크로가
  아니라 **struct field** 로 들고 있다. 접힌 shape 는 tile 이 `h x (cols*S)` 라 정사각이 아니고,
  `gemmini_params.h` 의 `DIM` 은 컴파일타임 상수라 stock 라이브러리로는 표현이 안 된다.
  이미 있는 runtime-parameterized 경로가 pass 의 backend 로 맞는 물건이다.

## pass 가 하는 결정

`shape_select.py` 가 규칙이고, 맞춘 값이 아니라 **기하 + 측정된 제약 하나**다.

    S* = max { S : rows/S >= max(K, 4) }

- `rows/S >= K` — **정확성 제약이고, E462 에서 실측으로 확인했다.** 배열은 K = h 행의 가중치를
  들고 있고 이 프로젝트의 명령 경로는 K 를 쪼개지 않으므로, `h < K` 인 shape 는 K 행 중 h 행만
  조용히 쓴다. `(M,K,N)=(4,8,64)` 를 세 shape 로 풀어 결과를 맞대면 S=4(h=4)만 어긋나고
  S=2(h=8=K)는 맞는다. 이 절을 어기면 느려지는 게 아니라 **틀린 답이 나온다.**
- ~~`cols*S <= N`~~ — **E461 에서 삭제했다.** 노는 열이 *시간*을 쓴다고 보고 넣은 절인데,
  재 보니 아니었다: `(M,K,N)=(4,4,16)` 에서 S=4 가 64 열 중 48 열을 버리면서도 32 사이클로
  S=1 의 56 사이클을 이긴다. 노는 열은 accumulator bank 와 write bandwidth 를 쓰지 사이클을
  쓰지 않는다. 자원 제약으로 따로 판단할 일이지 shape 선택식에 넣을 것이 아니었다.
- `h >= 4` — 하드웨어 제약. `total_rows` 의 하한이 4 라서 segment 가 그보다 낮으면
  preload 가 0 을 잡는다 (E453/E455).

`shape_select.py` 실행 결과:

| M,K,N | 고른 S | h × width | 활용률(S*) | 활용률(S=1) |
|---|---|---|---|---|
| 16,16,16 | 1 | 16×16 | 100% | 100% |
| 16,8,32 | 2 | 8×32 | 100% | 50% |
| 16,4,64 | 4 | 4×64 | 100% | 25% |
| 128,768,768 (BERT proj) | 1 | 16×16 | 100% | 100% |
| 1,4096,4096 (GEMV decode) | 1 | 16×16 | 100% | 100% |
| 16,4,16 | 1 | 16×16 | 25% | 25% |

읽는 법 두 가지:
- 위 세 줄은 E456 에서 실제로 돌린 세 shape 와 정확히 같은 선택이다.
- BERT·GEMV 는 K 가 커서 어떤 shape 로도 `h >= K` 가 안 되므로 S=1 을 고른다 — E428/E438 이
  "K 가 깊으면 접어도 이득이 없다"고 측정한 것과 같다. **접기는 K 가 얕고 N 이 넓을 때만 산다.**
- 마지막 줄이 한계다: K=4 로 얕지만 N=16 이라 넓힐 곳이 없어 접어도 25% 그대로다.

## 어디에 꽂히나

    linalg (static shape)
        |
        +-- [gemmini-select-shape]  <- 이 pass. op 에 gemmini.shape attribute 를 붙인다
        |
        +-- tiling / bufferize
        |
        +-- lowering to Gemmini ops  <- attribute 를 읽어 config_ex rs1[12:10] 에 싣는다
        |
        +-- RoCC intrinsic / inline asm

동적 shape 면 컴파일타임에 못 고르지만 **런타임 산술로 고르면 된다** — shape 필드는 rs1 의
비트일 뿐이고 rs1 은 레지스터라, `memref.dim` 으로 K, N 을 읽어 `arith`/`scf` 몇 개로 인덱스를
계산해 넣으면 된다. `example.mlir` 에 두 경우를 다 적어 뒀다.

## 아직 안 된 것 (정직하게)

1. **이 머신에 LLVM/MLIR 이 없다.** 그래서 pass 를 실제로 빌드·실행한 적이 없다.
   `shape_select.py` 는 결정 로직만 따로 뽑아 검증한 것이고, `example.mlir` 은 손으로 쓴 예시다.
2. **Gemmini dialect 에 shape 필드가 없다.** buddy-mlir 의 Gemmini dialect 를 쓴다면
   `gemmini.config_ex` 에 operand 나 attribute 를 하나 추가하고 lowering 을 고쳐야 한다.
   (그 dialect 의 현재 op 목록은 확인하지 않았다 — 인터넷 접근 없이 기억으로 쓰지 않는다.)
3. ~~머리 대 머리 측정이 없다~~ **-> E460 에서 했다.** 같은 `(M=4, K=4, N=64)` 를 세 shape 로
   돌린 결과 **104 / 48 / 32 사이클**이고, 규칙이 고르는 S=4 가 실제로 가장 빠르다 (3.25배).
   (E463 이 그 계산이 M 행 중 하나만 내놓고 있었다는 것을 찾았고 E464 가 고쳤다 — `c_stride`
   가 rs2[63:48] 인데 하네스가 [15:0] 에 넣고 있었다. 사이클은 그대로였고, 이제 **절대적으로
   옳은 계산**의 시간이다: shape 0 이 Scala golden 과 일치하고 세 shape 가 서로 일치한다.)
   남은 것은 규칙의 *경계* — 접기가 이득을 잃는 지점 — 를 재는 것이다. 한 점만 쟀다.
4. **라이브러리 경로.** stock `tiled_matmul_auto` 는 정사각 `DIM` 을 가정하므로 접힌 shape 를
   그대로 못 쓴다. pass 의 backend 는 `gemmini_rt.h` 계열이어야 한다.
