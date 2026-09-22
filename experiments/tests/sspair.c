// E644: SSGemm 대 **기본 Gemmini** 비교쌍. 이 트랙의 모든 이득은 shapeshift 비트스트림
// **안에서** shape0 대 shape2 를 비교한 것이고, 기본 Gemmini 와의 비교는 없었다.
//
//   ./sspair <mode> <K> <N> <M> <T> [--neg]
//     mode ws  : 기본 loop_ws 경로 — **양쪽 비트스트림에서 모두 돈다**. 같은 바이너리로
//                두 보드를 재면 "shapeshift 가 기본 경로에 지우는 비용"이 나온다.
//     mode ss  : 접힌 FSM 경로 (shapeshift 비트스트림 전용).
//                **기본 비트스트림에서 절대 돌리지 말 것** — 없는 funct 는 hang 이다.
//
// 두 모드 모두 CPU 참값과 대조한다. M 은 논리 행 수이고, loop_ws 는 16 행으로 패딩한다
// (그것이 접기가 노리는 낭비 그 자체다).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include "include/gemmini_rt.h"

#define DIM 16
#ifdef SPB
static const int SP_BANKS = SPB, SP_ENT = 16384 / SPB;
#else
static int SP_BANKS = 8, SP_ENT = 2048;
#endif

static const grt_ctx CTX = { DIM, 1, 4, GRT_OP_INT8 };
static inline uint64_t rdt(void) { uint64_t t; asm volatile("rdtime %0" : "=r"(t)); return t; }

#ifndef KMAX
#define KMAX 768
#endif
#ifndef NMAX
#define NMAX 768
#endif
static int K = 128, N = 128, M = 1;

#ifndef NROT
#define NROT 16
#endif                     /* E663: 레이어 회전용 가중치 사본 (Wrot/Wtm) */
/* E742: Bpre 는 shape 마다 세그당 행 수가 달라 nrot 이 달라지고, 그러면 **회전
 * 작업집합이 shape 마다 달라진다** — KMAX=512 에서 S=2 는 512 KB(=L2 경계), S=4 는
 * 1024 KB 였다. shape 비교의 심각한 혼입이므로 Bpre 를 넉넉히 잡고 `FORCE_NROT` 로
 * 사본 수를 통일할 수 있게 한다. */
#ifndef BPRE_ROWS
/* E779: 65536 이면 `[64x64]` 에서 `FORCE_NROT=96` 이 **조용히 무시**되어 ss 팔만 256 KB 로
 * 돌았다 (경고는 찍혔으나 읽지 않았다 — E742 의 재발). 2 MB 로 올려 96 사본을 담는다. */
#define BPRE_ROWS 131072
#endif
static int8_t A[16 * KMAX], W[KMAX * NMAX], C[16 * NMAX];
static int8_t Wrot[NROT][KMAX * NMAX];
static int8_t Wtm[NROT][KMAX * NMAX];   /* E664: 기본용 타일-major 재배열 (연속 읽기) */
static int pre_stock = 0;
static int rot_rows = 0;   /* E663: Bpre 안에서 회전할 세그먼트당 행 수 */
static int nrot = NROT;    /* E670: 크기에 맞춰 사본 수를 줄인다 (Bpre 한계) */
#ifdef SS_PAD
/* E785: 의미상 아무 것도 하지 않고 **뒤따르는 정적 심볼의 주소만 옮긴다**.
 * "바이너리 효과" 가 소스 차이 때문인지 배치 때문인지 가르는 탐침이다 (E312 의 설정). */
__attribute__((used)) static int8_t ss_pad[1 << 20];
#endif
static int rot = 0;
static int wide = 0;       /* E675: 4-타일 wide mvin (명령 1/4) */
static int wide_stock = 0; /* E768: 기본 팔의 유효 폭 — 조건이 ss 팔과 다르다 */
static int plan_wide = 0;  /* E776: `--wide` 를 손으로 주는 대신 계획기의 use_wide 를 따른다 */
/* E837c: 이 **비트스트림**이 지원하는 최대 shape 인덱스 (0/1/2 = S 1/2/4).
 * `max_segments=2` 보드에 shape 2 를 발행하면 aliasing 되어 조용히 틀린다 (E587).
 * 하드웨어를 소프트웨어가 알아낼 방법이 없으므로 `SS_MAX_SH` 로 받는다.
 * 런타임 변수여도 안전하다 — 계획기는 측정 준비 때 한 번 불리고 **발행 루프에는
 * 안 들어간다** (E595 의 `SP_BANKS` 와 다른 점이 이것이다). */
static int max_sh = 2;
static int noexec = 0;     /* E677: 적재만 — 계산을 빼서 두 항을 가른다 */
static int segint = 0;     /* E720: 세그먼트 인터리브 적재 (DRAM 스트림 1 개) */
static int catlay = 0;     /* E721: (pp,sg,r) 연속 배치 — 스트림만 1 개로 */
static int sgouter = 0;    /* E722: 세그먼트를 바깥 루프로 — 뱅크 전환만 줄인다 */
static int bank1 = 0;      /* E726: 적재 목적지를 한 뱅크로 (noexec 전용 격리) */
static int src0 = 0;       /* E728: 모든 세그먼트가 Bpre[0] 을 읽는다 (noexec 전용) */
static int poison = 0;     /* E723: 안 쓰이는 회전 사본을 오염 — 주소 버그 검출.
                            * **결함 있음, 쓰지 말 것**: T=1 (깨끗한 사본만 사용) 에서도
                            * FAIL 한다. 대조군을 통과하도록 고친 뒤에 쓸 것. */
static int32_t ref[16 * NMAX];
static int8_t Bpre[4][BPRE_ROWS * 16];

