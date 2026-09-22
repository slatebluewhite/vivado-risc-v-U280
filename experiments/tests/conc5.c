// conc5.c — 중첩의 상한이 **메모리 대역폭**인지 가른다 (E158).
//
// E156은 "명령 발행 대역폭이 상한"이라 했으나 E157에서 **틀린 것으로 밝혀졌다**
// (명령 29% 감소에도 시간·중첩률 무변화).
//
// 남은 유력 후보: 두 가속기가 **같은 TileLink/DDR 경로를 공유**한다.
// 타일마다 mvin 2 + mvout 1 = 768바이트씩 오간다.
//
// 시험: **DMA 없는 타일**로 재본다.
//   스크래치패드에 A·B를 한 번만 올리고, 루프에서는 preload+compute만 돈다(2명령, DMA 0).
//   마지막에 한 번 mvout 해서 정확성도 확인한다.
//
// 예측: 메모리 대역폭이 상한이었다면 중첩률이 100%에 근접한다.
//       그대로 ~71%면 상한은 다른 곳(예약 스테이션 공유, 코어 발행 직렬화)이다.
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

#define NT 1024
static int8_t Ai[16*16], Bi[16*16], Ci[16*16] __attribute__((aligned(64)));
static float  Af[8*8],   Bf[8*8],   Cf[8*8]   __attribute__((aligned(64)));

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

// 스크래치패드에 A,B를 올려두고 설정까지 끝낸다
static void setup(const grt_ctx *c, const void *A, const void *B){
  int d=c->dim, eb=c->elem_bytes;
  grt_config_ex(c, GRT_WS);
  grt_config_ld(c, (uint64_t)d*eb);
  grt_config_st(c, (uint64_t)d*eb);
  grt_mvin(c, A, 0, d, d);
  grt_mvin(c, B, 64, d, d);
  grt_fence();
}
// DMA 없는 타일: preload + compute 만 (2명령)
static inline void tile_nodma(const grt_ctx *c){
  int d=c->dim;
  grt_preload(c, 64, GRT_ACC(0), d,d,d,d);
  grt_compute(c, 0, GRT_GARBAGE, d,d,d,d);
}
static void finish(const grt_ctx *c, void *C){
  int d=c->dim; grt_mvout(c, C, GRT_ACC(0), d, d); grt_fence();
}

static void s_i(void){ for(int i=0;i<NT;i++) tile_nodma(&INT8); grt_fence(); }
static void s_f(void){ for(int i=0;i<NT;i++) tile_nodma(&FP32); grt_fence(); }
static void x_if(void){ for(int i=0;i<NT;i++){ tile_nodma(&INT8); tile_nodma(&FP32);} grt_fence(); }

static double timeit(void (*fn)(void)){
  double best=1e30;
  for(int s=0;s<3;s++){ double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt; }
  return best;
}

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/conc5.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&INT8); grt_flush_ctx(&FP32);
  for(int r=0;r<16;r++) for(int c=0;c<16;c++){ Ai[r*16+c]=(r==c)?1:0; Bi[r*16+c]=(int8_t)((r+c)%7-3); }
  for(int r=0;r<8;r++)  for(int c=0;c<8;c++) { Af[r*8+c]=(r==c)?1.0f:0.0f; Bf[r*8+c]=(float)((r+c)%7-3); }

  setup(&INT8, Ai, Bi); setup(&FP32, Af, Bf);
  mark("=== DMA 없는 타일에서의 중첩 (cpu=%d, 각 %d타일, preload+compute만) ===",
       sched_getcpu(), NT);
  double ti=timeit(s_i), tf=timeit(s_f), tc=timeit(x_if);

  // 정확성: A가 단위행렬이므로 C == B
  memset(Ci,0,sizeof(Ci)); memset(Cf,0,sizeof(Cf));
  finish(&INT8, Ci); finish(&FP32, Cf);
  int bad=0;
  for(int i=0;i<16*16;i++) if(Ci[i]!=Bi[i]) bad++;
  for(int i=0;i<8*8;i++)   if(Cf[i]!=Bf[i]) bad++;

  double sum=ti+tf, mx=(ti>tf?ti:tf);
  mark("A) INT8만      %8.3f ms", ti*1e3);
  mark("B) FP32만      %8.3f ms", tf*1e3);
  mark("C) 교대        %8.3f ms   정확성 %s", tc*1e3, bad?"FAIL":"PASS");
  mark("   합계 %.3f  최대 %.3f  → 중첩률 %.1f%%, 절감 %.1f%%",
       sum*1e3, mx*1e3, (sum-tc)/(sum-mx)*100.0, (1.0-tc/sum)*100.0);
  mark("");
  mark("대조: DMA 있는 타일(E156)은 중첩률 71.4%%였다");
  mark("=== CONC5_DONE ===");
  return bad?1:0;
}
