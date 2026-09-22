// SSGemm 기전 판별 5탄 (E565): ssdiag4 가 "회복 = mvout AND 신선 시작" 조합임을 보인
// 뒤의 후속 절단. 관찰이 전부 mvout 경유이므로 "쓰기 소실"과 "쓰기가 상류에 영구히
// 낌(mvout 은 낀 것을 지나쳐 읽음)"이 아직 미구분 — Q-A 의 이중 mvout 이 그걸 가른다.
//
//  Q-A: 24 tile -> mvout#1 -> fence+100ms -> mvout#2.
//       y1=exp16 & y2=exp24  => 상류에 낌 + mvout 이 뚫음
//       y1=y2=exp16          => 진짜 소실 (또는 여전히 낌)
//  Q-B: 24 tile -> 신선 pl/cp 1개(다른 행) -> mvout. exp24 면 신선 쓰기가 뚫음.
//  Q-E: 8 real + 8 garbage-C 패스 + 10 real -> mvout.
//       y=prefix(16 real)=exp18?  아니 real 18개면 앞 16 real 만 착지 시 exp16.
//       패스를 세면 garbage 8개가 허용량을 먹어 real 9..18 소실 -> y=exp8.
//  Q-S: 자기검증 (psum vs 직접곱).
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

static void tileop(int row, int tt, int fresh) {
  grt_preload(&CTX, (uint32_t)(1 * SP_ENT + bBase + tt * ks),
              fresh ? GRT_ACC(row) : GRT_ACC_ACC(row), DIM, h, DIM, 1);
  grt_compute(&CTX, tt, GRT_GARBAGE, h, 1, 0, 0);
}
static void tileop_garbagec(int tt) {   /* C=garbage: 패스는 돌지만 acc 쓰기 없음 */
  grt_preload(&CTX, (uint32_t)(1 * SP_ENT + bBase + tt * ks),
              GRT_GARBAGE, DIM, h, DIM, 1);
  grt_compute(&CTX, tt, GRT_GARBAGE, h, 1, 0, 0);
}

static void dump(const char *tag, const int8_t *q) {
  char b[200]; int off = snprintf(b, sizeof b, "%s y0..7:", tag);
  for (int n = 0; n < 8; n++) off += snprintf(b + off, sizeof b - off, " %d", q[n]);
  slog(b);
}

int main(void) {
  lfd = open("/mnt2/tmp/ssd5.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (lfd < 0) { perror("log"); return 2; }
  if (mlockall(MCL_CURRENT | MCL_FUTURE)) { perror("mlockall"); return 2; }
  grt_flush_ctx(&CTX);
  slog("ssd5 start");

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
  { char b[220]; int off = snprintf(b, sizeof b, "fill OK. exp8:");
    for (int n = 0; n < 8; n++) off += snprintf(b + off, sizeof b - off, " %d", (int)psum(8, n));
    off += snprintf(b + off, sizeof b - off, " exp16:");
    for (int n = 0; n < 8; n++) off += snprintf(b + off, sizeof b - off, " %d", (int)psum(16, n));
    off += snprintf(b + off, sizeof b - off, " exp18:");
    for (int n = 0; n < 8; n++) off += snprintf(b + off, sizeof b - off, " %d", (int)psum(18, n));
    off += snprintf(b + off, sizeof b - off, " exp24:");
    for (int n = 0; n < 8; n++) off += snprintf(b + off, sizeof b - off, " %d", (int)psum(24, n));
    slog(b); }

  static int8_t q[64];

  /* Q-A: 24 tiles -> mvout#1 -> fence+100ms -> mvout#2 */
  for (int tt = 0; tt < 24; tt++) tileop(40, tt, tt == 0);
  grt_mvout(&CTX, q, GRT_ACC(40), DIM, 1); grt_fence();
  dump("QA mv1", q);
  usleep(100000);
  grt_mvout(&CTX, q, GRT_ACC(40), DIM, 1); grt_fence();
  dump("QA mv2", q);

  /* Q-B: 24 tiles(R41) -> 신선 pl/cp(R43, tile 24) -> mvout R41, R43 */
  for (int tt = 0; tt < 24; tt++) tileop(41, tt, tt == 0);
  tileop(43, 24, 1);
  grt_mvout(&CTX, q, GRT_ACC(41), DIM, 1);
  grt_mvout(&CTX, q + 16, GRT_ACC(43), DIM, 1); grt_fence();
  dump("QB R41", q);
  { char b[200]; int off = snprintf(b, sizeof b, "QB R43 y0..7:");
    for (int n = 0; n < 8; n++) off += snprintf(b + off, sizeof b - off, " %d", q[16 + n]);
    off += snprintf(b + off, sizeof b - off, "  expChunk24:");
    for (int n = 0; n < 8; n++) {
      int32_t s = 0; for (int k = 24 * h; k < 25 * h; k++) s += (int32_t)x[k] * W[k * H + n];
      s = s > 127 ? 127 : s < -128 ? -128 : s;
      off += snprintf(b + off, sizeof b - off, " %d", (int)s);
    }
    slog(b); }

  /* Q-E: 8 real + 8 garbage-C + 10 real (tt 8..17) -> mvout.
     쓰기를 세면 real 16개까지 착지 -> exp16 (tt 0..15 의 합); 패스를 세면 exp8. */
  for (int tt = 0; tt < 8; tt++) tileop(42, tt, tt == 0);
  for (int g = 0; g < 8; g++) tileop_garbagec(24 + (g % 8));
  for (int tt = 8; tt < 18; tt++) tileop(42, tt, 0);
  grt_mvout(&CTX, q, GRT_ACC(42), DIM, 1); grt_fence();
  dump("QE 8r+8g+10r", q);

  { int bad = 0;
    for (int n = 0; n < 8; n++) {
      int32_t s = 0; for (int k = 0; k < 24 * h; k++) s += (int32_t)x[k] * W[k * H + n];
      s = s > 127 ? 127 : s < -128 ? -128 : s;
      if (s != psum(24, n)) bad++;
    }
    slog(bad ? "QS selfcheck FAIL" : "QS selfcheck PASS");
  }
  slog("ssd5 ALL DONE");
  return 0;
}
