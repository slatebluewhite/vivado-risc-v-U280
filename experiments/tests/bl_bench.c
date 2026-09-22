// bl_bench.c — big.LITTLE 구성에서 **한 코어·한 바이너리·같은 코드**로 두 가속기를 비교한다.
//
// 이 비교가 왜 의미 있나(JOURNAL 방법론):
//   E119→E120에서, 손으로 쓴 FP32 층과 번들 INT8 벤치마크를 비교했더니 3.13배가 나왔고
//   같은 코드로 맞춰 재니 5.12배였다. **같은 코드로 재지 않으면 비교가 성립하지 않는다.**
//   지금까지는 헤더가 컴파일 타임에 고정돼 구성마다 다른 바이너리를 써야 했으므로
//   이 통제가 원천적으로 불가능했다. 런타임 라이브러리가 그것을 가능하게 한다.
//
//   게다가 여기서는 **같은 코어, 같은 클럭, 같은 캐시 상태**다. 남는 변수가 가속기뿐이다.
//
// 측정 규칙(E113~E116에서 비싸게 배운 것): 최소 10회 세트를 3벌 돌려 최선값.
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

#define SZ 64                       // 16과 8 모두의 배수
static uint8_t A[SZ*SZ*4] __attribute__((aligned(64)));
static uint8_t B[SZ*SZ*4] __attribute__((aligned(64)));
static uint8_t C[SZ*SZ*4] __attribute__((aligned(64)));

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec + t.tv_nsec*1e-9; }

static void fill(int is_float){
  for (int i=0;i<SZ;i++) for(int j=0;j<SZ;j++){
    if (is_float){ ((float*)A)[i*SZ+j]=(i==j)?1.0f:0.0f;
                   ((float*)B)[i*SZ+j]=(float)((i+j)%5)-2.0f; }
    else         { ((int8_t*)A)[i*SZ+j]=(i==j)?1:0;
                   ((int8_t*)B)[i*SZ+j]=(int8_t)((i+j)%5)-2; }
  }
  memset(C,0,sizeof(C));
}

// A가 단위행렬이므로 C는 B와 같아야 한다 — 정확성을 먼저 통과해야 측정이 의미가 있다
static int verify(int is_float){
  int bad=0;
  for (int i=0;i<SZ;i++) for(int j=0;j<SZ;j++){
    double want = is_float ? (double)((float*)B)[i*SZ+j] : (double)((int8_t*)B)[i*SZ+j];
    double got  = is_float ? (double)((float*)C)[i*SZ+j] : (double)((int8_t*)C)[i*SZ+j];
    if (got != want) bad++;
  }
  return bad;
}

static void bench(const grt_ctx *c, int is_float, const char *tag){
  fill(is_float);
  grt_flush_ctx(c);
  grt_matmul(c, A, B, C, SZ, SZ, SZ);
  int bad = verify(is_float);
  mark("%-5s dim=%2d elem=%dB opcode=custom%d  정확성 %s (%d/%d)",
       tag, c->dim, c->elem_bytes, c->opcode, bad?"FAIL":"PASS", bad, SZ*SZ);
  if (bad) return;

  double best = 1e30;
  for (int set=0; set<3; set++){
    double t0 = now();
    for (int it=0; it<10; it++) grt_matmul(c, A, B, C, SZ, SZ, SZ);
    double dt = (now()-t0)/10.0;
    if (dt < best) best = dt;
  }
  double macs = (double)SZ*SZ*SZ;
  mark("%-5s  %.3f ms/matmul   %.1f MMAC/s   PE=%d   (best-of-3 x10)",
       tag, best*1e3, macs/best/1e6, c->dim*c->dim);
}

int main(int argc, char **argv){
  g = fopen(argc>1?argv[1]:"/mnt2/tmp/bl_bench.log","w");
  if(!g){ perror("로그"); exit(1); }
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");

  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s);
  sched_setaffinity(0,sizeof(s),&s); sched_yield();

  mark("=== 같은 코어(cpu%d)·같은 코드로 두 가속기 비교, %dx%dx%d ===",
       sched_getcpu(), SZ, SZ, SZ);
  bench(&INT8, 0, "INT8");
  bench(&FP32, 1, "FP32");
  mark("=== BL_BENCH_DONE ===");
  return 0;
}
