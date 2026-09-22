// het_mm.c — 런타임 라이브러리(gemmini_rt.h)로 두 가속기에서 **같은 코드**로 matmul.
//
// 목적 둘:
//   (1) 정확성 — CPU 참조와 대조
//   (2) 성능   — 같은 코드·같은 형상으로 INT8 16x16 대 FP32 8x8을 비교
//
// 주의(JOURNAL 방법론): rdcycle은 유저모드에서 트랩하므로 clock_gettime을 쓴다.
//   측정은 "10회 이상 세트를 3벌 돌려 최선값"을 취한다.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <sys/mman.h>
#include "include/gemmini_rt.h"

static const grt_ctx CORE0 = { .dim = 16, .elem_bytes = 1, .acc_bytes = 4 };
static const grt_ctx CORE1 = { .dim =  8, .elem_bytes = 4, .acc_bytes = 4 };

#define SZ 64                      // M=N=K=64, 16과 8 모두의 배수
static uint8_t A[SZ*SZ*4] __attribute__((aligned(64)));
static uint8_t B[SZ*SZ*4] __attribute__((aligned(64)));
static uint8_t C[SZ*SZ*4] __attribute__((aligned(64)));

static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec + t.tv_nsec*1e-9; }

static void pin(int cpu){ cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu,&s);
  sched_setaffinity(0,sizeof(s),&s); sched_yield(); }

static int check(const grt_ctx *c, int is_float){
  // 작은 값만 써서 INT8 포화를 피한다 (K=64이므로 누적이 커진다)
  int bad = 0;
  for (int i=0;i<SZ;i++) for(int j=0;j<SZ;j++){
    double acc = 0;
    for (int k=0;k<SZ;k++){
      double a = is_float ? ((float*)A)[i*SZ+k] : (double)((int8_t*)A)[i*SZ+k];
      double b = is_float ? ((float*)B)[k*SZ+j] : (double)((int8_t*)B)[k*SZ+j];
      acc += a*b;
    }
    double got = is_float ? ((float*)C)[i*SZ+j] : (double)((int8_t*)C)[i*SZ+j];
    if (got != acc) { if (bad < 3)
        printf("    불일치 [%d][%d] got=%.1f want=%.1f\n", i, j, got, acc);
      bad++; }
  }
  (void)c; return bad;
}

static void fill(int is_float){
  for (int i=0;i<SZ;i++) for(int j=0;j<SZ;j++){
    if (is_float){ ((float*)A)[i*SZ+j] = (i==j)?1.0f:0.0f;
                   ((float*)B)[i*SZ+j] = (float)((i+j)%5) - 2.0f; }
    else         { ((int8_t*)A)[i*SZ+j] = (i==j)?1:0;
                   ((int8_t*)B)[i*SZ+j] = (int8_t)((i+j)%5) - 2; }
  }
  memset(C,0,sizeof(C));
}

static void bench(const grt_ctx *c, int is_float, const char *tag){
  fill(is_float);
  grt_flush();
  grt_matmul(c, A, B, C, SZ, SZ, SZ);
  int bad = check(c, is_float);
  printf("%-7s cpu=%d dim=%d elem=%dB  정확성 %s (%d/%d 불일치)\n",
         tag, sched_getcpu(), c->dim, c->elem_bytes, bad?"FAIL":"PASS", bad, SZ*SZ);
  fflush(stdout);
  if (bad) return;

  double best = 1e30;
  for (int set=0; set<3; set++){
    double t0 = now();
    for (int it=0; it<10; it++) grt_matmul(c, A, B, C, SZ, SZ, SZ);
    double dt = (now()-t0)/10.0;
    if (dt < best) best = dt;
  }
  double macs = (double)SZ*SZ*SZ;
  printf("%-7s  %.3f ms/matmul, %.1f MMAC/s, PE=%d\n",
         tag, best*1e3, macs/best/1e6, c->dim*c->dim);
  fflush(stdout);
}

int main(){
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0){ perror("mlockall"); exit(1); }
  printf("=== 런타임 라이브러리 matmul %dx%dx%d ===\n", SZ,SZ,SZ); fflush(stdout);
  pin(0); bench(&CORE0, 0, "INT8");
  pin(1); bench(&CORE1, 1, "FP32");
  printf("=== HET_MM_DONE ===\n"); fflush(stdout);
  return 0;
}
