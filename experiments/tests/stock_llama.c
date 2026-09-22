// stock_llama.c — 기준선을 두 번째 아키텍처 계열에서. E347의 곡선이 BERT 형상만의
// 성질인지 확인한다. Llama형 FFN: gate·up [1024x2816] 두 개 + down [2816x1024].
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <sys/mman.h>
#include "include/gemmini.h"
#define SEQ 128
#define H   1024
#define FF  2816
static elem_t X[SEQ*H]  __attribute__((aligned(64)));
static elem_t Wg[H*FF]  __attribute__((aligned(64)));
static elem_t Wu[H*FF]  __attribute__((aligned(64)));
static elem_t Wd[FF*H]  __attribute__((aligned(64)));
static elem_t Hg[SEQ*FF]__attribute__((aligned(64)));
static elem_t Hu[SEQ*FF]__attribute__((aligned(64)));
static elem_t Y[SEQ*H]  __attribute__((aligned(64)));
static FILE *g;
static void mark(const char *f,...){ if(!g)return; va_list ap; va_start(ap,f);
  vfprintf(g,f,ap); va_end(ap); fprintf(g,"\n"); fflush(g); fsync(fileno(g)); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }
static void mm(int M,int K,int N,const elem_t*A,const elem_t*B,elem_t*C){
  tiled_matmul_auto(M,N,K, A,B,NULL,C, K,N,N,N,
                    MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY,
                    NO_ACTIVATION, ACC_SCALE_IDENTITY, 0, false,
                    false, false, false, false, 3, WS);
}
int main(int argc,char**argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/stock_llama.log","w");
  if(!g){perror("로그");exit(1);}
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  gemmini_flush(0);
  for(int i=0;i<SEQ*H;i++) X[i]=(int8_t)(i%7-3);
  for(int i=0;i<H*FF;i++){ Wg[i]=(int8_t)(i%5-2); Wu[i]=(int8_t)(i%7-3); }
  for(int i=0;i<FF*H;i++) Wd[i]=(int8_t)(i%5-2);
  mark("=== 기준선: 스톡 tiled_matmul_auto, 1대 — Llama FFN (h%d, inter %d) ===",H,FF);
  mark("%-10s %10s","단계","ms");
  double best=1e30;
  for(int r=0;r<4;r++){
    double t0=now(); mm(SEQ,H,FF,X,Wg,Hg);
    double t1=now(); mm(SEQ,H,FF,X,Wu,Hu);
    double t2=now(); mm(SEQ,FF,H,Hg,Wd,Y);
    double t3=now();
    if(t3-t0<best){ best=t3-t0;
      mark("gate       %10.3f",(t1-t0)*1e3);
      mark("up         %10.3f",(t2-t1)*1e3);
      mark("down       %10.3f",(t3-t2)*1e3);
      mark("합계       %10.3f",best*1e3); }
  }
  mark("=== STOCKLLAMA_DONE ===");
  return 0; }
