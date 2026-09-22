// mm2_cmp.c — 손으로 짠 교대 루프와 일반화 primitive를 **같은 바이너리에서** 비교한다 (E164).
//
// E161(손으로 짠 루프): 512:512 균형에서 1.80배
// E163(grt_mm2 일반화):  같은 조건에서 1.23배
// 순차 기준선은 일치했으므로(2.360 vs 2.358) 측정 환경이 아니라 **구현 차이**다.
// 두 구현이 논리적으로 동등해 보이므로, 같은 실행에서 번갈아 재서 확정한다.
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

#define SI 128
#define SF 64
static int8_t Ai[SI*SI], Bi[SI*SI], Ci[SI*SI] __attribute__((aligned(64)));
static float  Af[SF*SF], Bf[SF*SF], Cf[SF*SF] __attribute__((aligned(64)));

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

static void seq(void){
  grt_matmul(&INT8, Ai, Bi, Ci, SI,SI,SI);
  grt_matmul(&FP32, Af, Bf, Cf, SF,SF,SF);
}
// (a) E161의 손으로 짠 루프 — 본문이 루프 안에 인라인돼 있다
static void hand(void){
  const int d1=INT8.dim, e1=INT8.elem_bytes, d2=FP32.dim, e2=FP32.elem_bytes;
  grt_config_ex(&INT8, GRT_WS); grt_config_ex(&FP32, GRT_WS);
  int i1=0,j1=0,k1=0,i2=0,j2=0,k2=0,D1=0,D2=0;
  while(!D1||!D2){
    if(!D1){
      grt_config_ld(&INT8,(uint64_t)SI*e1); grt_mvin(&INT8, Ai+(size_t)i1*SI+k1, 0, d1,d1);
      grt_config_ld(&INT8,(uint64_t)SI*e1); grt_mvin(&INT8, Bi+(size_t)k1*SI+j1, 64, d1,d1);
      grt_preload(&INT8,64,(k1==0)?GRT_ACC(0):GRT_ACC_ACC(0),d1,d1,d1,d1);
      grt_compute(&INT8,0,GRT_GARBAGE,d1,d1,d1,d1);
      k1+=d1; if(k1>=SI){ k1=0; grt_config_st(&INT8,(uint64_t)SI*e1);
        grt_mvout(&INT8, Ci+(size_t)i1*SI+j1, GRT_ACC(0), d1,d1);
        j1+=d1; if(j1>=SI){ j1=0; i1+=d1; if(i1>=SI) D1=1; } } }
    if(!D2){
      grt_config_ld(&FP32,(uint64_t)SF*e2); grt_mvin(&FP32, Af+(size_t)i2*SF+k2, 0, d2,d2);
      grt_config_ld(&FP32,(uint64_t)SF*e2); grt_mvin(&FP32, Bf+(size_t)k2*SF+j2, 64, d2,d2);
      grt_preload(&FP32,64,(k2==0)?GRT_ACC(0):GRT_ACC_ACC(0),d2,d2,d2,d2);
      grt_compute(&FP32,0,GRT_GARBAGE,d2,d2,d2,d2);
      k2+=d2; if(k2>=SF){ k2=0; grt_config_st(&FP32,(uint64_t)SF*e2);
        grt_mvout(&FP32, Cf+(size_t)i2*SF+j2, GRT_ACC(0), d2,d2);
        j2+=d2; if(j2>=SF){ j2=0; i2+=d2; if(i2>=SF) D2=1; } } }
  }
  grt_fence();
}
// (b) 일반화 primitive
static void prim(void){
  grt_work w1 = { .c=&INT8, .A=Ai, .B=Bi, .C=Ci, .M=SI, .N=SI, .K=SI };
  grt_work w2 = { .c=&FP32, .A=Af, .B=Bf, .C=Cf, .M=SF, .N=SF, .K=SF };
  grt_mm2(&w1, &w2);
}
static int chk(void){
  int b=0;
  for(int i=0;i<SI*SI;i++) if(Ci[i]!=Bi[i]) b++;
  for(int i=0;i<SF*SF;i++) if(Cf[i]!=Bf[i]) b++;
  return b;
}
static double timeit(void (*fn)(void)){
  double best=1e30;
  for(int s=0;s<5;s++){ memset(Ci,0,sizeof(Ci)); memset(Cf,0,sizeof(Cf));
    double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt; }
  return best;
}

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/mm2cmp.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&INT8); grt_flush_ctx(&FP32);
  for(int r=0;r<SI;r++) for(int c=0;c<SI;c++){ Ai[r*SI+c]=(r==c)?1:0; Bi[r*SI+c]=(int8_t)((r+c)%7-3); }
  for(int r=0;r<SF;r++) for(int c=0;c<SF;c++){ Af[r*SF+c]=(r==c)?1.0f:0.0f; Bf[r*SF+c]=(float)((r+c)%7-3); }

  mark("=== 같은 바이너리에서 두 교대 구현 비교 (INT8 %d³ : FP32 %d³, best-of-5) ===", SI, SF);
  // 순서 효과를 배제하려고 두 번 교대로 잰다
  double ts1=timeit(seq);  int e0=chk();
  double th1=timeit(hand); int e1=chk();
  double tp1=timeit(prim); int e2=chk();
  double th2=timeit(hand);
  double tp2=timeit(prim);
  mark("순차            %8.3f ms   정확성 %s", ts1*1e3, e0?"FAIL":"PASS");
  mark("손으로 짠 루프  %8.3f / %8.3f ms   정확성 %s   속도 %.2f배", th1*1e3, th2*1e3, e1?"FAIL":"PASS", ts1/((th1<th2)?th1:th2));
  mark("일반화 primitive %7.3f / %8.3f ms   정확성 %s   속도 %.2f배", tp1*1e3, tp2*1e3, e2?"FAIL":"PASS", ts1/((tp1<tp2)?tp1:tp2));
  mark("=== MM2CMP_DONE ===");
  return 0;
}
