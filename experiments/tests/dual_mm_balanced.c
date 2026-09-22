// dual_mm.c — 동시 가동을 **실제 단위 작업(matmul 전체)**에서 검증한다 (E161).
//
// E156~E160은 합성 타일 루프로 중첩을 보였다. 실용적으로 의미가 있으려면
// **matmul 하나** 단위에서 이득이 나야 한다. `grt_matmul2`가 두 워크로드의 타일을
// 교차 발행한다.
//
// 비교: 순차(각각 grt_matmul) vs 교차(grt_matmul2). 총 연산량 동일.
// 정확성: A를 단위행렬로 두어 C == B 를 양쪽에서 확인한다.
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

// 타일 연산 수를 맞춘다: INT8(dim=16) SI^3/16^3 = FP32(dim=8) SF^3/8^3
// SI=128 → 512타일, SF=64 → 512타일. 같은 크기를 주면 FP32가 8배 일한다.
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
static void clear(void){ memset(Ci,0,sizeof(Ci)); memset(Cf,0,sizeof(Cf)); }

static void seq(void){
  grt_matmul(&INT8, Ai, Bi, Ci, SI,SI,SI);
  grt_matmul(&FP32, Af, Bf, Cf, SF,SF,SF);
}
// 두 워크로드의 형상이 다르므로 각자의 좌표를 돈다 — grt_matmul2는 같은 M,N,K를 받으므로
// 여기서는 타일 교차를 직접 쓴다(두 순회를 하나의 루프에서 번갈아 진행).
static void dual(void){
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
static int chk(void){
  int b=0;
  for(int i=0;i<SI*SI;i++) if(Ci[i]!=Bi[i]) b++;
  for(int i=0;i<SF*SF;i++) if(Cf[i]!=Bf[i]) b++;
  return b;
}
// 잡음이 ~3%p이므로(E160) 세트를 늘린다: best-of-5
static double timeit(void (*fn)(void)){
  double best=1e30;
  for(int s=0;s<5;s++){ clear(); double t0=now(); fn(); double dt=now()-t0; if(dt<best)best=dt; }
  return best;
}

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/dual.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&INT8); grt_flush_ctx(&FP32);
  for(int r=0;r<SI;r++) for(int c=0;c<SI;c++){ Ai[r*SI+c]=(r==c)?1:0; Bi[r*SI+c]=(int8_t)((r+c)%7-3); }
  for(int r=0;r<SF;r++) for(int c=0;c<SF;c++){ Af[r*SF+c]=(r==c)?1.0f:0.0f; Bf[r*SF+c]=(float)((r+c)%7-3); }

  mark("=== 타일 수를 맞춘 동시 가동 (cpu=%d, INT8 %d^3=%d타일 : FP32 %d^3=%d타일, best-of-5) ===",
       sched_getcpu(), SI, (SI/16)*(SI/16)*(SI/16), SF, (SF/8)*(SF/8)*(SF/8));
  double ts=timeit(seq);  int bs=chk();
  double td=timeit(dual); int bd=chk();
  mark("순차 (INT8 → FP32)   %8.3f ms   정확성 %s", ts*1e3, bs?"FAIL":"PASS");
  mark("교차 (타일 단위)     %8.3f ms   정확성 %s", td*1e3, bd?"FAIL":"PASS");
  mark("→ 절감 %.1f%%  (속도 %.2f배)", (1.0-td/ts)*100.0, ts/td);
  mark("=== DUAL_DONE ===");
  return (bs||bd)?1:0;
}
