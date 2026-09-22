// gateup.c — E315: 독립 matmul이 2개일 때의 배분 규칙 (Llama의 gate+up).
// 사전 등록: experiments/model/PREREG_E315.txt
//
// 규칙(SUMMARY §2-4): 독립 matmul 개수가 가속기 수의 배수면 하나씩 맡긴다.
// 근거는 전부 3개(QKV)로 쟀다. 2개는 미측정인데, 2가속기가 hidden>=1536의 권장 구성이다.
//   예측: m=2에서 하나씩(P2) > J분할(P3),  m=3에서 J분할(P4) > 하나씩+놀림(P5).
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <sys/mman.h>
#include "include/gemmini_rt.h"
static const grt_ctx AC[3] = {
  { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 },
  { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_FP32 },
  { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_C1   },
};
#define MM 128
#define KK 1024
#define NN 2816
#define FREQ 50.0e6
#define BI 8
#define BJ 4
#define KCV 16
static int8_t A[MM*KK]  __attribute__((aligned(64)));
static int8_t Wg[KK*NN] __attribute__((aligned(64)));
static int8_t Wu[KK*NN] __attribute__((aligned(64)));
static int8_t Cg[MM*NN] __attribute__((aligned(64)));
static int8_t Cu[MM*NN] __attribute__((aligned(64)));
static int8_t Rg[MM*NN], Ru[MM*NN];
static FILE *g;
static void mark(const char *f,...){ if(!g)return; va_list ap; va_start(ap,f);
  vfprintf(g,f,ap); va_end(ap); fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

// 블록 (i0,jb) 하나를 가속기 a에 발행. W/C는 인자로 받는다.
static void blk(int a,int i0,int jb,const int8_t*W,int8_t*C){
  int TI=MM/16, Ntil=NN/16;
  int j0=jb*BJ; int J=(j0+BJ<=Ntil)?BJ:(Ntil-j0);
  int I=(i0+BI<=TI)?BI:(TI-i0);
  grt_block_ksplit(&AC[a], I,J,KK/16,KCV,
                   A+(size_t)i0*16*KK, W+(size_t)j0*16,
                   C+(size_t)i0*16*NN+(size_t)j0*16, KK,NN,NN, 1);
}
static void cfg(int m){ for(int a=0;a<m;a++) grt_loop_ws_config(&AC[a], KK,NN,NN); }

// 하나씩: 가속기 a가 행렬 하나를 통째로. m대가 m개 행렬을 나눠 갖는다.
static void each(int m){
  const int8_t *W[2]={Wg,Wu}; int8_t *C[2]={Cg,Cu};
  int TI=MM/16, NB=(NN/16+BJ-1)/BJ;
  cfg(m);
  int i0[3]={0,0,0}, jb[3]={0,0,0}, done[3]={0,0,0};
  for(int a=0;a<m;a++) if(a>=2) done[a]=1;      /* 행렬이 2개뿐 */
  int left; do{ left=0;
    for(int a=0;a<m && a<2;a++){ if(done[a]) continue;
      blk(a,i0[a],jb[a],W[a],C[a]);
      i0[a]+=BI;
      if(i0[a]>=TI){ i0[a]=0; jb[a]++; if(jb[a]>=NB) done[a]=1; }
      if(!done[a]) left++; }
  }while(left);
  grt_fence();
}
// J분할: 행렬 하나를 m대가 블록 단위 라운드로빈으로. 두 행렬을 순차로.
static void jsplit(int m){
  const int8_t *W[2]={Wg,Wu}; int8_t *C[2]={Cg,Cu};
  int TI=MM/16, NB=(NN/16+BJ-1)/BJ;
  cfg(m);
  for(int w=0;w<2;w++){
    int i0[3]={0,0,0}, jb[3], done[3];
    for(int a=0;a<m;a++){ jb[a]=a; done[a]=(a>=NB); }
    int left; do{ left=0;
      for(int a=0;a<m;a++){ if(done[a]) continue;
        blk(a,i0[a],jb[a],W[w],C[w]);
        i0[a]+=BI;
        if(i0[a]>=TI){ i0[a]=0; jb[a]+=m; if(jb[a]>=NB) done[a]=1; }
        if(!done[a]) left++; }
    }while(left);
  }
  grt_fence();
}
static void fill(void){
  memset(A,0,sizeof A);
  for(int r=0;r<MM;r++){ int p=r%(KK-1); A[(size_t)r*KK+p]=1; A[(size_t)r*KK+p+1]=1; }
  for(int r=0;r<KK;r++) for(int c=0;c<NN;c++){
    Wg[(size_t)r*NN+c]=(int8_t)((r*3+c*5)%7-3);
    Wu[(size_t)r*NN+c]=(int8_t)((r*5+c*3)%7-3); }
  for(int r=0;r<MM;r++){ int p=r%(KK-1);
    for(int c=0;c<NN;c++){
      Rg[(size_t)r*NN+c]=(int8_t)(Wg[(size_t)p*NN+c]+Wg[(size_t)(p+1)*NN+c]);
      Ru[(size_t)r*NN+c]=(int8_t)(Wu[(size_t)p*NN+c]+Wu[(size_t)(p+1)*NN+c]); } }
}
static int chk(void){ int b=0;
  for(size_t i=0;i<(size_t)MM*NN;i++){ if(Cg[i]!=Rg[i]) b++; if(Cu[i]!=Ru[i]) b++; }
  return b; }
static void trial(const char*nm,void(*fn)(int),int m){
  double best=1e30; int bad=0;
  fn(m);                                     /* 웜업 */
  for(int r=0;r<5;r++){ memset(Cg,0,sizeof Cg); memset(Cu,0,sizeof Cu);
    double t0=now(); fn(m); double d=now()-t0; if(d<best)best=d; bad+=chk(); }
  double roof=2.0*MM*KK*NN/256.0/FREQ;        /* 두 matmul 합 */
  mark("%-28s m=%d | %9.3f ms | 루프라인/m %6.3f | %s",
       nm,m,best*1e3, best/(roof/m), bad?"FAIL":"PASS");
}
int main(int argc,char**argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/gateup.log","w");
  if(!g){perror("로그");exit(1);}
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  for(int a=0;a<3;a++) grt_flush_ctx(&AC[a]);
  mark("=== E315 gate+up (독립 matmul 2개) 배분 규칙, [128x1024]x[1024x2816] x2 ===");
  mark("블록 (8,4) Kc=%d 청크 %d — 규칙의 선택. 배분만 바꾼다. best-of-5",KCV,(KK/16+KCV-1)/KCV);
  fill();
  trial("P1 m=1",                jsplit,1);
  trial("P2 m=2 하나씩",          each,  2);
  trial("P3 m=2 순차 J분할",      jsplit,2);
  trial("P4 m=3 순차 J분할",      jsplit,3);
  trial("P5 m=3 하나씩(1대 놀림)", each,  3);
  mark("=== GATEUP_DONE ===");
  return 0; }
