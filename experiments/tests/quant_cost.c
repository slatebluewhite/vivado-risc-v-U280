// quant_cost.c — E148의 결론을 성분별로 분해한다 (E149).
//
// E148에서 2단 INT8 파이프라인이 58.7 ms, FP32가 24.0 ms로 나왔고
// "소프트웨어 양자화가 matmul 이득을 삼킨다"고 결론지었다. 그런데 내 변환 코드는
// **double을 거쳐** 양자화한다(`lrint(x/scale)`). 실제 양자화 추론은 그렇게 하지 않는다 —
// int32 누산값을 **정수 곱·시프트**로 재양자화한다.
//
// 즉 E148이 잰 것은 "양자화의 본질적 비용"이 아니라 "내 double 경유 구현의 비용"일 수 있다.
// 성분별로 분해해 확인한다:
//   (1) matmul만 (이미 양자화된 데이터)
//   (2) double→int8 양자화 (E148이 쓴 방식)
//   (3) int32→int8 재양자화, 정수 곱·시프트 (실제 파이프라인 방식)
//   (4) double→float 변환 (FP32 경로의 비용)
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

#define N 64
#define NE (N*N)
static double Ad[NE], Bd[NE];
static int32_t Acc[NE];
static uint8_t q1[NE], q2[NE], q3[NE*4] __attribute__((aligned(64)));
static float f1[NE], f2[NE];

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec*1e-9; }
static unsigned long rs=12345;
static double nextv(void){ rs=rs*6364136223846793005UL+1442695040888963407UL;
  return ((double)((rs>>33)&0xFFFFFF)/8388608.0)-1.0; }

// (2) E148 방식: double 나눗셈 + lrint + 클램프
static void quant_double(void){
  double s = 1.0/127.0;
  for (int i=0;i<NE;i++){
    long r = lrint(Ad[i]/s); if(r>127)r=127; if(r<-128)r=-128; ((int8_t*)q1)[i]=(int8_t)r;
    long t = lrint(Bd[i]/s); if(t>127)t=127; if(t<-128)t=-128; ((int8_t*)q2)[i]=(int8_t)t;
  }
}
// (3) 실제 파이프라인 방식: int32 누산값을 정수 곱·시프트로 재양자화
static void requant_int(void){
  const int32_t mult = 1518500250;   // 임의의 고정소수 배율
  const int shift = 31;
  for (int i=0;i<NE;i++){
    int64_t v = ((int64_t)Acc[i]*mult) >> shift;
    if(v>127)v=127; if(v<-128)v=-128; ((int8_t*)q1)[i]=(int8_t)v;
  }
}
// (4) FP32 경로의 변환 비용
static void conv_float(void){
  for (int i=0;i<NE;i++){ f1[i]=(float)Ad[i]; f2[i]=(float)Bd[i]; }
}

static double timeit(void (*fn)(void), int iters){
  double best=1e30;
  for (int s=0;s<3;s++){
    double t0=now(); for(int i=0;i<iters;i++) fn();
    double dt=(now()-t0)/iters; if(dt<best) best=dt;
  }
  return best;
}
static void mm(void){ grt_matmul(&INT8, q1, q2, q3, N, N, N); }

int main(int argc, char **argv){
  g=fopen(argc>1?argv[1]:"/mnt2/tmp/quant.log","w");
  if(!g){perror("로그");exit(1);}
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&INT8);

  for (int i=0;i<NE;i++){ Ad[i]=nextv(); Bd[i]=nextv(); Acc[i]=(int32_t)(nextv()*100000); }
  quant_double();

  mark("=== 양자화 비용 분해 (cpu=%d, N=%d, 원소 %d개) ===", sched_getcpu(), N, NE);
  double t_mm = timeit(mm, 10);
  double t_qd = timeit(quant_double, 10);
  double t_ri = timeit(requant_int, 10);
  double t_cf = timeit(conv_float, 10);

  mark("(1) INT8 matmul (양자화 완료 데이터)   %8.3f ms", t_mm*1e3);
  mark("(2) double→int8 양자화 2개 [E148 방식] %8.3f ms   matmul의 %.1f배", t_qd*1e3, t_qd/t_mm);
  mark("(3) int32→int8 재양자화 [실제 방식]    %8.3f ms   matmul의 %.2f배", t_ri*1e3, t_ri/t_mm);
  mark("(4) double→float 변환 2개 [FP32 경로]  %8.3f ms   matmul의 %.2f배", t_cf*1e3, t_cf/t_mm);
  mark("→ (2)가 (3)보다 %.1f배 비싸다", t_qd/t_ri);
  mark("=== QUANT_DONE ===");
  return 0;
}
