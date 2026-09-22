// same_type.c — 중첩의 원천이 "종류가 달라서"인가 "유닛이 둘이라서"인가 (E166).
//
// 같은 종류(INT8 16x16) 둘을 한 코어에서 교대 발행한다.
// 이종(INT8+FP32)에서는 균형 시 1.7~1.8배였다(E165).
//   같은 종류도 1.7~1.8배 → 원천은 "독립 유닛 둘"
//   같은 종류는 이득 없음 → 원천은 "서로 다른 자원"
//
// 같은 종류라 균형이 자명하다 — 같은 크기면 타일 수가 같다.
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

// 둘 다 INT8 16x16, opcode만 다르다
static const grt_ctx A8 = { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 };
static const grt_ctx B8 = { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_FP32 };

#define MAXN 192
static int8_t A1[MAXN*MAXN], B1[MAXN*MAXN], C1[MAXN*MAXN] __attribute__((aligned(64)));
static int8_t A2[MAXN*MAXN], B2[MAXN*MAXN], C2[MAXN*MAXN] __attribute__((aligned(64)));

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }
static int SZ;
static void seq(void){
  grt_matmul(&A8, A1, B1, C1, SZ,SZ,SZ);
  grt_matmul(&B8, A2, B2, C2, SZ,SZ,SZ);
}
// 각 워크로드를 따로 잰다 — 순차 합계가 이상할 때 어느 쪽인지 가르기 위해
static void only1(void){ grt_matmul(&A8, A1, B1, C1, SZ,SZ,SZ); }
static void only2(void){ grt_matmul(&B8, A2, B2, C2, SZ,SZ,SZ); }
static void dual(void){
  grt_work w1={.c=&A8,.A=A1,.B=B1,.C=C1,.M=SZ,.N=SZ,.K=SZ};
  grt_work w2={.c=&B8,.A=A2,.B=B2,.C=C2,.M=SZ,.N=SZ,.K=SZ};
  grt_mm2_i8i8(&w1,&w2);
}
static void fill(void){
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++){
    A1[r*SZ+c]=(r==c)?1:0; B1[r*SZ+c]=(int8_t)((r+c)%7-3);
    A2[r*SZ+c]=(r==c)?1:0; B2[r*SZ+c]=(int8_t)((r+c)%5-2); }
}
static int chk(void){
  int b=0;
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++){
    if(C1[r*SZ+c]!=B1[r*SZ+c]) b++; if(C2[r*SZ+c]!=B2[r*SZ+c]) b++; }
  return b;
}
static double timeit(void (*fn)(void)){
  double best=1e30;
  for(int s=0;s<5;s++){ memset(C1,0,(size_t)SZ*SZ); memset(C2,0,(size_t)SZ*SZ);
    double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt; }
  return best;
}
int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/same.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&A8); grt_flush_ctx(&B8);
  mark("=== 같은 종류 둘 (INT8 16x16 × 2, cpu=%d, best-of-5) ===", sched_getcpu());
  mark("대조: 이종(INT8+FP32)은 균형 시 1.7~1.8배 (E165)");
  mark("단독 두 개를 따로 재서 중첩률을 제대로 계산한다 (2.0배 초과는 불가능)");
  int Ns[]={64,128,192};
  for (unsigned i=0;i<sizeof(Ns)/sizeof(Ns[0]);i++){
    SZ=Ns[i]; fill();
    double t1=timeit(only1), t2=timeit(only2);
    double ts=timeit(seq); int b1=chk();
    double td=timeit(dual); int b2=chk();
    double sum=t1+t2, mx=(t1>t2?t1:t2);
    mark("%6d³ %7d  단독 %.3f/%.3f  합 %.3f  순차 %.3f  교차 %.3f  중첩률 %.0f%%  %s",
         SZ, (SZ/16)*(SZ/16)*(SZ/16), t1*1e3, t2*1e3, sum*1e3, ts*1e3, td*1e3,
         (sum-td)/(sum-mx)*100.0, (b1||b2)?"FAIL":"PASS");
  }
  mark("=== SAME_DONE ===");
  return 0;
}
