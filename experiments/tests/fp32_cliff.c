// fp32_cliff.c — 활용률 급락 지점을 찾아 스크래치패드 용량 가설을 검증한다 (E113 후속)
//
// E113: N=128에서 57.4%, N=256에서 18.3%. "행렬이 스크래치패드에 안 들어가서"라고 추정했다.
// FP32 원소는 4바이트이므로 N×N 행렬 = N²×4 바이트.
//   N=128 → 64KB,  N=160 → 100KB,  N=192 → 144KB,  N=256 → 256KB
// 스크래치패드는 256KB. 세 행렬(A·B·C)을 감안하면 N=128~160 사이에서 절벽이 예상된다.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif
#include "include/gemmini_testutils.h"
#define MAXD 256
static elem_t A[MAXD][MAXD] row_align(1), B[MAXD][MAXD] row_align(1), C[MAXD][MAXD] row_align(1);
static acc_t  Dm[MAXD][MAXD] row_align_acc(1);

static void run2(size_t N, int iters, int use_D) {
  for (size_t i=0;i<N;i++) for (size_t j=0;j<N;j++) {
    A[i][j]=(elem_t)(((int)((i*7+j*3)%15)-7)*0.125f);
    B[i][j]=(elem_t)(((int)((i*5+j*11)%13)-6)*0.25f);
  }
  tiled_matmul_auto(N,N,N,(elem_t*)A,(elem_t*)B, use_D?(void*)Dm:NULL, (void*)C,MAXD,MAXD,MAXD,MAXD,
      MVIN_SCALE_IDENTITY,MVIN_SCALE_IDENTITY,MVIN_SCALE_IDENTITY,
      NO_ACTIVATION,ACC_SCALE_IDENTITY,0,false,false,false,false,false,0,WS);
  unsigned long s=read_cycles();
  for (int t=0;t<iters;t++)
    tiled_matmul_auto(N,N,N,(elem_t*)A,(elem_t*)B, use_D?(void*)Dm:NULL, (void*)C,MAXD,MAXD,MAXD,MAXD,
        MVIN_SCALE_IDENTITY,MVIN_SCALE_IDENTITY,MVIN_SCALE_IDENTITY,
        NO_ACTIVATION,ACC_SCALE_IDENTITY,0,false,false,false,false,false,0,WS);
  unsigned long e=read_cycles();
  double ticks=(double)(e-s)/iters;
  double macs=(double)N*N*N;
  double mpc=macs/(ticks*100.0);
  printf("N=%3lu %s  %8.0f틱  %5.1f MAC/사이클  활용률 %4.1f%%\n",
         (unsigned long)N, use_D?"바이어스O":"바이어스X", ticks, mpc, 100.0*mpc/64.0);
  fflush(stdout);
}

int main(){
#ifndef BAREMETAL
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0){perror("mlockall");exit(1);}
#endif
  gemmini_flush(0);
  printf("=== FP32 바이어스 비용 (스크래치패드 %dKB) ===\n",
         (BANK_NUM*BANK_ROWS*DIM*(int)sizeof(elem_t))/1024);
  fflush(stdout);
  for (size_t i=0;i<MAXD;i++) for (size_t j=0;j<MAXD;j++) Dm[i][j]=0.5f;
  run2(128,8,0); run2(128,8,1);
  run2(192,4,0); run2(192,4,1);
  run2(256,2,0); run2(256,2,1);
  printf("=== CLIFF_DONE ===\n"); fflush(stdout);
  return 0;
}
