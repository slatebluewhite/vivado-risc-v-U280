// mm2_test.c — 일반화된 primitive `grt_mm2`를 검증한다 (E163).
//
// E161은 손으로 짠 교대 루프로 1.80배를 보였다. 재사용 가능하려면
// **형상이 다른 두 워크로드를 받는 primitive**가 같은 결과를 내야 한다.
//
// 세 지점을 잰다:
//   불균형 (INT8 64³ : FP32 64³)  → 타일 64 : 512, 이득이 작아야 한다
//   균형   (INT8 128³ : FP32 64³) → 타일 512 : 512, 1.8배가 나와야 한다
//   과균형 (INT8 192³ : FP32 64³) → 타일 1728 : 512, 다시 이득이 준다
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

#define MAXI 192
#define SF   64
static int8_t Ai[MAXI*MAXI], Bi[MAXI*MAXI], Ci[MAXI*MAXI] __attribute__((aligned(64)));
static float  Af[SF*SF], Bf[SF*SF], Cf[SF*SF] __attribute__((aligned(64)));

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

static int gSI;
static void seq(void){
  grt_matmul(&INT8, Ai, Bi, Ci, gSI,gSI,gSI);
  grt_matmul(&FP32, Af, Bf, Cf, SF,SF,SF);
}
static void dual(void){
  grt_work w1 = { .c=&INT8, .A=Ai, .B=Bi, .C=Ci, .M=gSI, .N=gSI, .K=gSI };
  grt_work w2 = { .c=&FP32, .A=Af, .B=Bf, .C=Cf, .M=SF,  .N=SF,  .K=SF  };
  grt_mm2(&w1, &w2);
}
static int chk(void){
  int b=0;
  for(int r=0;r<gSI;r++) for(int c=0;c<gSI;c++) if(Ci[r*gSI+c]!=Bi[r*gSI+c]) b++;
  for(int i=0;i<SF*SF;i++) if(Cf[i]!=Bf[i]) b++;
  return b;
}
static void fill(void){
  for(int r=0;r<gSI;r++) for(int c=0;c<gSI;c++){ Ai[r*gSI+c]=(r==c)?1:0; Bi[r*gSI+c]=(int8_t)((r+c)%7-3); }
  for(int r=0;r<SF;r++)  for(int c=0;c<SF;c++) { Af[r*SF+c]=(r==c)?1.0f:0.0f; Bf[r*SF+c]=(float)((r+c)%7-3); }
}
static double timeit(void (*fn)(void)){
  double best=1e30;
  for(int s=0;s<5;s++){ memset(Ci,0,(size_t)gSI*gSI); memset(Cf,0,sizeof(Cf));
    double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt; }
  return best;
}

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/mm2.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&INT8); grt_flush_ctx(&FP32);

  mark("=== 일반화 primitive grt_mm2 (cpu=%d, FP32는 %d³=%d타일 고정, best-of-5) ===",
       sched_getcpu(), SF, (SF/8)*(SF/8)*(SF/8));
  mark("%6s %10s %10s %10s %9s %8s", "INT8 N", "INT8타일", "순차ms", "교차ms", "속도", "정확성");
  int Ns[]={64,128,192};
  for (unsigned i=0;i<sizeof(Ns)/sizeof(Ns[0]);i++){
    gSI=Ns[i]; fill();
    double ts=timeit(seq); int b1=chk();
    double td=timeit(dual); int b2=chk();
    mark("%6d %10d %10.3f %10.3f %8.2f배 %8s", gSI, (gSI/16)*(gSI/16)*(gSI/16),
         ts*1e3, td*1e3, ts/td, (b1||b2)?"FAIL":"PASS");
  }
  mark("=== MM2_DONE ===");
  return 0;
}
