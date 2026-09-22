// mm2_scale.c — 1.80배가 **크기와 무관하게** 유지되는지 (E165).
//
// E161~E164의 헤드라인은 INT8 128³ : FP32 64³ 한 점에서 나왔다.
// 균형(타일 수 동일)을 유지한 채 크기를 훑어 일반성을 확인한다.
//
// 균형 조건: (SI/16)³ == (SF/8)³  →  SI = 2*SF
//   SF=32 → SI=64   : 64타일씩
//   SF=64 → SI=128  : 512타일씩
//   SF=96 → SI=192  : 1728타일씩
//   SF=128 → SI=256 : 4096타일씩
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

#define MAXI 256
#define MAXF 128
static int8_t Ai[MAXI*MAXI], Bi[MAXI*MAXI], Ci[MAXI*MAXI] __attribute__((aligned(64)));
static float  Af[MAXF*MAXF], Bf[MAXF*MAXF], Cf[MAXF*MAXF] __attribute__((aligned(64)));

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

static int SI, SF;
static void seq(void){
  grt_matmul(&INT8, Ai, Bi, Ci, SI,SI,SI);
  grt_matmul(&FP32, Af, Bf, Cf, SF,SF,SF);
}
static void dual(void){
  grt_work w1={.c=&INT8,.A=Ai,.B=Bi,.C=Ci,.M=SI,.N=SI,.K=SI};
  grt_work w2={.c=&FP32,.A=Af,.B=Bf,.C=Cf,.M=SF,.N=SF,.K=SF};
  grt_mm2(&w1,&w2);
}
static void fill(void){
  for(int r=0;r<SI;r++) for(int c=0;c<SI;c++){ Ai[r*SI+c]=(r==c)?1:0; Bi[r*SI+c]=(int8_t)((r+c)%7-3); }
  for(int r=0;r<SF;r++) for(int c=0;c<SF;c++){ Af[r*SF+c]=(r==c)?1.0f:0.0f; Bf[r*SF+c]=(float)((r+c)%7-3); }
}
static int chk(void){
  int b=0;
  for(int r=0;r<SI;r++) for(int c=0;c<SI;c++) if(Ci[r*SI+c]!=Bi[r*SI+c]) b++;
  for(int r=0;r<SF;r++) for(int c=0;c<SF;c++) if(Cf[r*SF+c]!=Bf[r*SF+c]) b++;
  return b;
}
static double timeit(void (*fn)(void)){
  double best=1e30;
  for(int s=0;s<5;s++){ memset(Ci,0,(size_t)SI*SI); memset(Cf,0,(size_t)SF*SF*4);
    double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt; }
  return best;
}

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/mm2scale.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&INT8); grt_flush_ctx(&FP32);

  mark("=== 균형 유지하며 크기 훑기 (cpu=%d, best-of-5) ===", sched_getcpu());
  mark("%7s %7s %9s %10s %10s %8s %8s", "INT8", "FP32", "타일수", "순차ms", "교차ms", "속도", "정확성");
  int SFs[]={32,64,96,128};
  for (unsigned i=0;i<sizeof(SFs)/sizeof(SFs[0]);i++){
    SF=SFs[i]; SI=2*SF; fill();
    double ts=timeit(seq); int b1=chk();
    double td=timeit(dual); int b2=chk();
    mark("%6d³ %6d³ %9d %10.3f %10.3f %7.2f배 %8s", SI, SF, (SI/16)*(SI/16)*(SI/16),
         ts*1e3, td*1e3, ts/td, (b1||b2)?"FAIL":"PASS");
  }
  mark("=== MM2SCALE_DONE ===");
  return 0;
}
