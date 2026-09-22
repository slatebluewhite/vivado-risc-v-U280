// SSGemm 기전 판별 6탄 (E565b): 회복 조건의 정밀화 + 실용 워크어라운드의 실리콘 검증.
//  P2': 16 tile(R1) -> junk mvout(미기록 행)+fence -> 16 tile 신선(R2) -> mvout 둘 다.
//       R2 착지  => 회복 mvout 은 아무 행이나 된다 (P2 의 잔여 의문 해소)
//       R2 = 0   => 회복 mvout 은 기록된 행을 읽어야 한다 (데이터 의존 — scale 경로?)
//  QF : 3 x (16 tile 신선 + own mvout + fence) — 워크어라운드가 무한 지속되는가.
//       각 사이클이 다른 chunk 구간을 계산하므로 기대값이 다 다르다.
//  QG : 16 tile(R) + own mvout + fence + 8 tile 이어서 accumulate(R) + own mvout.
//       두 번째 mvout = exp24 => own-row mvout 뒤 accumulate 연속 가능 (K-분할 GEMV 생존)
//       exp16              => 회복은 신선 시작만 — K>64 행은 acc 행을 옮겨야 한다.
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

static const int sh = 2, H = 128;
static const int S = 4, h = 4, ks = 4;
static const int bBase = 2048;
static int8_t W[128 * 128], x[128];

static int32_t prange(int t0, int t1, int n) {   /* chunks t0..t1-1 의 합 (포화) */
  int32_t s = 0;
  for (int k = t0 * h; k < t1 * h; k++) s += (int32_t)x[k] * W[k * H + n];
  return s > 127 ? 127 : s < -128 ? -128 : s;
}

static void tileop(int row, int tt, int fresh) {
  grt_preload(&CTX, (uint32_t)(1 * SP_ENT + bBase + tt * ks),
              fresh ? GRT_ACC(row) : GRT_ACC_ACC(row), DIM, h, DIM, 1);
  grt_compute(&CTX, tt, GRT_GARBAGE, h, 1, 0, 0);
}

static void dumpexp(const char *tag, const int8_t *q, int t0, int t1) {
  char b[220]; int off = snprintf(b, sizeof b, "%s y:", tag);
  for (int n = 0; n < 8; n++) off += snprintf(b + off, sizeof b - off, " %d", q[n]);
  off += snprintf(b + off, sizeof b - off, "  exp:");
  for (int n = 0; n < 8; n++) off += snprintf(b + off, sizeof b - off, " %d", (int)prange(t0, t1, n));
  slog(b);
}

int main(void) {
  lfd = open("/mnt2/tmp/ssd6.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (lfd < 0) { perror("log"); return 2; }
  if (mlockall(MCL_CURRENT | MCL_FUTURE)) { perror("mlockall"); return 2; }
  grt_flush_ctx(&CTX);
  slog("ssd6 start");

  const int nt = H / (DIM * S), ch = H / h;
  for (int k = 0; k < H; k++) for (int n = 0; n < H; n++) W[k * H + n] = (int8_t)((k + 2 * n) % 7 - 3);
  for (int k = 0; k < H; k++) x[k] = (int8_t)((3 * k) % 5 - 2);

  grt_config_ex_shape(&CTX, GRT_WS, sh);
  grt_config_st(&CTX, DIM);
  grt_config_ld(&CTX, H);
  for (int pp = 0; pp < nt; pp++)
    for (int tt = 0; tt < ch; tt++)
      for (int s = 0; s < S; s++)
        grt_mvin(&CTX, W + (size_t)(tt * h) * H + pp * DIM * S + s * DIM,
                 (uint32_t)(((1 + s) % SP_BANKS) * SP_ENT + bBase + (pp * ch + tt) * ks), DIM, h);
  grt_config_ld(&CTX, h);
  grt_mvin(&CTX, x, 0, h, ch);
  grt_fence();
  slog("fill OK");

  static int8_t q[64], junk[16];

  /* P2': 16(R20) -> junk mvout(R30 미기록)+fence -> 16 신선(R21) -> mvout 둘 다 */
  for (int tt = 0; tt < 16; tt++) tileop(20, tt, tt == 0);
  grt_mvout(&CTX, junk, GRT_ACC(30), DIM, 1); grt_fence();
  for (int tt = 16; tt < 32; tt++) tileop(21, tt, tt == 16);
  grt_mvout(&CTX, q, GRT_ACC(20), DIM, 1);
  grt_mvout(&CTX, q + 16, GRT_ACC(21), DIM, 1); grt_fence();
  dumpexp("P2p R20", q, 0, 16);
  dumpexp("P2p R21", q + 16, 16, 32);

  /* QF: 3 x (16 신선 + own mvout + fence) */
  for (int c = 0; c < 3; c++) {
    int row = 22 + c; int t0 = (c % 2) * 16;   /* chunk 구간 0-15, 16-31, 0-15 */
    for (int i = 0; i < 16; i++) tileop(row, t0 + i, i == 0);
    grt_mvout(&CTX, q, GRT_ACC(row), DIM, 1); grt_fence();
    char tag[16]; snprintf(tag, sizeof tag, "QF c%d", c);
    dumpexp(tag, q, t0, t0 + 16);
  }

  /* QG: 16(R25)+own mvout+fence -> 8 accumulate 이어서 -> own mvout */
  for (int tt = 0; tt < 16; tt++) tileop(25, tt, tt == 0);
  grt_mvout(&CTX, q, GRT_ACC(25), DIM, 1); grt_fence();
  dumpexp("QG mv1", q, 0, 16);
  for (int tt = 16; tt < 24; tt++) tileop(25, tt, 0);
  grt_mvout(&CTX, q, GRT_ACC(25), DIM, 1); grt_fence();
  dumpexp("QG mv2 (exp=chunk0..23)", q, 0, 24);

  slog("ssd6 ALL DONE");
  return 0;
}
