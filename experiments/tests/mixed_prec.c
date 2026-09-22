// mixed_prec.c — **혼합 정밀도의 실익**을 잰다 (E148).
//
// 앞선 두 결과를 합치면 검증 가능한 주장이 나온다:
//   E143: 이상치가 있으면 텐서 단위 스케일 INT8이 값의 대부분을 0으로 파괴한다
//   E140: 두 가속기 사이 전환 비용은 측정 한계 아래(< 7 us)다
//   → **이상치가 있는 연산만 FP32로 돌리면 정확도를 회복하면서 속도는 거의 지킨다**
//
// 2단 파이프라인으로 시험한다 (LLM의 활성화 이상치 상황을 흉내낸다):
//   H = X * W1   ← X에 이상치가 있다 (1%를 100배)
//   Y = H * W2   ← 평범하다
//
// 세 경로를 같은 코어·같은 바이너리로 비교한다:
//   A) 둘 다 INT8      — 가장 빠르지만 이상치에 취약
//   B) 1단 FP32 + 2단 INT8 — 혼합 (전환 1회)
//   C) 둘 다 FP32      — 가장 정확하지만 느리다
//
// 예측: B가 C의 정확도에 근접하면서 A에 가까운 속도를 낸다.
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
static double Xd[N*N], W1d[N*N], W2d[N*N], Hd[N*N], Yd[N*N];   // 오라클
static uint8_t buf1[N*N*4], buf2[N*N*4], buf3[N*N*4] __attribute__((aligned(64)));
static double Hcur[N*N], Ycur[N*N];

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec + t.tv_nsec*1e-9; }
static unsigned long rs;
static double nextv(void){ rs = rs*6364136223846793005UL + 1442695040888963407UL;
  return ((double)((rs>>33)&0xFFFFFF)/8388608.0) - 1.0; }

// 한 단계를 지정한 가속기로 수행한다. 입력/출력은 double 배열이고,
// INT8이면 대칭 양자화·역양자화를 거친다 (E142와 같은 방식).
static void stage(const grt_ctx *c, const double *A, const double *B, double *C){
  int is_int = (c->elem_bytes == 1);
  double amax=0,bmax=0;
  for (int i=0;i<N*N;i++){ if(fabs(A[i])>amax)amax=fabs(A[i]); if(fabs(B[i])>bmax)bmax=fabs(B[i]); }
  double sa = amax/127.0, sb = bmax/127.0;
  if (sa==0) sa=1; if (sb==0) sb=1;

  for (int i=0;i<N*N;i++){
    if (is_int){
      long ra=lrint(A[i]/sa), rb=lrint(B[i]/sb);
      if(ra>127)ra=127; if(ra<-128)ra=-128;
      if(rb>127)rb=127; if(rb<-128)rb=-128;
      ((int8_t*)buf1)[i]=(int8_t)ra; ((int8_t*)buf2)[i]=(int8_t)rb;
    } else {
      ((float*)buf1)[i]=(float)A[i]; ((float*)buf2)[i]=(float)B[i];
    }
  }
  memset(buf3,0,sizeof(buf3));
  grt_matmul(c, buf1, buf2, buf3, N, N, N);
  for (int i=0;i<N*N;i++)
    C[i] = is_int ? (double)((int8_t*)buf3)[i]*sa*sb : (double)((float*)buf3)[i];
}

static void run_path(const grt_ctx *s1, const grt_ctx *s2){
  stage(s1, Xd,   W1d, Hcur);
  stage(s2, Hcur, W2d, Ycur);
}

static double err_vs_oracle(void){
  double rms=0; for(int i=0;i<N*N;i++) rms += Yd[i]*Yd[i];
  rms = sqrt(rms/(N*N)); if (rms<1e-12) rms=1e-12;
  double sum=0; for(int i=0;i<N*N;i++) sum += fabs(Ycur[i]-Yd[i]);
  return sum/(N*N)/rms*100.0;
}

static void measure(const grt_ctx *s1, const grt_ctx *s2, const char *tag){
  run_path(s1,s2);
  double e = err_vs_oracle();
  double best=1e30;
  for (int set=0;set<3;set++){
    double t0=now(); for(int it=0;it<10;it++) run_path(s1,s2);
    double dt=(now()-t0)/10.0; if(dt<best) best=dt;
  }
  mark("%-22s %8.3f ms   오차 %8.3f%%", tag, best*1e3, e);
}

int main(int argc, char **argv){
  g = fopen(argc>1?argv[1]:"/mnt2/tmp/mixed.log","w");
  if(!g){ perror("로그"); exit(1); }
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();
  grt_flush_ctx(&INT8); grt_flush_ctx(&FP32);

  rs = 12345;
  for (int i=0;i<N*N;i++) Xd[i]=nextv();
  for (int i=0;i<N*N;i++) W1d[i]=nextv();
  for (int i=0;i<N*N;i++) W2d[i]=nextv();
  for (int i=0;i<N*N;i+=100) Xd[i]*=100.0;        // ★ 1%를 100배 — 이상치

  // 오라클 (double)
  for (int i=0;i<N;i++) for(int j=0;j<N;j++){
    double a=0; for(int k=0;k<N;k++) a += Xd[i*N+k]*W1d[k*N+j]; Hd[i*N+j]=a; }
  for (int i=0;i<N;i++) for(int j=0;j<N;j++){
    double a=0; for(int k=0;k<N;k++) a += Hd[i*N+k]*W2d[k*N+j]; Yd[i*N+j]=a; }

  mark("=== 혼합 정밀도 실익 (cpu=%d, %d^3 2단, X의 1%%가 100배 이상치) ===", sched_getcpu(), N);
  mark("%-22s %8s   %s", "경로", "시간", "오차(오라클 대비)");
  measure(&INT8,&INT8,"A) INT8 + INT8");
  measure(&FP32,&INT8,"B) FP32 + INT8 (혼합)");
  measure(&FP32,&FP32,"C) FP32 + FP32");
  mark("=== MIXED_DONE ===");
  return 0;
}
