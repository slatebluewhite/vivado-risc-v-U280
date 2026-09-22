// conc2.c — 중첩이 **어느 입도에서** 생기는가 (E156 후속).
//
// E156 1차: matmul 단위로 번갈았더니 중첩이 없었다(합계의 98.3%).
//   원인: N=64 matmul 하나가 ~384개 명령을 내는데 예약 스테이션은 훨씬 작다.
//   → 코어가 **matmul 하나를 먹이는 도중 큐가 차서 멈춘다.** 그동안 상대는 논다.
//
// 두 가지를 고친다:
//   (1) **타일 단위 교대** — 한쪽 큐가 찼을 때 코어가 다른 가속기를 먹일 수 있다
//   (2) **작업량 균형** — 1차는 FP32가 7.75배 느려 완전 중첩이어도 11%만 줄었다.
//       INT8 8타일 : FP32 1타일로 맞춘다.
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

#define SZ 64
#define NTILE 256          // 각 가속기에 낼 타일 연산 수 (충분히 크게)
#define RATIO 8            // INT8:FP32 작업량 비 — 소요 시간을 맞춘다
static int8_t Ai[SZ*SZ], Bi[SZ*SZ], Ci[SZ*SZ] __attribute__((aligned(64)));
static float  Af[SZ*SZ], Bf[SZ*SZ], Cf[SZ*SZ] __attribute__((aligned(64)));

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

// 한 타일 연산만 발행한다 (fence 없음)
static inline void tile(const grt_ctx *c, const void *A, const void *B, void *C){
  int d=c->dim, eb=c->elem_bytes;
  grt_config_ld(c, (uint64_t)d*eb);
  grt_mvin(c, A, 0, d, d);
  grt_mvin(c, B, 64, d, d);
  grt_preload(c, 64, GRT_ACC(0), d,d,d,d);
  grt_compute(c, 0, GRT_GARBAGE, d,d,d,d);
  grt_config_st(c, (uint64_t)d*eb);
  grt_mvout(c, C, GRT_ACC(0), d, d);
}
static void seq_int8(void){ for(int i=0;i<NTILE*RATIO;i++) tile(&INT8,Ai,Bi,Ci); grt_fence(); }
static void seq_fp32(void){ for(int i=0;i<NTILE;i++) tile(&FP32,Af,Bf,Cf); grt_fence(); }
// 타일 단위 교대: INT8 RATIO개마다 FP32 1개
static void inter_tile(void){
  for (int i=0;i<NTILE;i++){
    for (int k=0;k<RATIO;k++) tile(&INT8,Ai,Bi,Ci);
    tile(&FP32,Af,Bf,Cf);
  }
  grt_fence();
}
static double timeit(void (*fn)(void)){
  double best=1e30;
  for(int s=0;s<3;s++){ double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt; }
  return best;
}

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/conc2.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&INT8); grt_flush_ctx(&FP32);
  for (int i=0;i<SZ*SZ;i++){ Ai[i]=(int8_t)(i%7-3); Bi[i]=(int8_t)(i%5-2);
                             Af[i]=(float)(i%7-3);  Bf[i]=(float)(i%5-2); }

  mark("=== 타일 단위 교대 시 중첩 (cpu=%d, INT8 %d타일 : FP32 %d타일) ===",
       sched_getcpu(), NTILE*RATIO, NTILE);
  double ti=timeit(seq_int8), tf=timeit(seq_fp32), tc=timeit(inter_tile);
  mark("A) INT8만        %8.3f ms", ti*1e3);
  mark("B) FP32만        %8.3f ms", tf*1e3);
  mark("C) 타일 교대     %8.3f ms", tc*1e3);
  mark("");
  mark("   합계 A+B      %8.3f ms  (중첩 없음)", (ti+tf)*1e3);
  mark("   최대 max(A,B) %8.3f ms  (완전 중첩)", (ti>tf?ti:tf)*1e3);
  double sum=ti+tf, mx=(ti>tf?ti:tf);
  mark("   → 중첩률 %.1f%%  (0%%=합계, 100%%=최대)", (sum-tc)/(sum-mx)*100.0);
  mark("=== CONC2_DONE ===");
  return 0;
}
