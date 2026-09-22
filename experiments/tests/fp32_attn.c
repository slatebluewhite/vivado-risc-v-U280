// fp32_attn.c — 어텐션 형상 벤치마크 (부동소수의 실제 용도)
//
// 트랜스포머 한 헤드의 두 행렬곱:
//   1) scores = Q · K^T   : M=seq, K=head_dim, N=seq
//   2) out    = scores · V : M=seq, K=seq,      N=head_dim
// head_dim이 작아 K 또는 N이 작은 형상이 나온다 — E115에서 K<128이 불리함을 확인했다.
// 실제 어텐션이 이 하드웨어에서 얼마나 효율적인지 본다.
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
static unsigned long best_of(size_t M,size_t K,size_t N,int iters){
  mm(M,K,N);
  unsigned long best=~0UL;
  for(int r=0;r<3;r++){
    unsigned long s=read_cycles();
    for(int t=0;t<iters;t++) mm(M,K,N);
    unsigned long e=read_cycles();
    unsigned long per=(e-s)/iters;
    if(per<best) best=per;
  }
  return best;
}
static void head(const char*tag,size_t seq,size_t hd,int iters){
  unsigned long t1=best_of(seq,hd,seq,iters);   // Q·K^T
  unsigned long t2=best_of(seq,seq,hd,iters);   // scores·V
  double m1=(double)seq*hd*seq, m2=(double)seq*seq*hd;
  double u1=100.0*m1/(t1*100.0)/(DIM*DIM), u2=100.0*m2/(t2*100.0)/(DIM*DIM);
  double utot=100.0*(m1+m2)/((t1+t2)*100.0)/(DIM*DIM);
  printf("%-18s seq=%3lu hd=%3lu | QK^T %5lu틱(%4.1f%%)  SV %5lu틱(%4.1f%%) | 헤드합 %6lu틱 활용률 %4.1f%%\n",
         tag,(unsigned long)seq,(unsigned long)hd,t1,u1,t2,u2,t1+t2,utot);
  fflush(stdout);
}
int main(){
#ifndef BAREMETAL
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0){perror("mlockall");exit(1);}
#endif
  gemmini_flush(0);
  for(size_t i=0;i<MAXD;i++)for(size_t j=0;j<MAXD;j++){
    A[i][j]=(elem_t)(((int)((i*7+j*3)%15)-7)*0.125f);
    B[i][j]=(elem_t)(((int)((i*5+j*11)%13)-6)*0.25f);
  }
  printf("=== FP32 어텐션 형상 (DIM=%d, 피크 %d MAC/사이클, 3세트 중 최소) ===\n",DIM,DIM*DIM);
  fflush(stdout);
  head("BERT-base 헤드",  128, 64, 10);   // seq=128, head_dim=64
  head("긴 시퀀스",       256, 64,  5);   // seq=256
  head("큰 헤드",         128,128, 10);   // head_dim=128
  head("짧은 시퀀스",      64, 64, 15);
  printf("=== ATTN_DONE ===\n");fflush(stdout);
  return 0;
}
