// quant_scale.c — E149의 예측을 검증한다 (E150).
//
// 예측: 변환은 N^2로, matmul은 N^3로 늘어나므로 양자화 부담이 대략 1/N로 준다.
//       N=64에서 12.1배였으므로 N=256이면 약 3배여야 한다.
//
// 주의(E149에서 당한 것): 결과를 소비하지 않으면 컴파일러가 루프를 지운다.
//       여기서는 체크섬을 누적해 출력한다.
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

#define MAXN 256
#define MAXE (MAXN*MAXN)
static int32_t Acc[MAXE];
static uint8_t q1[MAXE], q2[MAXE], q3[MAXE*4] __attribute__((aligned(64)));
static volatile int64_t sink;      // 결과 소비용 — 죽은 코드 제거 방지

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

static int gN;
static void mm(void){ grt_matmul(&INT8, q1, q2, q3, gN, gN, gN); }
static void requant(void){
  const int32_t mult = 1518500250; const int shift = 31;
  int64_t chk = 0; int ne = gN*gN;
  for (int i=0;i<ne;i++){
    int64_t v = ((int64_t)Acc[i]*mult) >> shift;
    if(v>127)v=127; if(v<-128)v=-128;
    ((int8_t*)q1)[i]=(int8_t)v; chk += v;
  }
  sink = chk;                       // ★ 반드시 소비한다
}
static double timeit(void (*fn)(void), int it){
  double best=1e30;
  for(int s=0;s<3;s++){ double t0=now(); for(int i=0;i<it;i++) fn();
    double dt=(now()-t0)/it; if(dt<best)best=dt; }
  return best;
}

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/qscale.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&INT8);

  unsigned long rs=12345;
  for (int i=0;i<MAXE;i++){ rs=rs*6364136223846793005UL+1442695040888963407UL;
    Acc[i]=(int32_t)((rs>>33)&0xFFFF)-32768; ((int8_t*)q1)[i]=(int8_t)(i%17-8);
    ((int8_t*)q2)[i]=(int8_t)(i%13-6); }

  mark("=== 양자화 부담의 N 의존성 (예측: 대략 1/N) ===");
  mark("%5s %11s %13s %9s %10s", "N", "matmul ms", "재양자화 ms", "비", "예측(12.1*64/N)");
  int sizes[]={32,64,128,256};
  for (unsigned si=0; si<sizeof(sizes)/sizeof(sizes[0]); si++){
    gN = sizes[si];
    int it = gN<=64 ? 10 : (gN<=128 ? 5 : 3);
    double tm = timeit(mm, it);
    double tq = timeit(requant, it);
    mark("%5d %11.3f %13.3f %8.2f배 %9.2f배", gN, tm*1e3, tq*1e3, tq/tm, 12.1*64.0/gN);
  }
  mark("(체크섬 %lld — 최적화 제거 방지용)", (long long)sink);
  mark("=== QSCALE_DONE ===");
  return 0;
}