static void prearrange(int sh) {
  const int S = 1 << sh, h = DIM >> sh, w = DIM * S, ks = h < 4 ? 4 : h;
  const int nt = N / w, ch = K / h;
  for (int s = 0; s < S; s++)
    for (int pp = 0; pp < nt; pp++)
      for (int tt = 0; tt < ch; tt++)
        for (int r = 0; r < h; r++)
          memcpy(&Bpre[s][((pp * ch + tt) * ks + r) * 16],
                 W + (size_t)(tt * h + r) * N + pp * w + s * DIM, 16);
}
/* E675: 4-타일 wide mvin 을 쓰려면 DRAM 쪽 순서를 바꿔야 한다.
 *
 * `config_ld` 의 block_mvin_stride 가 DIM(=16)이므로, cols=64·rows=16 짜리 mvin 하나는
 * DRAM 의 16x64 블록을 읽어 타일 t 를 scratchpad 행 `sp + 16t + r` 에 놓는다. 즉
 *
 *     DRAM (r, 16t+j)  ->  sp 행 (sp + 16t + r), 열 j
 *
 * 우리는 sp 행 n 에 Bpre 행 n 이 오길 원하므로 `n = 16t + r`, 곧 DRAM 오프셋
 * `r*64 + 16t` 에 Bpre 행 n 을 두면 된다 (DRAM 행 간격은 64 B). 64 행씩 묶어 제자리
 * 치환한다. 오프라인 재배열이므로 런타임 비용은 0 이다 (E603 과 같은 성격). */
#define WGRP 64
/* E720: 세그먼트 인터리브 배치. 폭 S*16 짜리 mvin 하나가 16 행 x S 세그먼트를 채우도록
 * DRAM 을 (행 r, 세그 t, 열 j) 순으로 깐다. block_stride = 뱅크 크기이므로 타일 t 는
 * 뱅크 (base+t) 의 같은 행으로 간다. Bpre[0] 에 이어붙여 쓴다 (세그먼트 버퍼는 S<=4 라
 * Bpre[1..3] 이 남는다 — 인터리브 판에서는 안 쓰므로 그 공간을 빌린다). */
static int8_t *Bseg;
static void interleave(int sh, long rows_per_seg) {
  const int S = 1 << sh;
  Bseg = Bpre[0];                      /* 아래에서 덮어쓰기 전에 원본을 tmp 로 옮긴다 */
  static int8_t tmp[4][1 << 19];
  for (int t = 0; t < S; t++) memcpy(tmp[t], Bpre[t], (size_t)rows_per_seg * 16);
  for (long g = 0; g < rows_per_seg / 16; g++)
    for (int r = 0; r < 16; r++)
      for (int t = 0; t < S; t++)
        memcpy(&Bseg[((g * 16 + r) * S + t) * 16], &tmp[t][(g * 16 + r) * 16], 16);
}
static void widen(int sh, long rows_total) {
  const int S = 1 << sh;
  static int8_t tmp[WGRP * 16];
  for (int sg = 0; sg < S; sg++)
    for (long off = 0; off + WGRP <= rows_total; off += WGRP) {
      memcpy(tmp, &Bpre[sg][off * 16], sizeof tmp);
      for (int n = 0; n < WGRP; n++) {
        const int t = n / 16, r = n % 16;
        memcpy(&Bpre[sg][(off + r * 4 + t) * 16], &tmp[n * 16], 16);
      }
    }
}
/* E675: 기본 타일-major 버퍼에도 같은 치환. 목적지 sp 행이 tt 를 따라 연속이므로
 * 64 행 묶음 = 타일 4 개이고, 접힌 쪽과 **명령 수가 같아진다** (공정 비교). */
static void widen_stock(long rows_total) {
  static int8_t tmp[WGRP * 16];
  for (int q = 0; q < NROT; q++)
    for (long off = 0; off + WGRP <= rows_total; off += WGRP) {
      memcpy(tmp, &Wtm[q][off * 16], sizeof tmp);
      for (int n = 0; n < WGRP; n++) {
        const int t = n / 16, r = n % 16;
        memcpy(&Wtm[q][(off + r * 4 + t) * 16], &tmp[n * 16], 16);
      }
    }
}
static int SSB_ = 8, SSE_ = 2048;
static void load_b(int sh, int stride) {
  const int S = 1 << sh, h = DIM >> sh, w = DIM * S, ks = h < 4 ? 4 : h;
  const int rows = (N / w) * (K / h) * ks;
  grt_config_ld(&CTX, 16);
  for (int s = 0; s < S; s++)
    for (int r0 = 0; r0 < rows; r0 += DIM) {
      const int nb = rows - r0 > DIM ? DIM : rows - r0;
      grt_mvin(&CTX, &Bpre[s][r0 * 16],
               (uint32_t)(((1 + s * stride) % SSB_) * SSE_ + r0), DIM, nb);
    }
}
static void load_a(int sh, int abase) {
  const int h = DIM >> sh, ch = K / h;
  grt_config_ld(&CTX, K);
  for (int tt = 0; tt < ch; tt++)
    grt_mvin(&CTX, A + tt * h, (uint32_t)(abase + tt * M), h, M);
}

