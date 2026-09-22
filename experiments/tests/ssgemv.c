// SSGemm 보드 실증 (E547~E549): 접힌 shape 로 GEMV 체인을 돌린다.
//
//   ./ssgemv <mode> <shape> <H> <T>
//     mode  = t (tile-by-tile, 소프트웨어 발행)  |  f (ShapeshiftGemvLoop, funct 23/24)
//     shape = 0(16x16) | 1(8x32) | 2(4x64)
//     H     = K = N (K % h == 0 이어야 한다; 64 의 배수면 전 shape 안전)
//     T     = timestep 수 (rdtime 해상도가 100 사이클이라 수천 권장)
//
// W(HxH, int8)는 세그먼트 배치로 scratchpad 에 **상주**시킨다 — timestep 루프 안에는
// x mvin + preload/compute (+ mvout) 만 있다. 이것이 E547 의 수정된 실험 설계다:
//   [1'] mode t 는 shape 무관하게 같은 시간 (명령 바운드 ~27 cyc/tile)
//   [E548 기대] mode f 는 execute 비 (H=128 에서 S=4/S=1 = 3.4~3.8x)
//
// 정확성: CPU int32 골든을 ±127 포화까지 재현해 마지막 timestep 의 y 와 비교.
// (같은 x 를 반복하므로 모든 timestep 의 y 는 같다.)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include "include/gemmini_rt.h"

#define DIM 16
/* E594: 뱅크 수는 비트스트림 속성이다. 환경변수 SP_BANKS 로 받고(기본 4),
 * 뱅크당 엔트리는 총 16384 행을 나눈 값이다 (256 KB / 16 B). 같은 바이너리가
 * 두 비트스트림에서 돌아야 통제된 비교가 된다. */
#ifdef SPB                      /* 컴파일 타임 고정판 — 발행 루프의 주소 계산을 접기 위해.
                                 * CLAUDE.md: "발행 루프는 비어 있어야 한다" (E205 계열) */
static const int SP_BANKS = SPB, SP_ENT = 16384 / SPB;
#else
static int SP_BANKS = 4, SP_ENT = 4096;
#endif
#define ACC_ENT 256          /* 64 KB / 4 banks / 64 B */

static const grt_ctx CTX = { DIM, 1, 4, GRT_OP_INT8 };
static inline uint64_t rdt(void) { uint64_t t; asm volatile("rdtime %0" : "=r"(t)); return t; }

