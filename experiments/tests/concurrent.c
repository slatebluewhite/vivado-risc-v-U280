// concurrent.c — **한 코어의 두 가속기가 동시에 도는가** (E156).
//
// E140은 **순차 전환** 비용을 쟀고 0이었다. 동시 가동은 다른 질문이다.
// 되면 이 구성의 의미가 "정밀도 선택지"에서 "실제 병렬 처리량"으로 바뀐다.
//
// 구조적으로 답이 자명하지 않다:
//   - Gemmini는 예약 스테이션이 있어 명령을 큐에 넣고 비동기 처리한다
//     → INT8 명령을 쏟아넣고 이어서 FP32를 넣으면 두 어레이가 겹칠 수 있다
//   - 그러나 코어가 하나라 **명령 발행 자체가 직렬화**되고, fence는 하트를 막는다
//
// 총 작업량을 고정하고 fence 위치만 바꾼다:
//   A) INT8 N회 + fence            → T_i
//   B) FP32 N회 + fence            → T_f
//   C) 번갈아 발행 후 fence 한 번  → T_c
//
// T_c ≈ max(T_i,T_f) → 완전 중첩 / T_c ≈ T_i+T_f → 중첩 없음.
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

#define N 64
#define REP 10
static int8_t Ai[N*N], Bi[N*N], Ci[N*N] __attribute__((aligned(64)));
static float  Af[N*N], Bf[N*N], Cf[N*N] __attribute__((aligned(64)));

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

static void only_int8(void){
  for (int i=0;i<REP;i++) grt_matmul_nf(&INT8, Ai, Bi, Ci, N,N,N);
  grt_fence();
}
static void only_fp32(void){
  for (int i=0;i<REP;i++) grt_matmul_nf(&FP32, Af, Bf, Cf, N,N,N);
  grt_fence();
}
static void interleaved(void){
  for (int i=0;i<REP;i++){
    grt_matmul_nf(&INT8, Ai, Bi, Ci, N,N,N);
    grt_matmul_nf(&FP32, Af, Bf, Cf, N,N,N);
  }
  grt_fence();
}
static double timeit(void (*fn)(void)){
  double best=1e30;
  for(int s=0;s<3;s++){ double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt; }
  return best;
}

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/conc.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&INT8); grt_flush_ctx(&FP32);

  for (int i=0;i<N*N;i++){ Ai[i]=(int8_t)(i%7-3); Bi[i]=(int8_t)(i%5-2);
                           Af[i]=(float)(i%7-3);  Bf[i]=(float)(i%5-2); }

  mark("=== 두 가속기 동시 가동 여부 (cpu=%d, %d^3 x %d회) ===", sched_getcpu(), N, REP);
  double ti=timeit(only_int8), tf=timeit(only_fp32), tc=timeit(interleaved);
  mark("A) INT8만          %8.3f ms", ti*1e3);
  mark("B) FP32만          %8.3f ms", tf*1e3);
  mark("C) 번갈아 발행     %8.3f ms", tc*1e3);
  mark("");
  mark("   합계 A+B        %8.3f ms   (중첩 없음이면 이 값)", (ti+tf)*1e3);
  mark("   최대 max(A,B)   %8.3f ms   (완전 중첩이면 이 값)", (ti>tf?ti:tf)*1e3);
  double overlap = ((ti+tf) - tc) / (ti<tf?ti:tf) * 100.0;
  mark("   → C는 합계의 %.1f%%, 중첩률 약 %.1f%%", tc/(ti+tf)*100.0, overlap<0?0.0:overlap);
  mark("=== CONC_DONE ===");
  return 0;
}
