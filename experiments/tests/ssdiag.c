// SSGemm 보드 현미경 (E554): S=2 의 "열 0만 맞고 열 1+ 가 0" 을 해부한다.
//
// 한 tile 로 줄인다: H(K)=8 (h=8, ch=1), N=32 (S=2 에서 nt=1). x = e_0 이므로
// y = W 의 0행 — 출력이 곧 "PE 에 실린 B의 0행" 이다.
//
// 단계 (전부 stdout — 호출측이 NFS 로 리다이렉트):
//  [1] W tile 을 세그먼트 배치로 mvin (뱅크 1,2) 후, **mvout-from-sp 로 되읽어**
//      의도한 값과 비교 — "mvin 이 잘못 썼다 vs preload 가 잘못 읽었다" 절단.
//  [2] shape=1 로 preload+compute, acc 뱅크 0/1 의 0행을 mvout → y 32개 전부 출력.
//  [3] shape=0 대조군 (같은 W 0..15 열, 뱅크 1만).
//  --neg: 기대값 한 바이트를 일부러 틀어 검사기 발화 확인.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include "include/gemmini_rt.h"

#define DIM 16
#define SP_BANKS 4
#define SP_ENT 4096
#define ACC_ENT 256

static const grt_ctx CTX = { DIM, 1, 4, GRT_OP_INT8 };

int main(int argc, char **argv) {
  const int neg = argc > 1 && !strcmp(argv[1], "--neg");
  const int K = 8, N = 32;                 /* S=2: h=8, w=32, 1 tile */
  const int bBase = 2048;

  if (mlockall(MCL_CURRENT | MCL_FUTURE)) { perror("mlockall"); return 2; }
  grt_flush_ctx(&CTX);   /* E554: TLB flush — 두 번째 프로세스부터의 stale 번역이 보드
                            실패(부분 0/wedge)의 전부였다. corpus 규칙의 재학습. */

  static int8_t W[8 * 32], x[8], rb[16 * 16], y[32];
  for (int k = 0; k < K; k++) for (int n = 0; n < N; n++) W[k * N + n] = (int8_t)((k + 2 * n) % 7 - 3);
  memset(x, 0, sizeof x); x[0] = 1;

  grt_config_st(&CTX, N);
  grt_config_ld(&CTX, N);
  /* W: 세그먼트 s(0,1) → 뱅크 1+s, 행 bBase.. (blk=0) */
  for (int s = 0; s < 2; s++)
    grt_mvin(&CTX, W + s * DIM, (uint32_t)((1 + s) * SP_ENT + bBase), DIM, K);
  grt_fence();

  /* [1] 되읽기: mvout stride = 16 으로 rb 에 연속 저장 */
  grt_config_st(&CTX, DIM);
  int bad1 = 0;
  for (int s = 0; s < 2; s++) {
    memset(rb, 0x55, sizeof rb);
    grt_mvout(&CTX, rb, (uint32_t)((1 + s) * SP_ENT + bBase), DIM, K);
    grt_fence();
    for (int k = 0; k < K; k++) for (int c = 0; c < DIM; c++) {
      int8_t exp = W[k * N + s * DIM + c];
      if (neg && s == 0 && k == 0 && c == 3) exp ^= 1;
      if (rb[k * DIM + c] != exp && bad1++ < 6)
        printf("[1] sp readback BAD s=%d k=%d c=%d got=%d exp=%d\n", s, k, c, rb[k * DIM + c], exp);
    }
  }
  printf("[1] sp readback: %s\n", bad1 ? "FAIL" : "PASS");

  /* [2] shape=1: preload+compute, y = W row 0 이어야 한다 */
  grt_config_ex_shape(&CTX, GRT_WS, 1);
  grt_config_ld(&CTX, K);
  grt_mvin(&CTX, x, 0, K, 1);
  grt_fence();
  grt_preload(&CTX, (uint32_t)(1 * SP_ENT + bBase), GRT_ACC(0), DIM, K, DIM, 1);
  grt_compute(&CTX, 0, GRT_GARBAGE, K, 1, 0, 0);
  grt_config_st(&CTX, DIM);
  memset(y, 0x55, sizeof y);
  grt_mvout(&CTX, y,      GRT_ACC(0),           DIM, 1);   /* seg0 = acc 뱅크 0 */
  grt_mvout(&CTX, y + 16, GRT_ACC(ACC_ENT + 0), DIM, 1);   /* seg1 = acc 뱅크 1 */
  grt_fence();
  printf("[2] sh1 y  :"); for (int n = 0; n < N; n++) printf(" %d", y[n]); printf("\n");
  printf("[2] sh1 exp:"); for (int n = 0; n < N; n++) printf(" %d", W[0 * N + n]); printf("\n");
  int bad2 = 0; for (int n = 0; n < N; n++) if (y[n] != W[n]) bad2++;
  printf("[2] shape1 preload/compute: %s (bad %d/32)\n", bad2 ? "FAIL" : "PASS", bad2);

  /* [3] shape=0 대조군: 같은 뱅크 1 의 B, 열 0..15 */
  grt_config_ex_shape(&CTX, GRT_WS, 0);
  grt_preload(&CTX, (uint32_t)(1 * SP_ENT + bBase), GRT_ACC(8), DIM, K, DIM, 1);
  grt_compute(&CTX, 0, GRT_GARBAGE, K, 1, 0, 0);
  memset(y, 0x55, 16);
  grt_mvout(&CTX, y, GRT_ACC(8), DIM, 1);
  grt_fence();
  int bad3 = 0; for (int n = 0; n < 16; n++) if (y[n] != W[n]) bad3++;
  printf("[3] sh0 y  :"); for (int n = 0; n < 16; n++) printf(" %d", y[n]); printf("\n");
  printf("[3] shape0 대조군: %s (bad %d/16)\n", bad3 ? "FAIL" : "PASS", bad3);
  return (bad1 || bad2 || bad3) ? 1 : 0;
}
