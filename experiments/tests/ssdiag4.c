// SSGemm 기전 판별 4탄 (E564): "mvout 뒤 연속 matmul 16개 착지" 규칙의 리셋 기전을
// 가른다. RS+EC 는 xsim 에서 결백 판정(E564 R1~R8) — 남은 용의선은 Scratchpad glue
// (acc 쓰기 중재/AccumulatorScale/DMA)와 프론트엔드. 각 프로브가 프로그램 안에서
// fsync 되므로 wedge 로 죽어도 직전까지 NFS 에 남는다.
//
//  P0: 17-tile 체인 재현 (exp16 = -5 9 9 -5 -5 2 -5 -5 이면 절단 재현)
//  P4: 16 tile + fence + 100ms 지연 + 1 tile — 경성 카운터 vs 시간-배수 큐
//  P2: 16 tile + "무관 행" mvout + 8 tile — 아무 acc 읽기나 리셋하는가
//  P3: 16 tile(R1) + 8 tile(R2, mvout 없음) — 허용량이 전역인가
//  P1: 16 tile + mvin-to-acc + 8 tile — DMA 의 acc 쓰기도 리셋하는가 (최고위험 최후)
//  P5: 검사기 자기검증 (음성 대조)
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

static int32_t psum(int T, int n) {
  int32_t s = 0;
  for (int k = 0; k < T * h; k++) s += (int32_t)x[k] * W[k * H + n];
  return s > 127 ? 127 : s < -128 ? -128 : s;
}

/* tiles tStart..tStart+cnt-1 을 GRT_ACC(row) 로 체인. first=1 이면 첫 tile 은 덮어쓰기 */
static void chain(int row, int tStart, int cnt, int first) {
  for (int i = 0; i < cnt; i++) {
    int tt = tStart + i;
    grt_preload(&CTX, (uint32_t)(1 * SP_ENT + bBase + tt * ks),
                (first && i == 0) ? GRT_ACC(row) : GRT_ACC_ACC(row), DIM, h, DIM, 1);
    grt_compute(&CTX, tt, GRT_GARBAGE, h, 1, 0, 0);
  }
}

static void dump(const char *tag, int row, const int8_t *q) {
  char b[200]; int off = snprintf(b, sizeof b, "%s y0..7:", tag);
  for (int n = 0; n < 8; n++) off += snprintf(b + off, sizeof b - off, " %d", q[n]);
  slog(b);
}

int main(void) {
  lfd = open("/mnt2/tmp/ssd4.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (lfd < 0) { perror("log"); return 2; }
  if (mlockall(MCL_CURRENT | MCL_FUTURE)) { perror("mlockall"); return 2; }
  grt_flush_ctx(&CTX);
  slog("ssd4 start (S=4, H=128)");

  const int nt = H / (DIM * S), ch = H / h;   /* 2, 32 */
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
  slog("fill OK (W 256 mvins + x)");

  { char b[200]; int off = snprintf(b, sizeof b, "exp16:");
    for (int n = 0; n < 8; n++) off += snprintf(b + off, sizeof b - off, " %d", (int)psum(16, n));
    off += snprintf(b + off, sizeof b - off, "  exp17:");
    for (int n = 0; n < 8; n++) off += snprintf(b + off, sizeof b - off, " %d", (int)psum(17, n));
    off += snprintf(b + off, sizeof b - off, "  exp24:");
    for (int n = 0; n < 8; n++) off += snprintf(b + off, sizeof b - off, " %d", (int)psum(24, n));
    slog(b); }

  static int8_t q[64];

  /* P0: 재현 — 17 tiles, mvout */
  chain(60, 0, 17, 1);
  grt_mvout(&CTX, q, GRT_ACC(60), DIM, 1); grt_fence();
  dump("P0 T17", 60, q);

  /* P4: 16 + fence + 100ms + 1 — 시간이 허용량을 회복시키는가 */
  chain(61, 0, 16, 1);
  grt_fence();
  usleep(100000);
  chain(61, 16, 1, 0);
  grt_mvout(&CTX, q, GRT_ACC(61), DIM, 1); grt_fence();
  dump("P4 16+delay+1", 61, q);

  /* P2: 16 + 무관-행 mvout + 8 — 아무 acc 읽기나 리셋하는가 */
  chain(62, 0, 16, 1);
  { static int8_t junk[16];
    grt_mvout(&CTX, junk, GRT_ACC(59), DIM, 1); }   /* 미기록 행 — 값은 버린다 */
  grt_fence();
  chain(62, 16, 8, 0);
  grt_mvout(&CTX, q, GRT_ACC(62), DIM, 1); grt_fence();
  dump("P2 16+jmvout+8", 62, q);

  /* P3: 16(R1) + 8(R2), 사이 mvout 없음 — 허용량이 전역인가. R2 기대 = chunk16..23 */
  chain(63, 0, 16, 1);
  chain(58, 16, 8, 1);
  grt_mvout(&CTX, q, GRT_ACC(63), DIM, 1);
  grt_mvout(&CTX, q + 16, GRT_ACC(58), DIM, 1); grt_fence();
  dump("P3 R1(exp16)", 63, q);
  { char b[200]; int off = snprintf(b, sizeof b, "P3 R2 y0..7:");
    for (int n = 0; n < 8; n++) off += snprintf(b + off, sizeof b - off, " %d", q[16 + n]);
    off += snprintf(b + off, sizeof b - off, "  expChunk16..23:");
    for (int n = 0; n < 8; n++) {
      int32_t s = 0; for (int k = 16 * h; k < 24 * h; k++) s += (int32_t)x[k] * W[k * H + n];
      s = s > 127 ? 127 : s < -128 ? -128 : s;
      off += snprintf(b + off, sizeof b - off, " %d", (int)s);
    }
    slog(b); }

  /* P1: 16 + mvin-to-acc + 8 — DMA 의 acc 쓰기가 리셋하는가 (wedge 위험, 최후) */
  slog("P1 enter (mvin-to-acc)");
  chain(57, 0, 16, 1);
  { static int32_t zrow[16] = {0};
    grt_config_ld(&CTX, 64);
    grt_mvin(&CTX, zrow, (uint32_t)(GRT_ACC(56)), DIM, 1);
    grt_config_ld(&CTX, h); }
  grt_fence();
  chain(57, 16, 8, 0);
  grt_mvout(&CTX, q, GRT_ACC(57), DIM, 1); grt_fence();
  dump("P1 16+accmvin+8", 57, q);

  /* P5: 검사기 자기검증 — psum 이 CPU 직접곱과 같은가 + 음성 */
  { int bad = 0;
    for (int n = 0; n < 8; n++) {
      int32_t s = 0; for (int k = 0; k < 24 * h; k++) s += (int32_t)x[k] * W[k * H + n];
      s = s > 127 ? 127 : s < -128 ? -128 : s;
      if (s != psum(24, n)) bad++;
    }
    if (psum(24, 0) == psum(24, 0) + 1) bad += 100;  /* 음성: 이 줄이 참이 되면 안 된다 */
    slog(bad ? "P5 selfcheck FAIL" : "P5 selfcheck PASS");
  }
  slog("ssd4 ALL DONE");
  return 0;
}
