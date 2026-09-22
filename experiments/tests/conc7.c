// conc7.c — 중첩을 막는 것이 **의존 사슬**인지 가른다 (E160).
//
// E159: R=1(누적 없음) 99.7%, R=2(누적 시작) 82.4%로 급락 후 R=16에 90.9%로 회복.
//   DMA는 R이 커질수록 줄어드는데 R=1이 가장 좋다 → 메모리만으로 설명 안 된다.
//   차이는 하나: R>=2에서만 **같은 누산기 주소에 반복 누적**한다(의존 사슬).
//
// 시험: 같은 R에서 명령 수와 DMA를 **그대로 두고 의존성만** 없앤다.
//   (a) 누적:   모든 compute가 누산기 0번에 (의존 사슬)
//   (b) 독립:   compute마다 다른 누산기 주소 (사슬 없음)
//
// 예측: 의존 사슬이 원인이면 (b)의 중첩률이 (a)보다 뚜렷이 높다.
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
#define RR 4
static int8_t Ai[16*16], Bi[16*16], Ci[16*16] __attribute__((aligned(64)));
static float  Af[8*8],   Bf[8*8],   Cf[8*8]   __attribute__((aligned(64)));

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }
static int gDep;   // 1=누적(의존), 0=독립

static inline void blk(const grt_ctx *c, const void *A, const void *B, void *C){
  int d=c->dim;
  grt_mvin(c, B, 64, d, d);
  for (int k=0;k<RR;k++){
    grt_mvin(c, A, 0, d, d);
    uint32_t dst = gDep ? ((k==0)?GRT_ACC(0):GRT_ACC_ACC(0))
                        : GRT_ACC(k*d);          // ★ 독립: 주소를 분리
    grt_preload(c, 64, dst, d,d,d,d);
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
  for(int i=0;i<NITER;i++){ blk(&INT8,Ai,Bi,Ci); blk(&FP32,Af,Bf,Cf);} grt_fence(); }
static double timeit(void (*fn)(void)){
  double best=1e30;
  for(int s=0;s<3;s++){ double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt; }
  return best;
}

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/conc7.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&INT8); grt_flush_ctx(&FP32);
  for(int r=0;r<16;r++) for(int c=0;c<16;c++){ Ai[r*16+c]=(r==c)?1:0; Bi[r*16+c]=(int8_t)((r+c)%7-3); }
  for(int r=0;r<8;r++)  for(int c=0;c<8;c++) { Af[r*8+c]=(r==c)?1.0f:0.0f; Bf[r*8+c]=(float)((r+c)%7-3); }

  mark("=== 의존 사슬이 중첩을 막는가 (cpu=%d, R=%d, 블록 %d개) ===", sched_getcpu(), RR, NITER);
  mark("명령 수와 DMA는 두 조건이 동일하고, 누산기 주소만 다르다");
  for (gDep=1; gDep>=0; gDep--){
    double ti=timeit(si), tf=timeit(sf), tc=timeit(xx);
    double sum=ti+tf, mx=(ti>tf?ti:tf);
    mark("[%s] INT8 %.3f  FP32 %.3f  교대 %.3f ms  → 중첩률 %.1f%%",
         gDep?"누적(의존)":"독립  ", ti*1e3, tf*1e3, tc*1e3, (sum-tc)/(sum-mx)*100.0);
  }
  mark("=== CONC7_DONE ===");
  return 0;
}
