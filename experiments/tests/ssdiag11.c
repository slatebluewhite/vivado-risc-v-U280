// SSGemm 동시성 절단 (E570): bit6(RS acc-dst 수정)에서도 무펜스 t2 즉사 — 수정은
// 필요조건이었지만 불충분. 남은 후보는 의존이 아니라 **DMA와 접힌 EX 의 동시 비행
// 자체**(Scratchpad/DMA glue 교착). 어느 조합이 죽이는지 가른다 (사전 예측 포함):
//
//  V0: 완전 fence 스텝 x3        — 무사 (알려진 안전 경로; 검사기 sanity)
//  V1: pl/cp 만 x50 (루프 내 DMA 없음) — 무사 예측 (순수 EX)
//  V2: mvin + pl/cp x50 (mvout 없음)  — LD∥EX 동시성
//  V3: pl/cp + mvout x50 (mvin 없음)  — ST∥EX: RS 수정이 설계대로면 **무사** 예측
//     (store 는 넓힌 dst 로 compute 를 기다리고, 다음 스텝 preload 는 WAR 로 store 를
//      기다림 — 완전 직렬화. 여기서 죽으면 RS 수정 자체가 실리콘에서 불충분.)
//  V4: 전부 (무펜스 timestep 원형) x50 — 죽음 재현 확인
// 각 스텝마다 fsync 로그 — wedge 지점이 꼬리에 남는다. 순서는 안전→위험.
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
static const int S = 4, h = 4, w = 64, ks = 4, nt = 2, ch = 32;
static int8_t W[128 * 128], x[128], y[128];

static void plcp(void) {
  for (int pp = 0; pp < nt; pp++)
    for (int tt = 0; tt < ch; tt++) {
      grt_preload(&CTX, (uint32_t)(1 * SP_ENT + 2048 + (pp * ch + tt) * ks),
                  tt == 0 ? GRT_ACC(pp) : GRT_ACC_ACC(pp), DIM, h, DIM, 1);
      grt_compute(&CTX, tt, GRT_GARBAGE, h, 1, 0, 0);
    }
}
static void mvouts(void) {
  for (int pp = 0; pp < nt; pp++)
    for (int s = 0; s < S; s++)
      grt_mvout(&CTX, y + pp * w + s * DIM, GRT_ACC(s * ACC_ENT + pp), DIM, 1);
}
static void mvinx(void) {
  grt_config_ld(&CTX, h);
  grt_mvin_rows(&CTX, x, 0, h, ch, h);
}

int main(void) {
  lfd = open("/mnt2/tmp/ssd11.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (lfd < 0) { perror("log"); return 2; }
  if (mlockall(MCL_CURRENT | MCL_FUTURE)) { perror("mlockall"); return 2; }
  grt_flush_ctx(&CTX);
  slog("ssd11 start (bit6)");

  for (int k = 0; k < H; k++) for (int n = 0; n < H; n++) W[k * H + n] = (int8_t)((k + 2 * n) % 7 - 3);
  for (int k = 0; k < H; k++) x[k] = (int8_t)((3 * k) % 5 - 2);

  grt_config_ex_shape(&CTX, GRT_WS, 2);
  grt_config_st(&CTX, DIM);
  grt_config_ld(&CTX, H);
  for (int pp = 0; pp < nt; pp++)
    for (int tt = 0; tt < ch; tt++)
      for (int s = 0; s < S; s++)
        grt_mvin(&CTX, W + (size_t)(tt * h) * H + pp * w + s * DIM,
                 (uint32_t)(((1 + s) % SP_BANKS) * SP_ENT + 2048 + (pp * ch + tt) * ks), DIM, h);
  mvinx();
  grt_fence();
  slog("fill OK");

  /* V0 */
  for (int t = 0; t < 3; t++) { mvinx(); grt_fence(); plcp(); grt_fence(); mvouts(); grt_fence(); }
  slog("V0 fenced x3 done");

  /* V1: pl/cp 만 */
  for (int t = 0; t < 50; t++) {
    plcp();
    if (t % 10 == 9) { grt_fence(); char b[32]; snprintf(b, sizeof b, "V1 %d", t + 1); slog(b); }
  }
  grt_fence(); slog("V1 done");

  /* V2: mvin + pl/cp */
  for (int t = 0; t < 50; t++) {
    mvinx(); plcp();
    if (t % 10 == 9) { grt_fence(); char b[32]; snprintf(b, sizeof b, "V2 %d", t + 1); slog(b); }
  }
  grt_fence(); slog("V2 done");

  /* V3: pl/cp + mvout */
  for (int t = 0; t < 50; t++) {
    plcp(); mvouts();
    if (t % 10 == 9) { grt_fence(); char b[32]; snprintf(b, sizeof b, "V3 %d", t + 1); slog(b); }
  }
  grt_fence(); slog("V3 done");

  /* V4: 전부 */
  for (int t = 0; t < 50; t++) {
    mvinx(); plcp(); mvouts();
    if (t % 10 == 9) { grt_fence(); char b[32]; snprintf(b, sizeof b, "V4 %d", t + 1); slog(b); }
  }
  grt_fence(); slog("V4 done");
  slog("ssd11 ALL DONE");
  return 0;
}
