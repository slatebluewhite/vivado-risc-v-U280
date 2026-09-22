// conc3.c — 중첩을 **검증과 함께** 확정한다 (E156 최종).
//
// 2차에서 중첩률 99.7%가 나왔으나 두 구멍이 있었다:
//   (1) 정확성 검증이 없다 — FP32가 조용히 안 돌았다면 "중첩"이 아니라 "미실행"이다
//   (2) 작업량이 8:1이라 완전 중첩이어도 최대 11%만 줄어든다 (해상도 부족)
//
// 이번엔 **1:1로 맞추고**(완전 중첩이면 50% 절감) **양쪽 결과를 검증한다**.
// A를 단위행렬로 두어 C == B 여야 한다 — 한쪽이라도 안 돌면 즉시 드러난다.
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

#define NT 1024                     // 각 가속기에 같은 수의 타일
static int8_t Ai[16*16], Bi[16*16], Ci[16*16] __attribute__((aligned(64)));
static float  Af[8*8],   Bf[8*8],   Cf[8*8]   __attribute__((aligned(64)));

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

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
static void clear(void){ memset(Ci,0,sizeof(Ci)); memset(Cf,0,sizeof(Cf)); }
static void seq_i(void){ for(int i=0;i<NT;i++) tile(&INT8,Ai,Bi,Ci); grt_fence(); }
static void seq_f(void){ for(int i=0;i<NT;i++) tile(&FP32,Af,Bf,Cf); grt_fence(); }
static void inter(void){ for(int i=0;i<NT;i++){ tile(&INT8,Ai,Bi,Ci); tile(&FP32,Af,Bf,Cf);} grt_fence(); }

// A가 단위행렬이므로 C == B 여야 한다
static int check_i(void){ int b=0; for(int i=0;i<16*16;i++) if(Ci[i]!=Bi[i]) b++; return b; }
static int check_f(void){ int b=0; for(int i=0;i<8*8;i++)  if(Cf[i]!=Bf[i]) b++; return b; }

static double timeit(void (*fn)(void)){
  double best=1e30;
  for(int s=0;s<3;s++){ clear(); double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt; }
  return best;
}

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/conc3.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&INT8); grt_flush_ctx(&FP32);

  for(int r=0;r<16;r++) for(int c=0;c<16;c++){ Ai[r*16+c]=(r==c)?1:0; Bi[r*16+c]=(int8_t)((r+c)%7-3); }
  for(int r=0;r<8;r++)  for(int c=0;c<8;c++) { Af[r*8+c]=(r==c)?1.0f:0.0f; Bf[r*8+c]=(float)((r+c)%7-3); }

  mark("=== 동시 가동 확정 (cpu=%d, 각 %d타일, 1:1) ===", sched_getcpu(), NT);
  double ti=timeit(seq_i); int bi=check_i();
  double tf=timeit(seq_f); int bf=check_f();
  double tc=timeit(inter); int ci=check_i(), cf=check_f();

  mark("A) INT8만      %8.3f ms   정확성 %s", ti*1e3, bi?"FAIL":"PASS");
  mark("B) FP32만      %8.3f ms   정확성 %s", tf*1e3, bf?"FAIL":"PASS");
  mark("C) 타일 교대   %8.3f ms   정확성 INT8 %s / FP32 %s", tc*1e3,
       ci?"FAIL":"PASS", cf?"FAIL":"PASS");
  mark("");
  mark("   합계 A+B    %8.3f ms  (중첩 없음)", (ti+tf)*1e3);
  mark("   최대        %8.3f ms  (완전 중첩)", (ti>tf?ti:tf)*1e3);
  double sum=ti+tf, mx=(ti>tf?ti:tf);
  mark("   → 중첩률 %.1f%%,  절감 %.1f%%", (sum-tc)/(sum-mx)*100.0, (1.0-tc/sum)*100.0);
  mark("=== CONC3_DONE ===");
  return (bi||bf||ci||cf)?1:0;
}
