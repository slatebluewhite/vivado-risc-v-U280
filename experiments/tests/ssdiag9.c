// SSGemm t2 볼륨 wedge 이분탐색 (E567): 고친 x 적재로도 `ssgemv t 2 128 2000` 이
// hard-wedge(ping 사망) — mvin 아티팩트(E566)와 **별개의 실재 결함**. t0/t1 은
// T=2000 에서 무사했으므로 shape 2 + 볼륨 특이. 재부팅이 35분이므로 한 부팅으로
// 전 사다리를 끝낸다: 블록마다 fsync 로그 → wedge 지점이 로그 꼬리에 남는다.
//
//  [S1] t1(sh=1) 500 steps — 대조군 (무사 예상)
//  [S2] t2(sh=2) 사다리: 1, 10, 50, 100, 250, 500, 1000, 2000 steps
//       각 단계 끝에 정확성 검사 + 로그. 단계 사이 진행 로그는 50 step 마다.
// wedge 가 고정 step 수에서 나면 자원 고갈(결정적), 단계가 흔들리면 확률적.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "include/gemmini_rt.h"

#define DIM 16
#define SP_BANKS 4
#define SP_ENT 4096
#define ACC_ENT 256

static const grt_ctx CTX = { DIM, 1, 4, GRT_OP_INT8 };
static int lfd = -1;
static void slog(const char *msg) {
  char b[256]; int n = snprintf(b, sizeof b, "%s\n", msg);
  write(lfd, b, n); fsync(lfd);
}

static const int H = 128;
static int8_t W[128 * 128], x[128], y[128];
static int32_t yref[128];

/* ssgemv 의 mode t 한 timestep 을 그대로 (shape sh 파라미터화) */
static void timestep(int sh) {
  const int S = 1 << sh, h = DIM >> sh, w = DIM * S;
  const int ks = h < 4 ? 4 : h;
  const int nt = (H + w - 1) / w, ch = H / h;
  grt_config_ld(&CTX, h);
  grt_mvin_rows(&CTX, x, 0, h, ch, h);
  for (int pp = 0; pp < nt; pp++)
    for (int tt = 0; tt < ch; tt++) {
      uint32_t bsp = 1 * SP_ENT + 2048 + (pp * ch + tt) * ks;
      uint32_t csp = (tt == 0 ? GRT_ACC(pp) : GRT_ACC_ACC(pp));
      grt_preload(&CTX, bsp, csp, DIM, h, DIM, 1);
      grt_compute(&CTX, tt, GRT_GARBAGE, h, 1, 0, 0);
    }
  for (int pp = 0; pp < nt; pp++)
    for (int s = 0; s < S; s++)
      grt_mvout(&CTX, y + pp * w + s * DIM, GRT_ACC(s * ACC_ENT + pp), DIM, 1);
}

static void fillW(int sh) {
  const int S = 1 << sh, h = DIM >> sh, w = DIM * S;
  const int ks = h < 4 ? 4 : h;
  const int nt = (H + w - 1) / w, ch = H / h;
  grt_config_ex_shape(&CTX, GRT_WS, sh);
  grt_config_st(&CTX, DIM);
  grt_config_ld(&CTX, H);
  for (int pp = 0; pp < nt; pp++)
    for (int tt = 0; tt < ch; tt++)
      for (int s = 0; s < S; s++)
        grt_mvin(&CTX, W + (size_t)(tt * h) * H + pp * w + s * DIM,
                 (uint32_t)(((1 + s) % SP_BANKS) * SP_ENT + 2048 + (pp * ch + tt) * ks), DIM, h);
  grt_fence();
}

static int check(void) {
  int bad = 0;
  for (int n = 0; n < H; n++) if (y[n] != (int8_t)yref[n]) bad++;
  return bad;
}

int main(void) {
  lfd = open("/mnt2/tmp/ssd9.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (lfd < 0) { perror("log"); return 2; }
  if (mlockall(MCL_CURRENT | MCL_FUTURE)) { perror("mlockall"); return 2; }
  grt_flush_ctx(&CTX);
  slog("ssd9 start");

  for (int k = 0; k < H; k++) for (int n = 0; n < H; n++) W[k * H + n] = (int8_t)((k + 2 * n) % 7 - 3);
  for (int k = 0; k < H; k++) x[k] = (int8_t)((3 * k) % 5 - 2);
  for (int n = 0; n < H; n++) {
    int32_t s = 0;
    for (int k = 0; k < H; k++) s += (int32_t)x[k] * W[k * H + n];
    yref[n] = s > 127 ? 127 : s < -128 ? -128 : s;
  }

  /* [S1] 대조군: sh=1, 500 steps */
  fillW(1);
  for (int t = 0; t < 500; t++) {
    timestep(1);
    if (t % 50 == 49) { grt_fence(); char b[64]; snprintf(b, sizeof b, "S1 t1 step %d ok", t + 1); slog(b); }
  }
  grt_fence();
  { char b[64]; snprintf(b, sizeof b, "S1 t1 500steps done bad=%d", check()); slog(b); }

  /* [S2] t2 사다리 */
  fillW(2);
  const int lad[8] = { 1, 10, 50, 100, 250, 500, 1000, 2000 };
  for (int L = 0; L < 8; L++) {
    for (int t = 0; t < lad[L]; t++) {
      timestep(2);
      if (t % 50 == 49) { grt_fence(); char b[64]; snprintf(b, sizeof b, "S2 T%d step %d", lad[L], t + 1); slog(b); }
    }
    grt_fence();
    char b[64]; snprintf(b, sizeof b, "S2 t2 T=%d done bad=%d", lad[L], check()); slog(b);
  }
  slog("ssd9 ALL DONE");
  return 0;
}