int main(int argc, char **argv) {
  if (argc < 5) { fprintf(stderr, "usage: %s t|f shape H T [--neg]\n", argv[0]); return 2; }
  const int fsm = argv[1][0] == 'f';
  const int sh = atoi(argv[2]), H = atoi(argv[3]);
  const long T = atol(argv[4]);
  const int neg = argc > 5 && !strcmp(argv[5], "--neg");
  /* E570: fence 는 기본 OFF — bit6(RS 접힌-발자국 수정)의 무펜스 판정이 기본 경로.
   * bit4 류 구형 비트스트림이나 오버헤드 비교에는 --fence 로 켠다. */
  const int fenced = argc > 5 && !strcmp(argv[5], "--fence");
  /* E584: x 의 mvin 을 timestep 루프 밖으로 뺀다. 매 스텝 같은 x 를 쓰므로 결과는
   * 불변이고(정확성 검사 그대로 유효), 시간 차이가 곧 스텝당 mvin 의 몫이다. */
  const int noldx = argc > 5 && !strcmp(argv[5], "--noldx");
  const int S = 1 << sh, h = DIM >> sh, w = DIM * S;
  const int ks = h < 4 ? 4 : h;
  const int nt = (H + w - 1) / w, ch = H / h;
  if (H % h || H % DIM) { fprintf(stderr, "H %% %d != 0\n", h > DIM ? h : DIM); return 2; }

  #ifndef SPB
  { const char *e = getenv("SP_BANKS"); if (e) SP_BANKS = atoi(e);
    SP_ENT = 16384 / SP_BANKS; }
#endif        /* 총 16384 행(256 KB/16 B)을 뱅크로 나눔 */
  if (mlockall(MCL_CURRENT | MCL_FUTURE)) { perror("mlockall"); return 2; }
  grt_flush_ctx(&CTX);   /* E554: TLB flush — 두 번째 프로세스부터의 stale 번역이 보드
                            실패(부분 0/wedge)의 전부였다. corpus 규칙의 재학습. */

  static int8_t W[1024 * 1024], x[1024], y[1024];
  int32_t yref[1024];
  for (int k = 0; k < H; k++) for (int n = 0; n < H; n++) W[k * H + n] = (int8_t)((k + 2 * n) % 7 - 3);
  for (int k = 0; k < H; k++) x[k] = (int8_t)((3 * k) % 5 - 2);
  for (int n = 0; n < H; n++) {
    int32_t s = 0;
    for (int k = 0; k < H; k++) s += (int32_t)x[k] * W[k * H + n];
    yref[n] = s > 127 ? 127 : s < -128 ? -128 : s;
  }
  if (neg) yref[H / 2] ^= 1;   /* 음성 대조: 검사기가 실패할 수 있는가 */

  /* ---- W 를 세그먼트 배치로 sp 에 넣는다 (한 번) ----
   * tile (pp,tt) 의 세그먼트 s -> 뱅크 (1+s) % SP_BANKS, 행 bBase + (pp*ch+tt)*ks.
   * S=4 는 세그먼트 3 이 뱅크 0 으로 감기므로 bBase 를 A 영역(행 0..ch) 위로 띄운다. */
  const int aBase = 0, accBase = 0;
  const int bBase = SP_ENT / 2;                 /* 뱅크 0 wrap 과 A 의 충돌 회피 */
  if (bBase + nt * ch * ks > SP_ENT) { fprintf(stderr, "W가 sp에 안 들어감\n"); return 2; }
  grt_config_ex_shape(&CTX, GRT_WS, sh);
  grt_config_st(&CTX, H);
  grt_config_ld(&CTX, H);                       /* W: DRAM 행 간격 = H */
  for (int pp = 0; pp < nt; pp++)
    for (int tt = 0; tt < ch; tt++)
      for (int s = 0; s < S; s++) {
        uint32_t sp = ((1 + s) % SP_BANKS) * SP_ENT + bBase + (pp * ch + tt) * ks;
        grt_mvin(&CTX, W + (size_t)(tt * h) * H + pp * w + s * DIM, sp, DIM, h);
      }
  grt_fence();

  if (fsm) grt_ss_gemv_config(&CTX, H, H, 1, sh, aBase, bBase, accBase);

  uint64_t t0 = rdt();
  if (noldx) {                                  /* 한 번만 적재하고 루프에서는 생략 */
    grt_config_ld(&CTX, h);
    grt_mvin_rows(&CTX, x, aBase, h, ch, h);
    grt_fence();
  }
  for (long it = 0; it < T; it++) {
    if (!noldx) {
      grt_config_ld(&CTX, h);                   /* x: 연속 h 바이트가 한 tile 행 */
      grt_mvin_rows(&CTX, x, aBase, h, ch, h);  /* E566: rows > DIM 은 필드 절단 */
    }
    if (fsm) {
      /* E570: LD∥EX(mvin과 FSM 스트림)·스텝 경계가 Scratchpad층 교착의 방아쇠 —
       * --fence 로 앞뒤를 직렬화한다 (bit6의 ss_gemv_busy 수정으로 fence가 FSM을 기다림) */
      if (fenced) grt_fence();
      grt_ss_gemv(&CTX, y);
      if (fenced) grt_fence();
    } else {
      for (int pp = 0; pp < nt; pp++)
        for (int tt = 0; tt < ch; tt++) {
          uint32_t bsp = 1 * SP_ENT + bBase + (pp * ch + tt) * ks;
          uint32_t csp = (tt == 0 ? GRT_ACC(accBase + pp) : GRT_ACC_ACC(accBase + pp));
          grt_preload(&CTX, bsp, csp, DIM, h, DIM, 1);
          grt_compute(&CTX, aBase + tt, GRT_GARBAGE, h, 1, 0, 0);
        }
      /* E568/E570: RS 접힌-발자국 미수정 비트스트림(#4/#5)에서는 여기 fence 필수. */
      if (fenced) grt_fence();
      for (int pp = 0; pp < nt; pp++)
        for (int s = 0; s < S; s++)
          grt_mvout(&CTX, y + pp * w + s * DIM, GRT_ACC(s * ACC_ENT + accBase + pp), DIM, 1);
      /* E569/E570: 스텝 경계 WAR — 위와 동일 조건부. */
      if (fenced) grt_fence();
    }
  }
  grt_fence();
  uint64_t dt = rdt() - t0;

  int bad = 0;
  for (int n = 0; n < H; n++) if (y[n] != (int8_t)yref[n] && bad++ < 5)
    printf("MISMATCH n=%d got=%d exp=%d\n", n, y[n], (int)yref[n]);
  printf("%s shape=%d H=%d T=%ld  ticks=%llu  cyc/step=%.1f  %s\n",
         fsm ? "fsm" : "tile", sh, H, T, (unsigned long long)dt,
         (double)dt * 100.0 / (double)T, bad ? "FAIL" : "PASS");
  return bad != 0;
}
