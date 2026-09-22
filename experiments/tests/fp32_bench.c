// fp32_bench.c — FP32 Gemmini의 실제 워크로드 성격 벤치마크 (E112)
//
// 기존 측정: roofline(정사각 행렬), bwrate(순수 대역폭).
// 실제 신경망은 (1) 직사각 행렬 (2) 층을 연달아 (3) 바이어스 포함 이라는 성격을 갖는다.
// MLP 한 층 = C = A(batch×in) * B(in×out) + bias 를 여러 형상으로 재고,
// 층을 연달아 돌려 층간 오버헤드도 함께 본다.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif
#include "include/gemmini_testutils.h"

#define MAXD 256
static elem_t A[MAXD][MAXD] row_align(1);
static elem_t B[MAXD][MAXD] row_align(1);
static elem_t C[MAXD][MAXD] row_align(1);
static acc_t  D[MAXD][MAXD] row_align_acc(1);

static void fill(size_t r, size_t c) {
  for (size_t i=0;i<r;i++) for (size_t j=0;j<c;j++) {
    A[i][j] = (elem_t)(((int)((i*7+j*3)%15)-7) * 0.125f);
    B[i][j] = (elem_t)(((int)((i*5+j*11)%13)-6) * 0.25f);
    D[i][j] = 0.5f;
  }
}

// 한 층: C = A*B + D
static void layer(size_t M, size_t K, size_t N) {
  tiled_matmul_auto(M, N, K, (elem_t*)A, (elem_t*)B, (void*)D, (void*)C,
      MAXD, MAXD, MAXD, MAXD,
      MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY,
      NO_ACTIVATION, ACC_SCALE_IDENTITY, 0, false,
      false, false, false, false, 0, WS);
}

static void bench(const char *tag, size_t M, size_t K, size_t N, int layers) {
  fill(MAXD, MAXD);
  layer(M, K, N);                       // 워밍업
  unsigned long s = read_cycles();
  for (int l = 0; l < layers; l++) layer(M, K, N);
  unsigned long e = read_cycles();
  unsigned long ticks = (e - s) / layers;
  // MAC 수 = M*K*N. 틱 1 = 100사이클.
  double macs = (double)M * K * N;
  double mac_per_cycle = macs / (ticks * 100.0);
  printf("%-22s M=%3lu K=%3lu N=%3lu  %6lu틱/층  %5.1f MAC/사이클 (피크 64)  활용률 %4.1f%%\n",
         tag, (unsigned long)M, (unsigned long)K, (unsigned long)N, ticks,
         mac_per_cycle, 100.0*mac_per_cycle/64.0);
  fflush(stdout);
}

int main(){
#ifndef BAREMETAL
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0){perror("mlockall");exit(1);}
#endif
  gemmini_flush(0);
  printf("=== FP32 워크로드 벤치마크 (8x8 mesh, 피크 64 MAC/사이클) ===\n"); fflush(stdout);
  bench("정사각 소",      64,  64,  64,  10);
  bench("정사각 중",     128, 128, 128,  5);
  bench("정사각 대",     256, 256, 256,  2);
  bench("MLP 넓은 층",    64, 256, 256,  5);
  bench("MLP 좁은 층",   256, 256,  64,  5);
  bench("배치1 (M=8)",     8, 256, 256,  5);
  printf("=== BENCH_DONE ===\n"); fflush(stdout);
  return 0;
}
