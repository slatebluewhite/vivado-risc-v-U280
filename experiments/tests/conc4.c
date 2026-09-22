// conc4.c — "명령 발행 대역폭이 중첩의 상한"이라는 E156의 결론을 시험한다 (E157).
//
// E156: 타일 단위 교대로 중첩률 71.4%. 100%가 아닌 이유를
//       "코어 하나가 두 큐를 다 먹여야 하므로 발행 대역폭이 상한"이라고 설명했다.
//       설명은 시험하지 않으면 추측이다.
//
// 시험: 타일당 명령 수를 줄이고 중첩률이 오르는지 본다.
//   기존 타일 = 7명령 (config_ld, mvin, mvin, preload, compute, config_st, mvout)
//   축소 타일 = 5명령 (config_ld/config_st를 루프 밖으로 — 매 타일 같은 값이다)
//
// 예측: 발행 대역폭이 상한이면 (a) 절대 시간이 줄고 (b) 중첩률이 오른다.
//       상한이 다른 곳(가속기 처리량)이면 절대 시간만 조금 줄고 중첩률은 그대로다.
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

// 7명령 판 (E156과 동일)
static inline void tile7(const grt_ctx *c, const void *A, const void *B, void *C){
  int d=c->dim, eb=c->elem_bytes;
  grt_config_ld(c, (uint64_t)d*eb);
  grt_mvin(c, A, 0, d, d);  grt_mvin(c, B, 64, d, d);
  grt_preload(c, 64, GRT_ACC(0), d,d,d,d);
  grt_compute(c, 0, GRT_GARBAGE, d,d,d,d);
  grt_config_st(c, (uint64_t)d*eb);
  grt_mvout(c, C, GRT_ACC(0), d, d);
}
// 5명령 판 — config는 루프 밖에서 한 번
static inline void tile5(const grt_ctx *c, const void *A, const void *B, void *C){
  int d=c->dim;
  grt_mvin(c, A, 0, d, d);  grt_mvin(c, B, 64, d, d);
  grt_preload(c, 64, GRT_ACC(0), d,d,d,d);
  grt_compute(c, 0, GRT_GARBAGE, d,d,d,d);
  grt_mvout(c, C, GRT_ACC(0), d, d);
}
static void cfg(const grt_ctx *c){
  int d=c->dim, eb=c->elem_bytes;
  grt_config_ex(c, GRT_WS);
  grt_config_ld(c, (uint64_t)d*eb);
  grt_config_st(c, (uint64_t)d*eb);
}
static void clear(void){ memset(Ci,0,sizeof(Ci)); memset(Cf,0,sizeof(Cf)); }

static void s7i(void){ for(int i=0;i<NT;i++) tile7(&INT8,Ai,Bi,Ci); grt_fence(); }
static void s7f(void){ for(int i=0;i<NT;i++) tile7(&FP32,Af,Bf,Cf); grt_fence(); }
static void x7 (void){ for(int i=0;i<NT;i++){ tile7(&INT8,Ai,Bi,Ci); tile7(&FP32,Af,Bf,Cf);} grt_fence(); }
static void s5i(void){ cfg(&INT8); for(int i=0;i<NT;i++) tile5(&INT8,Ai,Bi,Ci); grt_fence(); }
static void s5f(void){ cfg(&FP32); for(int i=0;i<NT;i++) tile5(&FP32,Af,Bf,Cf); grt_fence(); }
static void x5 (void){ cfg(&INT8); cfg(&FP32);
  for(int i=0;i<NT;i++){ tile5(&INT8,Ai,Bi,Ci); tile5(&FP32,Af,Bf,Cf);} grt_fence(); }

static int chk(void){ int b=0;
  for(int i=0;i<16*16;i++) if(Ci[i]!=Bi[i]) b++;
  for(int i=0;i<8*8;i++)   if(Cf[i]!=Bf[i]) b++; return b; }
static double timeit(void (*fn)(void)){
  double best=1e30;
  for(int s=0;s<3;s++){ clear(); double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt; }
  return best;
}

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/conc4.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&INT8); grt_flush_ctx(&FP32);
  for(int r=0;r<16;r++) for(int c=0;c<16;c++){ Ai[r*16+c]=(r==c)?1:0; Bi[r*16+c]=(int8_t)((r+c)%7-3); }
  for(int r=0;r<8;r++)  for(int c=0;c<8;c++) { Af[r*8+c]=(r==c)?1.0f:0.0f; Bf[r*8+c]=(float)((r+c)%7-3); }

  mark("=== 타일당 명령 수와 중첩률 (cpu=%d, 각 %d타일) ===", sched_getcpu(), NT);
  double a7=timeit(s7i), b7=timeit(s7f), c7=timeit(x7); int e7=chk();
  double a5=timeit(s5i), b5=timeit(s5f), c5=timeit(x5); int e5=chk();
  double s7=a7+b7, m7=(a7>b7?a7:b7), s5=a5+b5, m5=(a5>b5?a5:b5);
  mark("[7명령/타일]  INT8 %.3f  FP32 %.3f  교대 %.3f ms   정확성 %s",
       a7*1e3,b7*1e3,c7*1e3, e7?"FAIL":"PASS");
  mark("              합계 %.3f  최대 %.3f  → 중첩률 %.1f%%, 절감 %.1f%%",
       s7*1e3,m7*1e3,(s7-c7)/(s7-m7)*100.0,(1.0-c7/s7)*100.0);
  mark("[5명령/타일]  INT8 %.3f  FP32 %.3f  교대 %.3f ms   정확성 %s",
       a5*1e3,b5*1e3,c5*1e3, e5?"FAIL":"PASS");
  mark("              합계 %.3f  최대 %.3f  → 중첩률 %.1f%%, 절감 %.1f%%",
       s5*1e3,m5*1e3,(s5-c5)/(s5-m5)*100.0,(1.0-c5/s5)*100.0);
  mark("");
  mark("명령 29%% 감소 → 교대 시간 %.1f%% 감소", (1.0-c5/c7)*100.0);
  mark("=== CONC4_DONE ===");
  return (e7||e5)?1:0;
}
