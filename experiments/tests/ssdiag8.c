// SSGemm 판결 실험 (E566): "16-절단"의 진범 후보 = 내 테스트들의 x 적재.
//   grt_mvin(x, 0, h, 32) 의 rows=32 는 rs2[52:48] 5비트로 절단되어 0 이 되고,
//   LoadController 는 (관찰상) 16행만 싣는다 -> sp bank0 rows 16..31 은 영원히 0
//   -> tile tt>=16 은 A=0 -> "17번째부터 소실"의 전부가 이것이라는 가설.
//
// 사전등록 예측 (실행 전 기록):
//   [1] 구식 적재(rows=32 한 방) 후 bank0 rows 0..31 되읽기:
//       rows 0..15 = x, rows 16..31 = 전부 0        <- 가설의 직접 확인
//   [2] 신식 적재(16행 mvin 두 번) 후 T=24 체인: y = exp24 정확
//       (틀리면 가설 기각 — 하드웨어 결함이 실재)
//   [3] 신식 적재 + T=32 전체 체인(H=128 의 ch=32 전부): y = full(=exp32) 정확
//   [4] 음성: 신식 적재 후 기대값을 한 바이트 틀면 검사가 실패해야 한다
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

static int32_t prange(int t0, int t1, int n) {
  int32_t s = 0;
  for (int k = t0 * h; k < t1 * h; k++) s += (int32_t)x[k] * W[k * H + n];
  return s > 127 ? 127 : s < -128 ? -128 : s;
}
static void tileop(int row, int tt, int fresh) {
  grt_preload(&CTX, (uint32_t)(1 * SP_ENT + bBase + tt * ks),
              fresh ? GRT_ACC(row) : GRT_ACC_ACC(row), DIM, h, DIM, 1);
  grt_compute(&CTX, tt, GRT_GARBAGE, h, 1, 0, 0);
}

int main(void) {
  lfd = open("/mnt2/tmp/ssd8.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (lfd < 0) { perror("log"); return 2; }
  if (mlockall(MCL_CURRENT | MCL_FUTURE)) { perror("mlockall"); return 2; }
  grt_flush_ctx(&CTX);
  slog("ssd8 start");

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
  grt_fence();

  static int8_t q[16];

  /* [1] 구식 적재 + 되읽기: rows 16..31 이 0 인지 */
  grt_config_ld(&CTX, h);
  grt_mvin(&CTX, x, 0, h, ch);              /* rows=32 — 문제의 인코딩 그대로 */
  grt_fence();
  {
    int okLo = 1, zHi = 1, nzHi = 0;
    grt_config_st(&CTX, h);                 /* sp 되읽기: DRAM 행 간격 = h 바이트 */
    for (int r = 0; r < 32; r++) {
      grt_mvout(&CTX, q, (uint32_t)r, h, 1); grt_fence();
      for (int c = 0; c < h; c++) {
        int8_t exp = x[r * h + c];
        if (r < 16 && q[c] != exp) okLo = 0;
        if (r >= 16 && q[c] != 0) { zHi = 0; nzHi++; }
      }
    }
    char b[128]; snprintf(b, sizeof b, "[1] old-load: rows0-15 %s, rows16-31 %s (nz=%d)",
                          okLo ? "==x" : "BAD", zHi ? "ALL-ZERO" : "nonzero", nzHi);
    slog(b);
  }

  /* [2] 신식 적재(16행씩 두 번) + T=24 체인 */
  grt_config_ld(&CTX, h);
  grt_mvin(&CTX, x, 0, h, 16);
  grt_mvin(&CTX, x + 16 * h, 16, h, 16);
  grt_fence();
  grt_config_st(&CTX, DIM);
  for (int tt = 0; tt < 24; tt++) tileop(50, tt, tt == 0);
  grt_mvout(&CTX, q, GRT_ACC(50), DIM, 1); grt_fence();
  { int ok = 1;
    for (int n = 0; n < 16; n++) if (q[n] != (int8_t)prange(0, 24, n)) ok = 0;
    char b[160]; int off = snprintf(b, sizeof b, "[2] T24 fixed-load: %s  y:", ok ? "PASS(=exp24)" : "FAIL");
    for (int n = 0; n < 8; n++) off += snprintf(b + off, sizeof b - off, " %d", q[n]);
    slog(b); }

  /* [3] T=32 전체 체인 */
  for (int tt = 0; tt < 32; tt++) tileop(51, tt, tt == 0);
  grt_mvout(&CTX, q, GRT_ACC(51), DIM, 1); grt_fence();
  { int ok = 1;
    for (int n = 0; n < 16; n++) if (q[n] != (int8_t)prange(0, 32, n)) ok = 0;
    char b[160]; int off = snprintf(b, sizeof b, "[3] T32 full: %s  y:", ok ? "PASS(=full)" : "FAIL");
    for (int n = 0; n < 8; n++) off += snprintf(b + off, sizeof b - off, " %d", q[n]);
    slog(b); }

  /* [4] 음성: 같은 y 를 틀린 기대와 비교 — 검사기 발화 확인 */
  { int bad = 0;
    for (int n = 0; n < 16; n++) if (q[n] != (int8_t)(prange(0, 32, n) ^ (n == 3))) bad++;
    slog(bad ? "[4] negative: FIRES ok" : "[4] negative: SILENT (검사기 고장)"); }

  slog("ssd8 ALL DONE");
  return 0;
}
