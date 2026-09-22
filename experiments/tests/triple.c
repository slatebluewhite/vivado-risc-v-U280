// triple.c — 가속기 **3개**의 교차 발행 (E167). 2개가 1.79~1.91배였다.
// 선형이면 2.8배, 포화하면 그 지점이 이 SoC의 한계다.
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

static const grt_ctx C3 = { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 };
static const grt_ctx C2 = { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_FP32 };
static const grt_ctx C1 = { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_C1   };

#define MAXN 128
static int8_t A1[MAXN*MAXN],B1[MAXN*MAXN],D1[MAXN*MAXN] __attribute__((aligned(64)));
static int8_t A2[MAXN*MAXN],B2[MAXN*MAXN],D2[MAXN*MAXN] __attribute__((aligned(64)));
static int8_t A3[MAXN*MAXN],B3[MAXN*MAXN],D3[MAXN*MAXN] __attribute__((aligned(64)));
static int SZ;

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

static void one(void){ grt_matmul(&C3,A1,B1,D1,SZ,SZ,SZ); }
static void seq3(void){ grt_matmul(&C3,A1,B1,D1,SZ,SZ,SZ);
                        grt_matmul(&C2,A2,B2,D2,SZ,SZ,SZ);
                        grt_matmul(&C1,A3,B3,D3,SZ,SZ,SZ); }
static void x2(void){
  grt_work w1={.c=&C3,.A=A1,.B=B1,.C=D1,.M=SZ,.N=SZ,.K=SZ};
  grt_work w2={.c=&C2,.A=A2,.B=B2,.C=D2,.M=SZ,.N=SZ,.K=SZ};
  grt_mm2_i8i8(&w1,&w2);
}
static void x3(void){
  grt_work w1={.c=&C3,.A=A1,.B=B1,.C=D1,.M=SZ,.N=SZ,.K=SZ};
  grt_work w2={.c=&C2,.A=A2,.B=B2,.C=D2,.M=SZ,.N=SZ,.K=SZ};
  grt_work w3={.c=&C1,.A=A3,.B=B3,.C=D3,.M=SZ,.N=SZ,.K=SZ};
  grt_mm3_i8(&w1,&w2,&w3);
}
static void fill(void){
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++){
    A1[r*SZ+c]=(r==c)?1:0; B1[r*SZ+c]=(int8_t)((r+c)%7-3);
    A2[r*SZ+c]=(r==c)?1:0; B2[r*SZ+c]=(int8_t)((r+c)%5-2);
    A3[r*SZ+c]=(r==c)?1:0; B3[r*SZ+c]=(int8_t)((r+c)%3-1); }
}
static int chk3(void){ int b=0;
  for(int r=0;r<SZ;r++) for(int c=0;c<SZ;c++){
    if(D1[r*SZ+c]!=B1[r*SZ+c])b++; if(D2[r*SZ+c]!=B2[r*SZ+c])b++;
    if(D3[r*SZ+c]!=B3[r*SZ+c])b++; } return b; }
static void clr(void){ memset(D1,0,(size_t)SZ*SZ);memset(D2,0,(size_t)SZ*SZ);memset(D3,0,(size_t)SZ*SZ); }
static double timeit(void (*fn)(void)){
  double best=1e30;
  for(int s=0;s<5;s++){ clr(); double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt; }
  return best;
}
int main(int argc,char**argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/triple.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&C3); grt_flush_ctx(&C2); grt_flush_ctx(&C1);
  mark("=== 가속기 3개 교차 발행 (cpu=%d, best-of-5) ===", sched_getcpu());
  mark("대조: 2개는 1.79~1.91배");
  int Ns[]={64,128};
  for(unsigned i=0;i<2;i++){
    SZ=Ns[i]; fill();
    double t1=timeit(one), t3=timeit(seq3), a2=timeit(x2), a3=timeit(x3);
    int bad=chk3();
    mark("%4d³  단독 %.3f  순차3 %.3f  교차2 %.3f  교차3 %.3f ms", SZ,t1*1e3,t3*1e3,a2*1e3,a3*1e3);
    mark("       3개 속도 %.2f배 (이론최대 3.00), 2개 속도 %.2f배  정확성 %s",
         t3/a3, (t1*2)/a2, bad?"FAIL":"PASS");
  }
  mark("=== TRIPLE_DONE ===");
  return 0;
}
