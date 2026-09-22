// SSGemm wedge 협착 (E567b): ssdiag9 에서 S1(sh1 500 steps 무사) 직후 S2 가
// **첫 로그도 없이** wedge — fillW(2) 또는 첫 sh2 timestep 안이다. 그런데 ssdiag2 의
// A(64 pl/cp)+B(4뱅크 mvout 8개)는 같은 구조로 통과했으므로, 후보는
//   (i) sh2 단독 재현(순수 볼륨/구성이 아니라 이 정확한 시퀀스)
//   (ii) sh1 -> sh2 **전환** (shapeshift 의 존재 이유인 그 기능)
// 한 부팅으로 가른다. 각 단계 사이 fsync 로그 — wedge 지점이 꼬리에 남는다.
//
//  [A] sh2-only: fillW(2) + timestep 1개를 단계별 로그로 해부
//      (fill / mvin / matmul 32개씩 2회 / mvout 뱅크별)
//  [B] sh2 timestep 10개 반복
//  [C] 전환 재현: fillW(1) + sh1 timestep 2개 -> fillW(2) + sh2 timestep 1개
//  [D] 전환 최소형: config_ex(sh1) 만 낀다 (fill/timestep 없이) -> sh2 timestep 1개
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
  lfd = open("/mnt2/tmp/ssd10.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (lfd < 0) { perror("log"); return 2; }
  if (mlockall(MCL_CURRENT | MCL_FUTURE)) { perror("mlockall"); return 2; }
  grt_flush_ctx(&CTX);
  slog("ssd10 start");

  for (int k = 0; k < H; k++) for (int n = 0; n < H; n++) W[k * H + n] = (int8_t)((k + 2 * n) % 7 - 3);
  for (int k = 0; k < H; k++) x[k] = (int8_t)((3 * k) % 5 - 2);
  for (int n = 0; n < H; n++) {
    int32_t s = 0;
    for (int k = 0; k < H; k++) s += (int32_t)x[k] * W[k * H + n];
    yref[n] = s > 127 ? 127 : s < -128 ? -128 : s;
  }

  const int sh = 2, S = 4, h = 4, w = 64, ks = 4, nt = 2, ch = 32;

  /* [A] sh2-only 해부 */
  fillW(2); slog("A fillW2 done");
  grt_config_ld(&CTX, h);
  grt_mvin_rows(&CTX, x, 0, h, ch, h);
  grt_fence(); slog("A mvin-x done");
  for (int pp = 0; pp < nt; pp++) {
    for (int tt = 0; tt < ch; tt++) {
      uint32_t bsp = 1 * SP_ENT + 2048 + (pp * ch + tt) * ks;
      uint32_t csp = (tt == 0 ? GRT_ACC(pp) : GRT_ACC_ACC(pp));
      grt_preload(&CTX, bsp, csp, DIM, h, DIM, 1);
      grt_compute(&CTX, tt, GRT_GARBAGE, h, 1, 0, 0);
    }
    grt_fence();
    { char b[48]; snprintf(b, sizeof b, "A matmul pp=%d done", pp); slog(b); }
  }
  for (int pp = 0; pp < nt; pp++)
    for (int s = 0; s < S; s++) {
      grt_mvout(&CTX, y + pp * w + s * DIM, GRT_ACC(s * ACC_ENT + pp), DIM, 1);
      grt_fence();
      { char b[48]; snprintf(b, sizeof b, "A mvout pp=%d s=%d done", pp, s); slog(b); }
    }
  { char b[48]; snprintf(b, sizeof b, "A check bad=%d", check()); slog(b); }

  /* [B] sh2 timestep 10개 (ssgemv 동형, fence 는 끝에 한 번) */
  for (int t = 0; t < 10; t++) {
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
    grt_fence();
    { char b[48]; snprintf(b, sizeof b, "B step %d done", t + 1); slog(b); }
  }
  { char b[48]; snprintf(b, sizeof b, "B check bad=%d", check()); slog(b); }

  /* [C] 전환 재현: sh1 두 step -> sh2 한 step */
  fillW(1); slog("C fillW1 done");
  for (int t = 0; t < 2; t++) {
    const int S1 = 2, h1 = 8, w1 = 32, ks1 = 8, nt1 = 4, ch1 = 16;
    grt_config_ld(&CTX, h1);
    grt_mvin_rows(&CTX, x, 0, h1, ch1, h1);
    for (int pp = 0; pp < nt1; pp++)
      for (int tt = 0; tt < ch1; tt++) {
        grt_preload(&CTX, (uint32_t)(1 * SP_ENT + 2048 + (pp * ch1 + tt) * ks1),
                    tt == 0 ? GRT_ACC(pp) : GRT_ACC_ACC(pp), DIM, h1, DIM, 1);
        grt_compute(&CTX, tt, GRT_GARBAGE, h1, 1, 0, 0);
      }
    for (int pp = 0; pp < nt1; pp++)
      for (int s = 0; s < S1; s++)
        grt_mvout(&CTX, y + pp * w1 + s * DIM, GRT_ACC(s * ACC_ENT + pp), DIM, 1);
    grt_fence();
  }
  { char b[48]; snprintf(b, sizeof b, "C sh1 2steps bad=%d", check()); slog(b); }
  fillW(2); slog("C fillW2 done");
  {
    grt_config_ld(&CTX, h);
    grt_mvin_rows(&CTX, x, 0, h, ch, h);
    for (int pp = 0; pp < nt; pp++)
      for (int tt = 0; tt < ch; tt++) {
        grt_preload(&CTX, (uint32_t)(1 * SP_ENT + 2048 + (pp * ch + tt) * ks),
                    tt == 0 ? GRT_ACC(pp) : GRT_ACC_ACC(pp), DIM, h, DIM, 1);
        grt_compute(&CTX, tt, GRT_GARBAGE, h, 1, 0, 0);
      }
    for (int pp = 0; pp < nt; pp++)
      for (int s = 0; s < S; s++)
        grt_mvout(&CTX, y + pp * w + s * DIM, GRT_ACC(s * ACC_ENT + pp), DIM, 1);
    grt_fence();
  }
  { char b[64]; snprintf(b, sizeof b, "C sh1->sh2 step bad=%d", check()); slog(b); }

  slog("ssd10 ALL DONE");
  return 0;
}
