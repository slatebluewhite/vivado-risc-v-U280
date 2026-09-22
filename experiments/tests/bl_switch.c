// bl_switch.c — 두 가속기를 **번갈아 쓰는 비용**을 잰다 (E140).
//
// 왜 필요한가:
//   E139에서 어느 가속기가 유리한지가 행렬 크기에 따라 뒤집힌다는 것이 나왔다.
//   그래서 "런타임에 고른다"가 의미를 갖는데, **고르는 데 비용이 든다면** 자주 바꾸는
//   스케줄은 불리해진다. 그 비용을 아직 아무도 재지 않았다.
//
// 방법: 총 작업량을 동일하게 두고 순서만 바꾼다.
//   묶음 방식: INT8 N회 → FP32 N회   (전환 1회)
//   교대 방식: INT8, FP32, INT8, ... (전환 2N-1회)
//   두 시간의 차이가 전환 비용이다. 연산량·데이터·코어·클럭이 모두 같다.
//
// 전환 비용이 있다면 어디서 오는가: 가속기마다 config(ld/st/ex)를 다시 실어야 하고,
// 상대 가속기의 스크래치패드 상태는 남아 있어도 쓸 수 없다.
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

#define SZ 64
#define REP 10
static uint8_t A8[SZ*SZ], B8[SZ*SZ], C8[SZ*SZ] __attribute__((aligned(64)));
static uint8_t Af[SZ*SZ*4], Bf[SZ*SZ*4], Cf[SZ*SZ*4] __attribute__((aligned(64)));

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"\n"); fflush(g); fsync(fileno(g));
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec + t.tv_nsec*1e-9; }

static void do_int8(void){ grt_matmul(&INT8, A8, B8, C8, SZ, SZ, SZ); }
static void do_fp32(void){ grt_matmul(&FP32, Af, Bf, Cf, SZ, SZ, SZ); }

int main(int argc, char **argv){
  g = fopen(argc>1?argv[1]:"/mnt2/tmp/bl_switch.log","w");
  if(!g){ perror("로그"); exit(1); }
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  cpu_set_t s; CPU_ZERO(&s); CPU_SET(0,&s); sched_setaffinity(0,sizeof(s),&s); sched_yield();

  for (int i=0;i<SZ;i++) for(int j=0;j<SZ;j++){
    ((int8_t*)A8)[i*SZ+j]=(i==j)?1:0;  ((int8_t*)B8)[i*SZ+j]=(int8_t)((i+j)%5-2);
    ((float*)Af)[i*SZ+j]=(i==j)?1.0f:0.0f; ((float*)Bf)[i*SZ+j]=(float)((i+j)%5-2);
  }
  grt_flush_ctx(&INT8); grt_flush_ctx(&FP32);

  mark("=== 전환 비용 측정 (cpu=%d, %d^3, 각 %d회) ===", sched_getcpu(), SZ, REP);
  mark("총 작업량 동일, 순서만 다름. 묶음=전환 1회, 교대=전환 %d회", 2*REP-1);

  double best_batch=1e30, best_alt=1e30;
  for (int set=0; set<3; set++){
    // 묶음: INT8 전부 → FP32 전부
    double t0=now();
    for (int i=0;i<REP;i++) do_int8();
    for (int i=0;i<REP;i++) do_fp32();
    double tb=now()-t0; if (tb<best_batch) best_batch=tb;

    // 교대: 하나씩 번갈아
    t0=now();
    for (int i=0;i<REP;i++){ do_int8(); do_fp32(); }
    double ta=now()-t0; if (ta<best_alt) best_alt=ta;
  }

  // 정확성도 확인 — 번갈아 쓴 뒤에도 두 결과가 맞아야 한다
  int bad8=0, badf=0;
  for (int i=0;i<SZ;i++) for(int j=0;j<SZ;j++){
    if (((int8_t*)C8)[i*SZ+j] != ((int8_t*)B8)[i*SZ+j]) bad8++;
    if (((float*)Cf)[i*SZ+j]  != ((float*)Bf)[i*SZ+j])  badf++;
  }
  mark("정확성: INT8 %s (%d/%d), FP32 %s (%d/%d)",
       bad8?"FAIL":"PASS", bad8, SZ*SZ, badf?"FAIL":"PASS", badf, SZ*SZ);

  mark("묶음 %.3f ms   교대 %.3f ms   차이 %+.3f ms (%+.2f%%)",
       best_batch*1e3, best_alt*1e3, (best_alt-best_batch)*1e3,
       (best_alt/best_batch-1.0)*100.0);
  mark("→ 전환 1회당 %+.1f us", (best_alt-best_batch)*1e6/(2.0*REP-2.0));
  mark("=== SWITCH_DONE ===");
  return 0;
}
