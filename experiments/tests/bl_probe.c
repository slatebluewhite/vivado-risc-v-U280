// bl_probe.c — big.LITTLE 구성(Rocket64b2gembl) 검증.
//
// 코어0에 가속기가 둘 붙어 있고 opcode로 갈린다: INT8=custom3, FP32=custom2.
// 코어1에는 가속기가 없다.
//
// **한 바이너리가 한 코어에서 두 가속기를 번갈아 쓴다** — 이게 이 트랙의 목표였다.
// 코어 간 태스크 패싱이 필요 없고, 헤더 재빌드도 필요 없다.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>
#include "include/gemmini_rt.h"

static const grt_ctx INT8 = { .dim=16, .elem_bytes=1, .acc_bytes=4, .opcode=GRT_OP_INT8 };
static const grt_ctx FP32 = { .dim= 8, .elem_bytes=4, .acc_bytes=4, .opcode=GRT_OP_FP32 };

static FILE *g;
static void mark(const char *fmt, ...){
  if(!g) return; va_list ap; va_start(ap,fmt); vfprintf(g,fmt,ap); va_end(ap);
  fprintf(g,"  (cpu=%d)\n", sched_getcpu()); fflush(g); fsync(fileno(g));
}
static void pin(int c){ cpu_set_t s; CPU_ZERO(&s); CPU_SET(c,&s);
  sched_setaffinity(0,sizeof(s),&s); sched_yield(); }

static uint8_t A[16*16*4] __attribute__((aligned(64)));
static uint8_t B[16*16*4] __attribute__((aligned(64)));
static uint8_t C[16*16*4] __attribute__((aligned(64)));

// 단위행렬 × 3 을 한 타일 계산하고 CPU 기대값과 대조한다
static int one_tile(const grt_ctx *c, int is_float, const char *tag){
  int d = c->dim;
  for (int i=0;i<d;i++) for(int j=0;j<d;j++){
    if (is_float){ ((float*)A)[i*d+j]=(i==j)?1.0f:0.0f; ((float*)B)[i*d+j]=3.0f; }
    else         { ((int8_t*)A)[i*d+j]=(i==j)?1:0;      ((int8_t*)B)[i*d+j]=3;   }
  }
  memset(C,0,sizeof(C));

  mark("%s: flush 직전 (opcode=custom%d)", tag, c->opcode);
  grt_flush_ctx(c);
  mark("%s: flush 통과", tag);

  grt_config_ld(c, (uint64_t)d*c->elem_bytes);
  grt_config_st(c, (uint64_t)d*c->elem_bytes);
  grt_config_ex(c, GRT_WS);
  mark("%s: config 3종 통과", tag);

  grt_mvin(c, A, 0, d, d);
  grt_mvin(c, B, d, d, d);
  grt_fence();
  mark("%s: mvin 통과", tag);

  grt_preload(c, d, GRT_ACC(0), d, d, d, d);
  grt_compute(c, 0, GRT_GARBAGE, d, d, d, d);
  grt_fence();
  grt_mvout(c, C, GRT_ACC(0), d, d);
  grt_fence();

  int bad=0;
  for (int i=0;i<d;i++) for(int j=0;j<d;j++){
    double v = is_float ? ((float*)C)[i*d+j] : (double)((int8_t*)C)[i*d+j];
    if (v != 3.0) bad++;
  }
  double first = is_float ? ((float*)C)[0] : (double)((int8_t*)C)[0];
  mark("%s: mvout 통과, C[0][0]=%.1f (기대 3.0), 오류 %d/%d → %s",
       tag, first, bad, d*d, bad?"FAIL":"PASS");
  return bad;
}

int main(int argc, char **argv){
  g = fopen(argc>1?argv[1]:"/mnt2/tmp/bl_probe.log","w");
  if(!g){ perror("로그"); exit(1); }
  if (mlockall(MCL_CURRENT|MCL_FUTURE)!=0) mark("mlockall 실패(계속)");
  mark("=== big.LITTLE 검증 시작 ===");

  pin(0);
  int b1 = one_tile(&INT8, 0, "코어0/INT8");
  int b2 = one_tile(&FP32, 1, "코어0/FP32");
  int b3 = one_tile(&INT8, 0, "코어0/INT8재");   // 되돌아오기

  mark("=== 결과: %s ===", (b1||b2||b3) ? "FAIL" : "PASS");
  return (b1||b2||b3)?1:0;
}