int main(int argc, char **argv) {
  if (argc < 6) { fprintf(stderr, "usage: %s <ws|tile|ss|plan> K N M T [--neg]\n", argv[0]); return 2; }
  int ss = !strcmp(argv[1], "ss");
  /* E695: `ws` 는 ss/tile/plan 이 모두 아닐 때의 분기이므로 **`ws2` 는 코드 없이 이미
   * 동일-코드 별칭이다** (md5 가 안 바뀌는 것으로 확인). 별칭을 만들려고 코드를 더할
   * 필요가 없었다 — 더한 줄은 컴파일러가 지웠다. */
  /* E694: `tile2`/`plan2` 는 각각 `tile`/`plan` 의 **완전한 별칭**이다 — 같은 코드를 다른
   * argv 로 부르는 동일-코드 대조군. E693 이 argv 만 달라도 6.7 % 다른 모드에 앉는 것을
   * 보였으므로, A/B 비교마다 잡음 바닥을 **같은 실행 안에서** 재기 위해 넣는다. */
  int tile = !strcmp(argv[1], "tile") || !strcmp(argv[1], "tile2");
  /* E651: `plan` 모드 — 계획기를 **그대로 따른다**. `use_fsm=0` 이면 타일 경로로 떨어진다.
   * 이것이 "SSGemm 을 실제로 쓸 때" 의 성능이고, 어느 M 에서도 기본보다 느리면 안 된다. */
  int plan_mode = !strcmp(argv[1], "plan") || !strcmp(argv[1], "plan2");
  K = atoi(argv[2]); N = atoi(argv[3]); M = atoi(argv[4]);
  const long T = atol(argv[5]);
  /* E658 수정: 플래그를 argv[6] 에서만 찾으면 `--ovl --neg` 처럼 두 개를 주었을 때
   * **음성 대조가 조용히 꺼진다**. 실제로 그렇게 한 번 재고 PASS 를 받았다. 전부 훑는다. */
  int neg = 0, with_load = 0, ovl = 0, amort = 0;
  for (int ai = 6; ai < argc; ai++) {
    if (!strcmp(argv[ai], "--neg")) neg = 1;
    else if (!strcmp(argv[ai], "--load")) with_load = 1;
    else if (!strcmp(argv[ai], "--ovl")) { with_load = 1; ovl = 1; }
    else if (!strcmp(argv[ai], "--ovlc")) { with_load = 1; ovl = 2; }  /* E659: 거친 입도 */
    else if (!strcmp(argv[ai], "--amort")) amort = 1;   /* E661: 적재 1 회 + T 토큰 */
    else if (!strcmp(argv[ai], "--rot")) rot = 1;      /* E663: 레이어 회전 */
    else if (!strcmp(argv[ai], "--wide")) wide = wide_stock = 1;  /* E675: 4-타일 mvin */
    else if (!strcmp(argv[ai], "--noexec")) noexec = 1; /* E677: 적재만 */
    else if (!strcmp(argv[ai], "--seg")) { wide = 1; segint = 1; } /* E720 */
    else if (!strcmp(argv[ai], "--cat")) { wide = 1; catlay = 1; } /* E721 */
    else if (!strcmp(argv[ai], "--sgo")) { wide = 1; sgouter = 1; } /* E722: 적재 전용 */
    else if (!strcmp(argv[ai], "--poison")) poison = 1; /* E723 */
    else if (!strcmp(argv[ai], "--bank1")) { wide = 1; bank1 = 1; } /* E726: noexec 전용 */
    else if (!strcmp(argv[ai], "--b1sq")) { wide = 1; bank1 = 1; sgouter = 1; } /* E727 */
    else if (!strcmp(argv[ai], "--b1s0")) { wide = 1; bank1 = 1; sgouter = 1; src0 = 1; } /* E728 */
    else if (!strcmp(argv[ai], "--pre")) pre_stock = 1; /* E664: 기본도 재배열 */
    else if (!strcmp(argv[ai], "--pw")) plan_wide = 1;  /* E776: 폭을 계획기가 정한다 */
    else if (!strcmp(argv[ai], "--pipe")) { with_load = 1; ovl = 3; }  /* E815: 소프트웨어 파이프라인 */
  }
  /* E654: 가중치 적재를 **타이밍 안에** 넣는다. 레이어가 scratchpad 에 안 들어가면
   * 토큰마다 다시 실어야 하므로, 그때의 실제 성능이 이것이다. */

  /* E658: 적재와 계산을 **번갈아** 발행한다 (E609 의 4 타일 입도). 겹치면 시간이
   * load+compute 가 아니라 max(load, compute) 에 가까워진다. */

  if (K > KMAX || N > NMAX || M < 1 || M > 16) { fprintf(stderr, "범위 초과\n"); return 2; }
#ifndef SPB
  { const char *e = getenv("SP_BANKS"); if (e) SP_BANKS = atoi(e); SP_ENT = 16384 / SP_BANKS; }
  { const char *e = getenv("SS_MAX_SH");
    if (e) { max_sh = atoi(e); if (max_sh < 0) max_sh = 0; if (max_sh > 2) max_sh = 2;
             fprintf(stderr, "SS_MAX_SH=%d — 이 보드가 지원하는 최대 shape\n", max_sh); } }
#endif
  /* E776: `use_wide` 는 계획기가 계산해 놓고도 **아무도 안 쓰던 필드**였다 — 기록에는
   * "계획기가 폭을 정한다" 고 적혀 있는데 측정은 내내 `--wide` 를 손으로 줬다.
   * 여기서 잇는다. 계획기 호출은 순수 함수이므로 두 번 불러도 무해하고, `widen_stock()`
   * 보다 **앞**이어야 한다 (그 함수가 폭에 따라 배열을 섞기 때문). */
  if (plan_wide) {
    grt_ss_plan_t pw; grt_ss_plan_hw(&pw, M, K, N, 8, 16384 / 8, with_load, max_sh);
    wide = wide_stock = pw.use_wide;
    fprintf(stderr, "[--pw] 계획기가 정한 폭: %d (use_wide=%d, shape=%d)\n",
            pw.use_wide ? 4 : 1, pw.use_wide, pw.shape);
  }
  if (mlockall(MCL_CURRENT | MCL_FUTURE)) { perror("mlockall"); return 2; }
  grt_flush_ctx(&CTX);

  for (int i = 0; i < 16; i++) for (int k = 0; k < K; k++)
    A[i * K + k] = (i < M) ? (int8_t)((i + 3 * k) % 5 - 2) : 0;
  for (int k = 0; k < K; k++) for (int n = 0; n < N; n++)
    W[(size_t)k * N + n] = (int8_t)((k + 2 * n) % 7 - 3);
  for (int i = 0; i < M; i++)
    for (int n = 0; n < N; n++) {
      int32_t s = 0;
      for (int k = 0; k < K; k++) s += (int32_t)A[i * K + k] * W[(size_t)k * N + n];
      ref[i * N + n] = s > 127 ? 127 : s < -128 ? -128 : s;
    }
  if (neg) ref[(M / 2) * N + N / 2] ^= 1;
  if (rot) {
    /* E670: Wrot[q] 는 각각 독립된 KMAX*NMAX 버퍼이므로 사본 수 제약이 없다. 제약은
     * SSGemm 의 Bpre(세그먼트 버퍼 안에서 오프셋으로 회전)뿐이므로 거기에 맞춘다. */
    const int rows_ss = (K * N) / 64;                       /* shape 2 의 세그먼트당 행 */
    nrot = (int)(((long)KMAX * NMAX) / ((long)rows_ss * 16));
    if (nrot > NROT) nrot = NROT;
    /* E743: **두 팔의 회전 사본 수를 같게 맞추는 지점.** ws/tile 팔과 ss 팔이 각자 자기
     * 배열 크기로 nrot 을 정하던 것이 E742 의 혼입 원인이었다. FORCE_NROT 은 양쪽에 건다. */
    { const char *e = getenv("FORCE_NROT");
      if (e) { const int f = atoi(e);
        if (f >= 2 && f <= NROT) nrot = f;
        else fprintf(stderr, "FORCE_NROT=%d 무시 (ws 팔 최대 %d)\n", f, NROT); } }
    if (nrot < 2) { fprintf(stderr, "회전 사본 부족\n"); return 2; }
    fprintf(stderr, "[ws/tile 팔] nrot=%d, 회전 작업집합 %ld KB%s\n", nrot,
            (long)nrot*K*N/1024, ((long)nrot*K*N <= 524288L) ? "  **<= L2 512 KB**" : "");
    for (int q = 0; q < nrot; q++) memcpy(Wrot[q], W, (size_t)K * N);
  }  /* 같은 값, 다른 주소 */
  if (pre_stock) {                    /* E664: (pp,tt) 타일을 연속으로 재배열 (오프라인 상당) */
    const int nt0 = N / DIM, ch0 = K / DIM;
    for (int pp = 0; pp < nt0; pp++)
      for (int tt = 0; tt < ch0; tt++)
        for (int r = 0; r < DIM; r++)
          memcpy(&Wtm[0][((pp * ch0 + tt) * DIM + r) * DIM],
                 W + (size_t)(tt * DIM + r) * N + pp * DIM, DIM);
    for (int q = 1; q < NROT; q++) memcpy(Wtm[q], Wtm[0], (size_t)K * N);
    /* E768: 기본 팔의 wide 는 pp 블록 안에서 **4 타일씩** 끊으므로 `ch = K/DIM` 이 4 의
     * 배수여야 한다 (= K % 64 == 0). 총 행 수를 보던 옛 가드는 E715 와 **같은 버그**이고,
     * [96x96] 은 ch=6 이라 마지막 2 타일이 좁은 경로로 떨어지는데 배열은 이미 섞여 있어
     * **조용히 틀린 답**(bad=96)을 냈다. 못 쓰면 중단하지 말고 좁은 경로로 되돌린다. */
    if (wide && (K / DIM) % (WGRP / DIM)) {
      fprintf(stderr, "wide(stock): ch=%d 가 %d 의 배수가 아님 — 폭 1 로 되돌림\n",
              K / DIM, WGRP / DIM);
      wide_stock = 0;
    }
    if (wide_stock) widen_stock((long)(K / DIM) * (N / DIM) * DIM);
  }

  if (plan_mode) {
    /* E702: 적재가 타이밍 안에 있으면(= 스트리밍) 그 체제의 규칙으로 고른다. */
    grt_ss_plan_t p0; grt_ss_plan_hw(&p0, M, K, N, 8, 16384 / 8, with_load, max_sh);
    if (p0.use_fsm && !p0.risky) ss = 1;   /* FSM 경로로 */
    else tile = 1;                          /* 타일 경로로 떨어진다 */
  }
  double cyc; int bad = 0;
  if (tile) {
    /* ---- 기본 preload/compute 타일 경로, B 를 미리 상주 (양쪽 비트스트림에서 돎) ----
     * 접힌 FSM 과 **데이터 이동 조건을 맞추기 위한** 축이다: 둘 다 B 가 scratchpad 에 있고
     * 타이밍 루프 안에는 계산만 있다. 다른 점은 접힘 유무뿐. */
    /* E644: **평탄 주소만 쓴다** — 뱅크 수(보드마다 4 vs 8)를 참조하지 않으므로 한 바이너리가
     * 양쪽에서 같은 코드로 돈다. A 는 평탄 0.., B 는 평탄 4096.. (4 뱅크 보드에서는 뱅크 1,
     * 8 뱅크 보드에서는 뱅크 2 — 어느 쪽이든 A 와 다른 뱅크다). 런타임 SP_BANKS 를 쓰면
     * 발행 루프의 주소 계산이 안 접혀 **기준선 경로만** 20~26 % 느려진다(E598). */
    const int nt = N / DIM, ch = K / DIM;
    const uint32_t BFLAT = 4096;
    grt_config_ex(&CTX, GRT_WS);
    grt_config_st(&CTX, N);
    grt_config_ld(&CTX, N);
    for (int pp = 0; pp < nt; pp++)
      for (int tt = 0; tt < ch; tt++)
        grt_mvin(&CTX, W + (size_t)(tt * DIM) * N + pp * DIM,
                 (uint32_t)(BFLAT + (pp * ch + tt) * DIM), DIM, DIM);
    grt_config_ld(&CTX, K);
    for (int tt = 0; tt < ch; tt++)
      grt_mvin(&CTX, A + tt * DIM, (uint32_t)(tt * DIM), DIM, M);
    grt_fence();
    memset(C, 0, sizeof C);
    const uint64_t t0 = rdt();
    if (amort) {                            /* E661: 적재는 타이밍 안에서 딱 한 번 */
      grt_config_ld(&CTX, N);
      for (int pp2 = 0; pp2 < nt; pp2++)
        for (int tt2 = 0; tt2 < ch; tt2++)
          grt_mvin(&CTX, W + (size_t)(tt2 * DIM) * N + pp2 * DIM,
                   (uint32_t)(BFLAT + (pp2 * ch + tt2) * DIM), DIM, DIM);
    }
    for (long it = 0; it < T; it++) {
      if (with_load && !ovl) {               /* E654: 적재를 먼저 다 한다 (겹침 없음) */
        grt_config_ld(&CTX, N);
        for (int pp2 = 0; pp2 < nt; pp2++)
          for (int tt2 = 0; tt2 < ch; tt2++)
            grt_mvin(&CTX, W + (size_t)(tt2 * DIM) * N + pp2 * DIM,
                     (uint32_t)(BFLAT + (pp2 * ch + tt2) * DIM), DIM, DIM);
      }
      if (ovl) {                             /* E658: G 타일씩 적재하고 바로 계산 */
        const int G = (ovl == 2) ? ch : 4;   /* E659: ovlc 는 pp 당 한 묶음 = SSGemm 과 같은 입도 */
        const int ridx = rot ? (int)(it % nrot) : 0;   /* E672: 나눗셈을 루프 밖으로 */
        for (int pp = 0; pp < nt; pp++) {
          for (int t0 = 0; t0 < ch; t0 += G) {
            const int te = t0 + G > ch ? ch : t0 + G;
            grt_config_ld(&CTX, N);
            if (pre_stock) {          /* 연속 읽기: config_ld stride 를 DIM 으로 */
              const int8_t *Ws = Wtm[ridx];
              if (wide_stock && te - t0 == 4) {   /* E675: 타일 4 개를 명령 하나로 */
                grt_config_ld(&CTX, WGRP);
                grt_mvin(&CTX, Ws + (size_t)(pp * ch + t0) * DIM * DIM,
                         (uint32_t)(BFLAT + (pp * ch + t0) * DIM), WGRP, DIM);
              } else {
              grt_config_ld(&CTX, DIM);
              for (int tt = t0; tt < te; tt++)
                grt_mvin(&CTX, Ws + (size_t)(pp * ch + tt) * DIM * DIM,
                         (uint32_t)(BFLAT + (pp * ch + tt) * DIM), DIM, DIM);
              }
              grt_config_ld(&CTX, N);
            } else {
              const int8_t *Wsrc = rot ? Wrot[ridx] : W;
              for (int tt = t0; tt < te; tt++)
                grt_mvin(&CTX, Wsrc + (size_t)(tt * DIM) * N + pp * DIM,
                         (uint32_t)(BFLAT + (pp * ch + tt) * DIM), DIM, DIM);
            }
            if (!noexec)
            for (int tt = t0; tt < te; tt++) {
              const uint32_t bsp = BFLAT + (pp * ch + tt) * DIM;
              const uint32_t cdst = (tt == 0 ? GRT_ACC(pp * M) : GRT_ACC_ACC(pp * M));
              grt_preload(&CTX, bsp, cdst, DIM, DIM, DIM, M);
              grt_compute(&CTX, (uint32_t)(tt * DIM), GRT_GARBAGE, DIM, M, 0, 0);
            }
          }
          if (!noexec) grt_mvout(&CTX, C + pp * DIM, GRT_ACC(pp * M), DIM, M);
        }
        continue;
      }
      for (int pp = 0; pp < nt; pp++) {
        for (int tt = 0; tt < ch; tt++) {
          const uint32_t bsp = BFLAT + (pp * ch + tt) * DIM;
          const uint32_t cdst = (tt == 0 ? GRT_ACC(pp * M) : GRT_ACC_ACC(pp * M));
          grt_preload(&CTX, bsp, cdst, DIM, DIM, DIM, M);
          grt_compute(&CTX, (uint32_t)(tt * DIM), GRT_GARBAGE, DIM, M, 0, 0);
        }
        grt_mvout(&CTX, C + pp * DIM, GRT_ACC(pp * M), DIM, M);
      }
    }
    grt_fence();
    cyc = (double)(rdt() - t0) * 100.0 / (amort ? 1.0 : (double)T);
  } else if (!ss) {
    /* ---- 기본 loop_ws: 양쪽 비트스트림에서 도는 경로 ---- */
    /* E696: J = N/DIM 을 그대로 쓰면 N=512 에서 (I,J) = (1,32) 가 되고, 스크래치패드는
     * `I*Kt + Kt*J` = 528 타일이 된다. CLAUDE.md 의 "`I*J` 가 32 이고 `I*K + K*J` 가 ~500 을
     * 넘으면 그 모양을 반드시 확인하라" 에 정확히 걸리는 자리이고, 실제로 **보드가 wedge
     * 한다** ([256x512] 에서 결정적으로 재현; [128x512] 는 264 타일이라 통과한다).
     * J 를 16 타일씩 끊어 여러 번 부른다 — 총 일도 바이트도 같다. */
    const int I = 1, Kt = K / DIM;
    const int J = N / DIM;
    /* E697: J 를 끊는 것만으로는 부족하다 — `[512x256]` 은 (1,16) 이라 `I*J = 16` 인데도
     * `Kt*(I+J)` = 32*17 = **544 타일**로 예산(512)을 넘어 **bad=256 으로 틀린다**.
     * 정확성 검사가 잡았다. 스크래치패드 조건 `Kt*(I+JMAX) <= 512` 를 직접 풀어 고른다. */
    int JMAX = J < 16 ? J : 16;
    while (JMAX > 1 && (Kt * (I + JMAX) > 512 || J % JMAX)) JMAX--;   /* 예산 + 나누어떨어짐 */
    grt_loop_ws_config(&CTX, K, N, N);
    memset(C, 0, sizeof C);
    const uint64_t t0 = rdt();
    for (long it = 0; it < T; it++)
      { const int8_t *Bs = rot ? Wrot[it % nrot] : W;
        for (int j0 = 0; j0 < J; j0 += JMAX) {
          const int jn = J - j0 > JMAX ? JMAX : J - j0;
          grt_loop_ws(&CTX, I, jn, Kt, A, Bs + (size_t)j0 * DIM, C + (size_t)j0 * DIM, K, N, N);
        } }
    grt_fence();
    cyc = (double)(rdt() - t0) * 100.0 / (amort ? 1.0 : (double)T);
  } else {
    /* ---- SSGemm: **계획기가 고르는 shape** 을 쓴다 (E638). 즉 "실제로 쓸 때" 를 잰다.
     * M 이 커지면 계획기가 안 접는 shape 0 을 고르고, 그때는 기본과 같아야 한다. ---- */
    /* E703: **여기가 실제 shape 을 정하는 곳이다.** 191 행의 `p0` 는 FSM/타일 경로 선택에만
     * 쓰이므로 그쪽만 고쳤을 때는 아무 것도 안 바뀌었다 — 계획기가 두 군데서 불린다. */
    grt_ss_plan_t pl; grt_ss_plan_hw(&pl, M, K, N, 8, 16384 / 8, with_load, max_sh);
    /* E700: shape 을 강제해 계획기의 선택을 채점한다. 없으면 계획기를 그대로 따른다. */
    int sh = pl.shape;
    { const char *e = getenv("FORCE_SHAPE"); if (e) sh = atoi(e); }
    const int S = 1 << sh, h = DIM >> sh, w = DIM * S, ks = h < 4 ? 4 : h;
    if (N % w || K % h) { fprintf(stderr, "shape 2 에 안 맞음\n"); return 2; }
    const int SSB = 8, SSE = 16384 / 8;      /* ss 는 shapeshift 보드 전용 (8 뱅크) */
    const int need = (N / w) * (K / h) * ks, cap = SSB / S;
    int st = 1; while ((long)st * SSE < need && st < cap) st *= 2;
    if ((long)need > (long)st * SSE) { fprintf(stderr, "상주 불가\n"); return 2; }
    int sl = 0; while ((1 << sl) < st) sl++;
    /* E741: stride 를 강제해 **같은 K·N·같은 shape 에서 뱅크 간격만** 바꾼다. 계획기가 고른
     * 최소 stride 보다 크게만 줄 수 있다 (작으면 상주 불가). 세그먼트가 겹치지 않는 한 합법. */
    { const char *e = getenv("FORCE_STRIDE");
      if (e) { const int f = atoi(e);
        if (f >= st && f * S <= SSB) { st = f; sl = 0; while ((1 << sl) < st) sl++; }
        else fprintf(stderr, "FORCE_STRIDE=%d 무시 (최소 %d, 최대 %d)\n", f, st, SSB / S); } }
    const int abase = SSE - (K / h) * M;
    if (abase < 0) { fprintf(stderr, "A 자리 없음\n"); return 2; }
    /* E733: FORCE_SHAPE 는 계획기의 용량/넘침 검사를 우회한다. 마지막 세그먼트가
     * 스크래치패드를 넘어 **감아 도는지**, 그 넘침이 A 를 침범하는지 여기서 계산해
     * 알린다 — 강제 구성의 FAIL 을 "하드웨어 결함" 으로 오독하지 않기 위해서다
     * ([384x448] 강제 shape 2 가 그 사례였다, E732). */
    { const int nb2 = (need + SSE - 1) / SSE;
      const int lastbank = (1 + (S - 1) * st) % SSB;
      const long top = (long)lastbank * SSE + need;
      int hits0 = 0;
      for (int sg2 = 0; sg2 < S; sg2++)
        for (int j2 = 0; j2 < nb2; j2++)
          if (((1 + sg2 * st + j2) % SSB) == 0) hits0 = 1;
      const int spill = (hits0 && nb2 > 1) ? (need - (nb2 - 1) * SSE) : 0;
      if (top > (long)SSB * SSE || spill > abase)
        fprintf(stderr, "주의: 강제 구성이 계획기 제약 밖이다 — 세그당 %d 행, stride %d, "
                "마지막 뱅크 %d, 최상단 %ld (한계 %d), 넘침 %d, A 시작 %d\n",
                need, st, lastbank, top, SSB * SSE, spill, abase);
    }
    prearrange(sh);
    { const int rows_all = (N / w) * (K / h) * ks;
      rot_rows = rows_all;
      if (rot) {                          /* E663/E670: 같은 값, 다른 주소로 nrot 벌 */
        nrot = (int)((long)BPRE_ROWS / (long)rows_all);
        /* E781: 이 상한은 **자동값에만** 걸어야 한다. 예전에는 `FORCE_NROT` 적용 *뒤*에
         * 있어서, 명시적으로 준 96 을 **경고 없이 64 로 덮어썼다** — `FORCE_NROT` 은 E742 의
         * 혼입(두 팔의 작업집합 불일치)을 막으려고 넣은 장치인데, 그 장치를 무력화하는
         * clamp 가 세 줄 아래 있었다. 그래서 `[64x64]`·`[128x128]` 처럼 사본이 많이 필요한
         * 셀에서 ss 팔만 작업집합이 3 분의 2 로 줄어 접기 이득이 부풀었다. */
        if (nrot > 64) nrot = 64;
        { const char *e = getenv("FORCE_NROT");
          if (e) { const int f = atoi(e);
            if (f >= 2 && (long)f * rows_all <= BPRE_ROWS) nrot = f;
            else fprintf(stderr, "FORCE_NROT=%d 무시 (최대 %ld) — **두 팔이 어긋난다**\n",
                         f, (long)BPRE_ROWS / rows_all); } }
        fprintf(stderr, "[ss 팔] nrot=%d, 회전 작업집합 %ld KB%s\n", nrot,
                (long)nrot*K*N/1024, ((long)nrot*K*N <= 524288L) ? "  **<= L2 512 KB**" : "");
        if (nrot < 2) { fprintf(stderr, "회전 사본을 못 만듦 (%d 행)\n", rows_all); return 2; }
        for (int sg = 0; sg < (1 << sh); sg++)
          for (int q = 1; q < nrot; q++)
            memcpy(&Bpre[sg][q * rows_all * 16], &Bpre[sg][0], (size_t)rows_all * 16);
        /* E723: 마지막 반복이 쓰는 사본만 남기고 나머지를 오염시킨다. 발자국은 그대로이고
         * 사본 인덱스가 틀리면 반드시 답이 틀린다 (E721 의 맹점). */
        if (poison) {
          /* E725: 깨끗한 사본 둘레에 `POISON_GUARD` 행의 여유를 남긴다. wide mvin 이 요청
           * 블록을 조금 넘겨 읽는 것이 원인이라면 여유를 주면 PASS 한다 (하네스 버그라면
           * 여유와 무관하게 FAIL). 같은 바이너리에서 G=0 대 G>0 으로 가른다. */
          const int qlast = (int)((T - 1) % nrot);
          const char *ge = getenv("POISON_GUARD");
          const long G = ge ? atol(ge) : 0;
          const long lo = (long)qlast * rows_all - G, hi = (long)(qlast + 1) * rows_all + G;
          const long tot = (long)nrot * rows_all;
          for (int sg = 0; sg < (1 << sh); sg++) {
            if (lo > 0) memset(&Bpre[sg][0], 0x5A, (size_t)lo * 16);
            if (hi < tot) memset(&Bpre[sg][hi * 16], 0x5A, (size_t)(tot - hi) * 16);
          }
        }
      }
      if (segint) {   /* E720: 세그먼트 인터리브 (wide 치환 대신) */
        const long tot = (long)rows_all * (rot ? nrot : 1);
        if (tot * 16 > (1L << 19)) { fprintf(stderr, "seg: 임시 버퍼 초과\n"); return 2; }
        if (rows_all % 16) { fprintf(stderr, "seg: rows_all 이 16 배수가 아님\n"); return 2; }
        interleave(sh, tot);
      } else if (wide) {   /* E675: 회전 사본까지 만든 뒤 wide 순서로 치환 */
        /* E768: 적재부의 `wide_ok` 와 **같은 조건**(rpb)으로 가드해야 치환과 적재가 어긋나지
         * 않는다. rows_all 만 보면 rpb 가 어긋나는 경우를 놓친다 (E715). 중단이 아니라
         * 되돌림 — 그래야 폭이 섞인 진짜 모델을 잴 수 있다. */
#ifdef OLD_WIDEGUARD
        /* E790: 옛 형태 — 중단하고 `wide` 를 재대입하지 않는다 (상수 접기 유지 가설) */
        if (rows_all % WGRP) { fprintf(stderr, "wide: %d 행이 %d 로 안 나눠짐\n",
                                       rows_all, WGRP); return 2; }
        widen(sh, (long)rows_all * (rot ? nrot : 1));
#else
        { const int h_ = DIM >> sh, ks_ = h_ < 4 ? 4 : h_, rpb_ = (K / h_) * ks_;
          if (rpb_ % WGRP) {
            fprintf(stderr, "wide: rpb=%d 가 %d 로 안 나눠짐 — 폭 1 로 되돌림\n", rpb_, WGRP);
            wide = 0;
          } }
        if (wide) widen(sh, (long)rows_all * (rot ? nrot : 1));
#endif

        if (catlay) {   /* E721: (q,pp,sg,r) 로 재배치 — 스트림만 1 개로, 쓰기는 그대로 */
          const int S0 = 1 << sh, nt0 = N / (DIM << sh), rp0 = rows_all / nt0;
          const int Q = rot ? nrot : 1;
          const size_t tot = (size_t)Q * nt0 * S0 * rp0 * 16;
          if (tot > (size_t)4 * KMAX * NMAX) { fprintf(stderr, "cat: 버퍼 초과\n"); return 2; }
          int8_t *t2 = (int8_t *)malloc(tot);
          if (!t2) { fprintf(stderr, "cat: malloc 실패\n"); return 2; }
          for (int q = 0; q < Q; q++)
            for (int pp = 0; pp < nt0; pp++)
              for (int sg = 0; sg < S0; sg++)
                memcpy(&t2[((((size_t)q * nt0 + pp) * S0 + sg) * rp0) * 16],
                       &Bpre[sg][((size_t)q * rows_all + (size_t)pp * rp0) * 16],
                       (size_t)rp0 * 16);
          memcpy(&Bpre[0][0], t2, tot); free(t2);
        }
      } }
    grt_config_ex_shape_stride(&CTX, GRT_WS, sh, sl);
    grt_config_st(&CTX, N);
    load_b(sh, st); load_a(sh, abase);
    grt_ss_gemv_config(&CTX, K, N, M, sh, abase, 0, 0);
    grt_fence();
    memset(C, 0, sizeof C);
    const uint64_t t0 = rdt();
    if (amort) { load_b(sh, st); grt_ss_gemv_config(&CTX, K, N, M, sh, abase, 0, 0); }
    for (long it = 0; it < T; it++) {
      if (ovl) {                       /* E658: pp 블록마다 적재 + 부분 실행 (FSM 이 낼 수
                                        * 있는 가장 잘은 입도, E610 의 grt_ss_gemv_blk) */
        const int S2 = 1 << sh, h2 = DIM >> sh, w2 = DIM * S2, ks2 = h2 < 4 ? 4 : h2;
        /* E672: 나눗셈을 **발행 루프 밖으로**. `it % nrot` 를 안쪽에 두었더니 런타임
         * 나눗셈이 매 mvin 마다 돌아 plan 경로가 79 % 느려졌다 (corpus: 발행 루프는
         * 비어 있어야 한다, E205/E598). 상수 NROT 판(sspair_W)이 옳은 측정이었다. */
        const int rotoff = rot ? (int)(it % nrot) * rot_rows : 0;
        /* E721: `--cat` 의 사본 인덱스. `rotoff/rpb` 를 쓰면 q 가 아니라 **q*ntb** 가 되어
         * 범위를 벗어난다. S=4 는 사본들이 같은 값이라 우연히 PASS 했고 S=2 만 bad=256 으로
         * 잡혔다 — **회전 사본이 동일하면 주소 오류를 정확성 검사가 못 잡을 수 있다.** */
        const int qidx = rot ? (int)(it % nrot) : 0;
        const int ntb = N / w2, chb = K / h2, rpb = chb * ks2;
        /* E715: wide 는 **pp 블록 안에서** 64 행씩 끊으므로 `rpb` 가 64 의 배수여야 한다.
         * `widen()` 의 가드는 `rows_all`(= ntb*rpb) 만 봤는데, 그것이 64 로 나눠떨어져도
         * `rpb` 는 아닐 수 있다 ([160x256] S=2: rows_all 1280 OK, rpb 160 어긋남) —
         * 그러면 마지막 조각이 블록을 넘어 **bad=160 으로 조용히 틀린다**. 정확성 검사가
         * 잡았다. 어긋나면 그 모양만 폭 1 로 떨어뜨린다. */
        const int wide_ok = wide && !segint && (rpb % WGRP == 0);
        if (segint) grt_config_ld_bs(&CTX, (uint64_t)(S2 * 16), SSE_);
        else grt_config_ld(&CTX, wide_ok ? WGRP : 16);
        if (segint) {   /* E720: 16 행 x S 세그먼트를 명령 하나로, DRAM 스트림 1 개 */
          for (int pp = 0; pp < ntb; pp++) {
            for (int r0 = 0; r0 < rpb; r0 += DIM)
              grt_mvin(&CTX, &Bseg[(size_t)(rotoff + pp * rpb + r0) * S2 * 16],
                       (uint32_t)(SSE_ + pp * rpb + r0), S2 * DIM, DIM);
            if (!noexec) grt_ss_gemv_blk(&CTX, C, pp, 1);
          }
          continue;
        }
        if (sgouter) {   /* E722: 세그먼트를 바깥으로 — 한 뱅크에 몰아 쓴 뒤 다음 뱅크로.
                          * 계산 순서가 깨지므로 **적재 전용**(--noexec 와 함께)이다. */
          for (int sg = 0; sg < S2; sg++)
            for (int pp = 0; pp < ntb; pp++)
              for (int r0 = 0; r0 < rpb; r0 += WGRP)
                /* E728: src0 면 전 세그먼트가 Bpre[0] 의 연속 구간을 읽는다 — 소스 연속성이
                 * S=1 과 같아진다 (데이터는 틀리지만 noexec 이라 무관). */
                grt_mvin(&CTX, src0 ? &Bpre[0][(rotoff + (sg * ntb + pp) * rpb + r0) * 16]
                                    : &Bpre[sg][(rotoff + pp * rpb + r0) * 16],
                         bank1 ? (uint32_t)(SSE_ + (sg * ntb + pp) * rpb + r0)
                               : (uint32_t)(((1 + sg * st) % SSB_) * SSE_ + pp * rpb + r0),
                         WGRP, DIM);
          continue;
        }
        /* E815: `--pipe` 는 **계산 pp 앞에 적재 pp+1 을 낸다.** 현재 순서(적재 pp ->
         * 계산 pp)는 계산이 자기 적재를 기다리는데, 한 블록 앞서 실으면 그 대기가
         * 다음 블록의 적재로 채워진다. scratchpad 주소가 pp 로 갈리므로 안전하다
         * (`... + pp*rpb + r0`). E813e 가 K=256 에서 노출 계산 50 % 를 쟀고, 그것이
         * 줄어드는지 보는 것이 목적이다. */
        #define SS_LOAD_PP(PP) do { \
          for (int sg = 0; sg < S2; sg++) \
            if (wide_ok) \
              for (int r0 = 0; r0 < rpb; r0 += WGRP) \
                grt_mvin(&CTX, &Bpre[sg][(rotoff + (PP) * rpb + r0) * 16], \
                         (uint32_t)(((1 + sg * st) % SSB_) * SSE_ + (PP) * rpb + r0), \
                         WGRP, DIM); \
            else \
              for (int r0 = 0; r0 < rpb; r0 += DIM) { \
                const int nb3 = rpb - r0 > DIM ? DIM : rpb - r0; \
                grt_mvin(&CTX, &Bpre[sg][(rotoff + (PP) * rpb + r0) * 16], \
                         (uint32_t)(((1 + sg * st) % SSB_) * SSE_ + (PP) * rpb + r0), \
                         DIM, nb3); \
              } \
        } while (0)
        if (ovl == 3 && !noexec && !catlay && !bank1) {
          SS_LOAD_PP(0);
          for (int pp = 0; pp < ntb; pp++) {
            if (pp + 1 < ntb) SS_LOAD_PP(pp + 1);   /* 한 블록 앞서 싣는다 */
            grt_ss_gemv_blk(&CTX, C, pp, 1);
          }
          continue;
        }
        for (int pp = 0; pp < ntb; pp++) {
          for (int sg = 0; sg < S2; sg++)
            if (wide_ok)  /* E675: 64 행을 명령 하나로 (cols=64, rows=16) */
              for (int r0 = 0; r0 < rpb; r0 += WGRP)
                grt_mvin(&CTX, catlay
                           ? &Bpre[0][((((size_t)qidx * ntb + pp) * S2 + sg) * rpb + r0) * 16]
                           : &Bpre[sg][(rotoff + pp * rpb + r0) * 16],
                         bank1   /* E726: 뱅크 1 부터 연속 (계산은 틀리지만 noexec 전용) */
                           ? (uint32_t)(SSE_ + (sg * ntb + pp) * rpb + r0)
                           : (uint32_t)(((1 + sg * st) % SSB_) * SSE_ + pp * rpb + r0),
                         WGRP, DIM);
            else
            for (int r0 = 0; r0 < rpb; r0 += DIM) {
              const int nb2 = rpb - r0 > DIM ? DIM : rpb - r0;
              grt_mvin(&CTX, &Bpre[sg][(rotoff + pp * rpb + r0) * 16],
                       (uint32_t)(((1 + sg * st) % SSB_) * SSE_ + pp * rpb + r0), DIM, nb2);
            }
          if (!noexec) grt_ss_gemv_blk(&CTX, C, pp, 1);
        }
        continue;
      }
      if (with_load) { load_b(sh, st); grt_ss_gemv_config(&CTX, K, N, M, sh, abase, 0, 0); }
      grt_ss_gemv(&CTX, C);
    }
    grt_fence();
    cyc = (double)(rdt() - t0) * 100.0 / (amort ? 1.0 : (double)T);
  }
  if (noexec) bad = -1;    /* E677: 계산을 안 했으므로 검사 불가 — PASS 로 위장하지 않는다 */
  else
  for (int i = 0; i < M; i++) for (int n = 0; n < N; n++)
    if (C[i * N + n] != (int8_t)ref[i * N + n]) bad++;
  printf("pair mode=%s K=%d N=%d M=%d  %9.1f cyc  bad=%d %s\n",
         plan_mode ? (ss ? "plan/fsm" : "plan/tile") : ss ? "ss" : tile ? "tile" : "ws",
         K, N, M, cyc, bad, bad < 0 ? "SKIP" : bad ? "FAIL" : "PASS");
  if (ss) { grt_ss_plan_t q; grt_ss_plan_hw(&q, M, K, N, 8, 16384 / 8, with_load, max_sh);
            printf("   plan shape=%d strideLog=%d risky=%d\n", q.shape, q.stride_log, q.risky); }
  return bad != 0;
}
