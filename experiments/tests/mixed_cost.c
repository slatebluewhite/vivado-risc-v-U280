// mixed_cost.c — 혼합 정밀도의 **진짜 비용**을 잰다 (E152). E148의 시간 측정을 대체한다.
//
// E148은 단계마다 double↔int8을 오갔는데, E151에서 그것이 피할 수 있는 비용임이
// 밝혀졌다(하드웨어 재양자화가 공짜). 그래서 E148의 시간 비교는 무효다.
// (정확도 결론 — 이상치가 전파돼 부분 혼합이 소용없다 — 은 수치 경로가 같으므로 유효하다.)
//
// 올바른 비교:
//   A) 순수 INT8 : 두 단 모두 INT8. **호스트 변환 없음** — int8이 그대로 흐른다.
//   C) 순수 FP32 : 두 단 모두 FP32. 역시 호스트 변환 없음.
//   B) 혼합      : 1단 FP32 → **경계에서 float→int8 변환(불가피)** → 2단 INT8.
//
// 혼합만이 경계 변환 비용을 낸다. 그 비용이 얼마인지가 이 실험의 답이다.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <sys/mman.h>
#include "include/gemmini_rt.h"

static const grt_ctx INT8 = { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 };
static const grt_ctx FP32 = { .dim= 8, .elem_bytes=4, .acc_bytes=4, .opcode=GRT_OP_FP32 };

#define N 64
#define NE (N*N)
static int8_t Xi[NE], W1i[NE], W2i[NE], Hi[NE], Yi[NE] __attribute__((aligned(64)));
static float  Xf[NE], W1f[NE], W2f[NE], Hf[NE], Yf[NE] __attribute__((aligned(64)));
static volatile int64_t sink;

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }

// A) 순수 INT8 — 하드웨어가 재양자화하므로 호스트는 아무것도 안 한다
static void path_int8(void){
  grt_matmul(&INT8, Xi, W1i, Hi, N,N,N);
  grt_matmul(&INT8, Hi, W2i, Yi, N,N,N);
}
// C) 순수 FP32
static void path_fp32(void){
  grt_matmul(&FP32, Xf, W1f, Hf, N,N,N);
  grt_matmul(&FP32, Hf, W2f, Yf, N,N,N);
}
// B) 혼합 — 경계에서 float→int8 변환이 불가피하다
static void path_mixed(void){
  grt_matmul(&FP32, Xf, W1f, Hf, N,N,N);
  int64_t chk=0;
  for (int i=0;i<NE;i++){
    long v = lrintf(Hf[i]*4.0f);            // 임의 스케일
    if(v>127)v=127; if(v<-128)v=-128; Hi[i]=(int8_t)v; chk+=v;
  }
  sink=chk;
  grt_matmul(&INT8, Hi, W2i, Yi, N,N,N);
}
// 경계 변환만 따로
static void conv_only(void){
  int64_t chk=0;
  for (int i=0;i<NE;i++){
    long v = lrintf(Hf[i]*4.0f);
    if(v>127)v=127; if(v<-128)v=-128; Hi[i]=(int8_t)v; chk+=v;
  }
  sink=chk;
}
static double timeit(void (*fn)(void), int it){
  double best=1e30;
  for(int s=0;s<3;s++){ double t0=now(); for(int i=0;i<it;i++) fn();
    double dt=(now()-t0)/it; if(dt<best)best=dt; }
  return best;
}

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/mixcost.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&INT8); grt_flush_ctx(&FP32);

  for (int i=0;i<NE;i++){
    Xi[i]=(int8_t)(i%15-7); W1i[i]=(int8_t)(i%11-5); W2i[i]=(int8_t)(i%13-6);
    Xf[i]=(float)(i%15-7);  W1f[i]=(float)(i%11-5);  W2f[i]=(float)(i%13-6);
    Hf[i]=(float)(i%9-4);
  }

  mark("=== 혼합 정밀도의 진짜 비용 (N=%d, 2단, 하드웨어 재양자화) ===", N);
  double ta=timeit(path_int8,10), tb=timeit(path_mixed,10), tc=timeit(path_fp32,10);
  double tconv=timeit(conv_only,10);
  mark("A) 순수 INT8  (호스트 변환 없음)   %8.3f ms", ta*1e3);
  mark("B) 혼합 FP32→INT8 (경계 변환 있음) %8.3f ms   A의 %.2f배", tb*1e3, tb/ta);
  mark("C) 순수 FP32  (호스트 변환 없음)   %8.3f ms   A의 %.2f배", tc*1e3, tc/ta);
  mark("   경계 변환만                     %8.3f ms   B의 %.0f%%", tconv*1e3, tconv/tb*100.0);
  mark("(체크섬 %lld)", (long long)sink);
  mark("=== MIXCOST_DONE ===");
  return 0;
}
