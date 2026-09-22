// bf16_tiled.c — 라이브러리 경로(tiled_matmul_auto)의 오류를 크기별로 본다 (E103 후속)
// E97: SZ=16에서 결과가 전부 0. 원시 경로는 2%만 4원소 오류(E103).
// 단일 타일(SZ=DIM)이면 원시 경로와 같아야 한다 — 크기를 늘리며 어디서 깨지는지 본다.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif
#include "include/gemmini_testutils.h"
#define MAXSZ 32
static elem_t A[MAXSZ][MAXSZ] row_align(1), B[MAXSZ][MAXSZ] row_align(1), C[MAXSZ][MAXSZ] row_align(1);
static inline float b2f(uint16_t b){union{uint32_t u;float f;}c;c.u=((uint32_t)b)<<16;return c.f;}
static inline uint16_t f2b(float f){union{uint32_t u;float f;}c;c.f=f;uint32_t l=(c.u>>16)&1u,x=0x7fffu+l;return (uint16_t)((c.u+x)>>16);}

static void test(size_t SZ, int iters) {
  // A=단위행렬, B=5.0 → C 전 원소 5.0
  for (size_t i=0;i<SZ;i++) for (size_t j=0;j<SZ;j++) {
    A[i][j]=f2b(i==j?1.0f:0.0f); B[i][j]=f2b(5.0f);
  }
  int bad_runs=0; unsigned long tot=0;
  for (int r=0;r<iters;r++) {
    for (size_t i=0;i<SZ;i++) for (size_t j=0;j<SZ;j++) C[i][j]=f2b(9.0f);
    tiled_matmul_auto(SZ,SZ,SZ,(elem_t*)A,(elem_t*)B,NULL,(elem_t*)C,
        MAXSZ,MAXSZ,MAXSZ,MAXSZ,
        MVIN_SCALE_IDENTITY,MVIN_SCALE_IDENTITY,MVIN_SCALE_IDENTITY,
        NO_ACTIVATION,ACC_SCALE_IDENTITY,0,false,false,false,false,false,0,WS);
    unsigned long bad=0;
    for (size_t i=0;i<SZ;i++) for (size_t j=0;j<SZ;j++) if (b2f(C[i][j])!=5.0f) bad++;
    if (bad) { bad_runs++; tot+=bad; }
  }
  printf("SZ=%2lu (%lu타일): 오류회차 %d/%d, 총 오류원소 %lu / %lu  C[0][0]=%.1f\n",
         (unsigned long)SZ, (unsigned long)((SZ/DIM)*(SZ/DIM)), bad_runs, iters,
         tot, (unsigned long)(SZ*SZ*(size_t)iters), b2f(C[0][0]));
  fflush(stdout);
}

int main(){
#ifndef BAREMETAL
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0){perror("mlockall");exit(1);}
#endif
  gemmini_flush(0);
  printf("=== BF16 TILED (DIM=%d) 기대 5.0 ===\n", DIM); fflush(stdout);
  test(DIM,   100);      // 단일 타일
  test(2*DIM, 50);       // 2x2 타일
  test(4*DIM, 20);       // 4x4 타일
  printf("=== TILED_DONE ===\n"); fflush(stdout);
  return 0;
}
