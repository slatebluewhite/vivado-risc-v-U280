// bf16_warm.c — 첫 matmul만 어긋나는지 확인한다 (E101 후속)
// 같은 곱셈을 3회 반복하며 매번 오류 개수를 센다.
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
int main(){
#ifndef BAREMETAL
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0){perror("mlockall");exit(1);}
#endif
  gemmini_flush(0);
  for(size_t i=0;i<DIM;i++)for(size_t j=0;j<DIM;j++){A[i][j]=f2b(i==j?1.0f:0.0f);B[i][j]=f2b(5.0f);}
  const int Aa=0,Ba=DIM,Ca=2*DIM;
  gemmini_config_ld(DIM*sizeof(elem_t)); gemmini_config_st(DIM*sizeof(elem_t));
  gemmini_mvin(A,Aa); gemmini_mvin(B,Ba); gemmini_fence();
  gemmini_extended_config_ex(WEIGHT_STATIONARY, NO_ACTIVATION, 0, 1, false, false);
  printf("=== BF16 WARMUP TEST (DIM=%d) 기대 5.0 ===\n", DIM); fflush(stdout);
  int N=200, bad_runs=0; unsigned long tot_bad=0, diag_hist[32]={0};
  for(int run=0;run<N;run++){
    for(size_t i=0;i<DIM;i++)for(size_t j=0;j<DIM;j++) C[i][j]=f2b(9.0f);  // 표식
    gemmini_preload_zeros(Ca);
    gemmini_compute_preloaded(Aa,Ba);
    gemmini_fence();
    gemmini_mvout(C,Ca);
    gemmini_fence();
    size_t bad=0, marker=0;
    for(size_t i=0;i<DIM;i++)for(size_t j=0;j<DIM;j++){
      float v=b2f(C[i][j]);
      if(v!=5.0f) bad++;
      if(v==9.0f) marker++;
    }
    if(bad){ bad_runs++; tot_bad+=bad;
      for(size_t i=0;i<DIM;i++)for(size_t j=0;j<DIM;j++)
        if(b2f(C[i][j])!=5.0f && (i+j)<32) diag_hist[i+j]++;
    }
  }
  printf("반복 %d회: 오류난 회차 %d (%.1f%%), 총 오류원소 %lu, 회당 평균 %.2f\n",
         N, bad_runs, 100.0*bad_runs/N, tot_bad, bad_runs?(double)tot_bad/bad_runs:0.0);
  printf("대각선 분포 (i+j):");
  for(int d=0;d<2*DIM;d++) if(diag_hist[d]) printf(" %d:%lu", d, diag_hist[d]);
  printf("\n"); fflush(stdout);
  printf("=== WARM_DONE ===\n"); fflush(stdout);
  return 0;
}
