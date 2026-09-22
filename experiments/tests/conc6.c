// conc6.c — **연산 밀도와 중첩률의 관계**를 곡선으로 (E159).
//
// E156~E158로 알아낸 것:
//   타일 단위 교대, DMA 있음 → 71.4%   /   DMA 없음 → 100.6%
//   상한은 메모리 대역폭이고, 타일 시간의 74%가 데이터 이동이다.
//
// 그렇다면 **연산 밀도를 얼마나 올려야 중첩 이득이 다 나오는가?**
// 재사용 계수 R을 훑어 곡선을 그린다:
//   mvin B (1회) → R × [mvin A, preload, compute(누적)] → mvout (1회)
//   compute 하나당 DMA = (2+R)/R  →  R=1이면 3, R=16이면 1.125
//
// 정확성: A가 단위행렬이고 R번 누적하므로 C == R*B 여야 한다.
//   (B가 [-3,3]이라 R=16에서도 48로 int8 범위 안이다)
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

static const grt_ctx INT8 = { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 };
static const grt_ctx FP32 = { .dim= 8, .elem_bytes=4, .acc_bytes=4, .opcode=GRT_OP_FP32 };

#define NITER 256
static int8_t Ai[16*16], Bi[16*16], Ci[16*16] __attribute__((aligned(64)));
static float  Af[8*8],   Bf[8*8],   Cf[8*8]   __attribute__((aligned(64)));

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

static int gR;
// 재사용 R짜리 블록 하나
static inline void blk(const grt_ctx *c, const void *A, const void *B, void *C){
  int d=c->dim;
  grt_mvin(c, B, 64, d, d);                       // B 한 번만
  for (int k=0;k<gR;k++){
    grt_mvin(c, A, 0, d, d);
    grt_preload(c, 64, (k==0)?GRT_ACC(0):GRT_ACC_ACC(0), d,d,d,d);
    grt_compute(c, 0, GRT_GARBAGE, d,d,d,d);
  }
  grt_mvout(c, C, GRT_ACC(0), d, d);
}
static void cfg(const grt_ctx *c){
  int d=c->dim, eb=c->elem_bytes;
  grt_config_ex(c, GRT_WS); grt_config_ld(c,(uint64_t)d*eb); grt_config_st(c,(uint64_t)d*eb);
}
static void si(void){ cfg(&INT8); for(int i=0;i<NITER;i++) blk(&INT8,Ai,Bi,Ci); grt_fence(); }
static void sf(void){ cfg(&FP32); for(int i=0;i<NITER;i++) blk(&FP32,Af,Bf,Cf); grt_fence(); }
static void xx(void){ cfg(&INT8); cfg(&FP32);
  for(int i=0;i<NITER;i++){ blk(&INT8,Ai,Bi,Ci); blk(&FP32,Af,Bf,Cf); } grt_fence(); }
static double timeit(void (*fn)(void)){
  double best=1e30;
  for(int s=0;s<3;s++){ double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt; }
  return best;
}
static int chk(void){
  int bad=0;
  for(int i=0;i<16*16;i++) if(Ci[i]!=(int8_t)(gR*Bi[i])) bad++;
  for(int i=0;i<8*8;i++)   if(Cf[i]!=(float)gR*Bf[i])    bad++;
  return bad;
}

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/conc6.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&INT8); grt_flush_ctx(&FP32);
  for(int r=0;r<16;r++) for(int c=0;c<16;c++){ Ai[r*16+c]=(r==c)?1:0; Bi[r*16+c]=(int8_t)((r+c)%7-3); }
  for(int r=0;r<8;r++)  for(int c=0;c<8;c++) { Af[r*8+c]=(r==c)?1.0f:0.0f; Bf[r*8+c]=(float)((r+c)%7-3); }

  mark("=== 연산 밀도와 중첩률 (cpu=%d, 블록 %d개) ===", sched_getcpu(), NITER);
  mark("%3s %10s %9s %9s %9s %9s %8s", "R", "DMA/연산", "INT8ms", "FP32ms", "교대ms", "중첩률", "정확성");
  int Rs[]={1,2,4,8,16};
  for (unsigned i=0;i<sizeof(Rs)/sizeof(Rs[0]);i++){
    gR=Rs[i];
    double ti=timeit(si), tf=timeit(sf), tc=timeit(xx);
    int bad=chk();
    double sum=ti+tf, mx=(ti>tf?ti:tf);
    mark("%3d %10.3f %9.3f %9.3f %9.3f %8.1f%% %8s", gR, (2.0+gR)/gR,
         ti*1e3, tf*1e3, tc*1e3, (sum-tc)/(sum-mx)*100.0, bad?"FAIL":"PASS");
  }
  mark("=== CONC6_DONE ===");
  return 0;
}
