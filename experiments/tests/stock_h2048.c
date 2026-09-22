// stock_bert.c — 기준선: 스톡 Gemmini 라이브러리(tiled_matmul_auto)를 가속기 하나로.
//
// 산출물은 "규칙 + 2가속기 = 32.342 ms"를 말하는데, 새 사용자의 출발점은
// tiled_matmul_auto 를 그냥 부르는 것이다. 그 기준선이 층 단위로 측정된 적이 없다.
// 같은 판(62.5MHz), 같은 형상, 같은 M=128 seq.
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
#define H   2048
#define FF  8192
static elem_t X[SEQ*H]  __attribute__((aligned(64)));
static elem_t Wq[H*H]   __attribute__((aligned(64)));
static elem_t W1[H*FF]  __attribute__((aligned(64)));
static elem_t W2[FF*H]  __attribute__((aligned(64)));
static elem_t Hq[SEQ*H] __attribute__((aligned(64)));
static elem_t Hf[SEQ*FF]__attribute__((aligned(64)));
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
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/stock_h2048.log","w");
  if(!g){perror("로그");exit(1);}
  if(mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); sched_setaffinity(0,sizeof(cs),&cs); sched_yield();
  gemmini_flush(0);
  for(int i=0;i<SEQ*H;i++)  X[i]=(int8_t)(i%7-3);
  for(int i=0;i<H*H;i++)    Wq[i]=(int8_t)(i%5-2);
  for(int i=0;i<H*FF;i++)   W1[i]=(int8_t)(i%5-2);
  for(int i=0;i<FF*H;i++)   W2[i]=(int8_t)(i%5-2);
  mark("=== 기준선: 스톡 tiled_matmul_auto, 가속기 1개 (62.5MHz) ===");
  mark("hidden 2048 층의 matmul만: QKV(3회) + 출력 + FFN1 + FFN2");
  mark("%-10s %10s","단계","ms");
  double best=1e30;
  for(int r=0;r<3;r++){
    double t0=now();
    for(int q=0;q<3;q++) mm(SEQ,H,H,X,Wq,Hq);     /* QKV */
    double t1=now();
    mm(SEQ,H,H,Hq,Wq,Hq);                          /* 출력 */
    double t2=now();
    mm(SEQ,H,FF,X,W1,Hf);                          /* FFN1 */
    double t3=now();
    mm(SEQ,FF,H,Hf,W2,Hq);                         /* FFN2 */
    double t4=now();
    double tot=t4-t0;
    if(tot<best){ best=tot;
      mark("QKV(3회)   %10.3f",(t1-t0)*1e3);
      mark("출력       %10.3f",(t2-t1)*1e3);
      mark("FFN1       %10.3f",(t3-t2)*1e3);
      mark("FFN2       %10.3f",(t4-t3)*1e3);
      mark("합계       %10.3f",tot*1e3); }
  }
  mark("=== STOCKH2048_DONE ===");
  return 0; }
