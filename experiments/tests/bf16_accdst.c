// bf16_accdst.c — mesh 결과를 "누산기"에 쓰면 0이 되는지 확인한다 (E103 후속)
// 원시 테스트는 스크래치패드를 목적지로 썼고 98% 정확했다.
// 라이브러리(sp_tiled_matmul_ws)는 누산기를 목적지로 쓰고 100% 0이다.
// 목적지만 바꿔 비교한다.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif
#include "include/gemmini_testutils.h"
static elem_t A[DIM][DIM] row_align(1), B[DIM][DIM] row_align(1), C[DIM][DIM] row_align(1);
static inline float b2f(uint16_t b){union{uint32_t u;float f;}c;c.u=((uint32_t)b)<<16;return c.f;}
static inline uint16_t f2b(float f){union{uint32_t u;float f;}c;c.f=f;uint32_t l=(c.u>>16)&1u,x=0x7fffu+l;return (uint16_t)((c.u+x)>>16);}

static void trial(const char *tag, uint32_t dst, int iters) {
  int bad_runs = 0; unsigned long tot = 0;
  for (int r = 0; r < iters; r++) {
    for (size_t i=0;i<DIM;i++) for (size_t j=0;j<DIM;j++) C[i][j]=f2b(9.0f);
    gemmini_preload_zeros(dst);
    gemmini_compute_preloaded(0, DIM);       // A@0, B@DIM
    gemmini_fence();
    gemmini_mvout(C, dst);
    gemmini_fence();
    unsigned long bad = 0;
    for (size_t i=0;i<DIM;i++) for (size_t j=0;j<DIM;j++) if (b2f(C[i][j]) != 5.0f) bad++;
    if (bad) { bad_runs++; tot += bad; }
  }
  printf("%-16s 오류회차 %d/%d, 총 오류원소 %lu/%lu, C[0][0]=%.1f\n",
         tag, bad_runs, iters, tot, (unsigned long)(DIM*DIM*(size_t)iters), b2f(C[0][0]));
  fflush(stdout);
}

int main(){
#ifndef BAREMETAL
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0){perror("mlockall");exit(1);}
#endif
  gemmini_flush(0);
  for (size_t i=0;i<DIM;i++) for (size_t j=0;j<DIM;j++){ A[i][j]=f2b(i==j?1.0f:0.0f); B[i][j]=f2b(5.0f); }
  gemmini_config_ld(DIM*sizeof(elem_t)); gemmini_config_st(DIM*sizeof(elem_t));
  gemmini_mvin(A, 0); gemmini_mvin(B, DIM); gemmini_fence();
  gemmini_extended_config_ex(WEIGHT_STATIONARY, NO_ACTIVATION, 0, 1, false, false);

  printf("=== BF16 목적지 비교 (DIM=%d) 기대 5.0 ===\n", DIM); fflush(stdout);
  trial("스크래치패드", 2*DIM, 100);
  trial("누산기(덮어쓰기)", ((uint32_t)1 << (ADDR_LEN-1)), 100);
  // 라이브러리가 실제로 쓰는 주소: 비트31(누산기) + 비트30(누적 모드)
  trial("누산기(누적모드)", ((uint32_t)3 << (ADDR_LEN-2)), 100);
  printf("=== ACCDST_DONE ===\n"); fflush(stdout);
  return 0;
}
