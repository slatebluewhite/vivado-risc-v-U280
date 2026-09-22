// fp32_shape.c — 형상 비대칭을 충분한 표본으로 재측정한다 (E114 후속)
// E113에서 "N(출력)이 M(입력)보다 중요"를 관찰했으나 iters=5로 표본이 작았다.
// MAC 수를 고정한 채 M·K·N을 바꿔가며, 바이어스 없이, 반복을 늘려 측정한다.
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

static void mm(size_t M,size_t K,size_t N){
  tiled_matmul_auto(M,N,K,(elem_t*)A,(elem_t*)B,NULL,(void*)C,MAXD,MAXD,MAXD,MAXD,
      MVIN_SCALE_IDENTITY,MVIN_SCALE_IDENTITY,MVIN_SCALE_IDENTITY,
      NO_ACTIVATION,ACC_SCALE_IDENTITY,0,false,false,false,false,false,0,WS);
}
static void bench(const char*tag,size_t M,size_t K,size_t N,int iters){
  mm(M,K,N);                          // 워밍업
  unsigned long best=~0UL;
  for(int rep=0;rep<3;rep++){         // 3회 측정 중 최소값 — 간섭 배제
    unsigned long s=read_cycles();
    for(int t=0;t<iters;t++) mm(M,K,N);
    unsigned long e=read_cycles();
    unsigned long per=(e-s)/iters;
    if(per<best) best=per;
  }
  double mpc=(double)M*K*N/(best*100.0);
  printf("%-14s M=%3lu K=%3lu N=%3lu  %6lu틱  %6.1f MAC/사이클  활용률 %4.1f%%\n",
         tag,(unsigned long)M,(unsigned long)K,(unsigned long)N,best,mpc,100.0*mpc/(double)(DIM*DIM));
  fflush(stdout);
}
int main(){
#ifndef BAREMETAL
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0){perror("mlockall");exit(1);}
#endif
  gemmini_flush(0);
  for(size_t i=0;i<MAXD;i++)for(size_t j=0;j<MAXD;j++){
    A[i][j]=(elem_t)((int)((i*7+j*3)%7)-3);
    B[i][j]=(elem_t)((int)((i*5+j*11)%5)-2);
  }
  printf("=== 배치 크기 의존성 (DIM=%d, 피크 %d MAC/사이클)\n=== 상세 (K=N=256, 바이어스 없음, 3세트 중 최소) ===\n", DIM, DIM*DIM);fflush(stdout);
  bench("배치 8",     8,256,256,20);
  bench("배치 16",   16,256,256,20);
  bench("배치 32",   32,256,256,15);
  bench("배치 64",   64,256,256,10);
  bench("배치 128", 128,256,256,10);
  bench("배치 256", 256,256,256,5);
  printf("=== BATCH_DONE ===\n");fflush(stdout);
  return 0;
}
